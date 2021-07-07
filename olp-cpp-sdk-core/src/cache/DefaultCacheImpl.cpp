/*
 * Copyright (C) 2019-2020 HERE Europe B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */

#include "DefaultCacheImpl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "olp/core/logging/Log.h"
#include "olp/core/porting/make_unique.h"
#include "olp/core/utils/Dir.h"

namespace {
using CacheType = olp::cache::DefaultCache::CacheType;
using StorageOpenResult = olp::cache::DefaultCache::StorageOpenResult;

constexpr auto kLogTag = "DefaultCache";
constexpr auto kExpirySuffix = "::expiry";
constexpr auto kProtectedKeys = "internal::protected::protected_data";
constexpr auto kInternalKeysPrefix = "internal::";
constexpr auto kMaxDiskSize = std::uint64_t(-1);
constexpr auto kMinDiskUsedThreshold = 0.85f;
constexpr auto kMaxDiskUsedThreshold = 0.9f;
constexpr auto kEvictionPortion = 1024u * 1024u;  // 1 MB

// current epoch time contains 10 digits.
constexpr auto kExpiryValueSize = 10;
const auto kExpirySuffixLength = strlen(kExpirySuffix);

std::string CreateExpiryKey(const std::string& key) {
  return key + kExpirySuffix;
}

bool IsExpiryKey(const std::string& key) {
  return key.rfind(kExpirySuffix) != std::string::npos;
}

bool IsExpiryValid(time_t expiry) {
  return expiry < olp::cache::KeyValueCache::kDefaultExpiry;
}

time_t GetRemainingExpiryTime(const std::string& key,
                              olp::cache::DiskCache& disk_cache) {
  auto expiry_key = CreateExpiryKey(key);
  auto expiry = olp::cache::KeyValueCache::kDefaultExpiry;
  auto expiry_value = disk_cache.Get(expiry_key);
  if (expiry_value) {
    expiry = std::stol(*expiry_value);
    expiry -= olp::cache::InMemoryCache::DefaultTimeProvider()();
  }

  return expiry;
}

void PurgeDiskItem(const std::string& key, olp::cache::DiskCache& disk_cache,
                   uint64_t& removed_data_size) {
  auto expiry_key = CreateExpiryKey(key);
  uint64_t data_size = 0u;

  disk_cache.Remove(key, data_size);
  removed_data_size += data_size;

  disk_cache.Remove(expiry_key, data_size);
  removed_data_size += data_size;
}

size_t StoreExpiry(const std::string& key, leveldb::WriteBatch& batch,
                   time_t expiry) {
  auto expiry_key = CreateExpiryKey(key);
  auto time_str = std::to_string(expiry);
  batch.Put(expiry_key, time_str);

  return expiry_key.size() + time_str.size();
}

leveldb::CompressionType GetCompression(
    olp::cache::CompressionType compression) {
  return (compression == olp::cache::CompressionType::kNoCompression)
             ? leveldb::kNoCompression
             : leveldb::kSnappyCompression;
}

olp::cache::StorageSettings CreateStorageSettings(
    const olp::cache::CacheSettings& settings) {
  olp::cache::StorageSettings storage_settings;
  storage_settings.max_disk_storage = settings.max_disk_storage;
  storage_settings.max_chunk_size = settings.max_chunk_size;
  storage_settings.enforce_immediate_flush = settings.enforce_immediate_flush;
  storage_settings.max_file_size = settings.max_file_size;
  storage_settings.compression = GetCompression(settings.compression);

  return storage_settings;
}

int64_t GetElapsedTime(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

bool IsInternalKey(const std::string& key) {
  return key.find(kInternalKeysPrefix) == 0u;
}

olp::cache::DefaultCache::StorageOpenResult ToStorageOpenResult(
    olp::cache::OpenResult input) {
  switch (input) {
    case olp::cache::OpenResult::Fail:
      return StorageOpenResult::OpenDiskPathFailure;
    case olp::cache::OpenResult::Corrupted:
      return StorageOpenResult::ProtectedCacheCorrupted;
    case olp::cache::OpenResult::Repaired:
    case olp::cache::OpenResult::Success:
      return StorageOpenResult::Success;
  }

  return {};
}
}  // namespace

namespace olp {
namespace cache {

DefaultCacheImpl::DefaultCacheImpl(CacheSettings settings)
    : settings_(std::move(settings)),
      is_open_(false),
      memory_cache_(nullptr),
      mutable_cache_(nullptr),
      mutable_cache_lru_(nullptr),
      protected_cache_(nullptr),
      mutable_cache_data_size_(0),
      eviction_portion_(kEvictionPortion) {}

DefaultCache::StorageOpenResult DefaultCacheImpl::Open() {
  std::lock_guard<std::mutex> lock(cache_lock_);
  is_open_ = true;
  return SetupStorage();
}

DefaultCache::StorageOpenResult DefaultCacheImpl::Open(
    DefaultCache::CacheType type) {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!is_open_) {
    return DefaultCache::NotReady;
  }

  if (type == DefaultCache::CacheType::kMutable) {
    if (mutable_cache_) {
      return DefaultCache::Success;
    }

    if (!settings_.disk_path_mutable) {
      OLP_SDK_LOG_ERROR_F(kLogTag,
                          "Failed to open the mutable cache - path is empty");
      return DefaultCache::OpenDiskPathFailure;
    }

    if (memory_cache_) {
      memory_cache_->Clear();
    }

    return SetupMutableCache();
  }

  // DefaultCache::CacheType::kProtected case
  if (protected_cache_) {
    return DefaultCache::Success;
  }

  if (!settings_.disk_path_protected) {
    OLP_SDK_LOG_ERROR_F(kLogTag,
                        "Failed to open the protected cache - path is empty");
    return DefaultCache::OpenDiskPathFailure;
  }

  if (memory_cache_) {
    memory_cache_->Clear();
  }

  return SetupProtectedCache();
}

DefaultCacheImpl::~DefaultCacheImpl() { Close(); }

void DefaultCacheImpl::Close() {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!is_open_) {
    return;
  }

  memory_cache_.reset();
  DestroyCache(DefaultCache::CacheType::kMutable);
  DestroyCache(DefaultCache::CacheType::kProtected);
  is_open_ = false;
}

bool DefaultCacheImpl::Close(DefaultCache::CacheType type) {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!is_open_) {
    return false;
  }

  DestroyCache(type);

  return true;
}

bool DefaultCacheImpl::Clear() {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!is_open_) {
    return false;
  }

  if (memory_cache_) {
    memory_cache_->Clear();
  }

  if (mutable_cache_lru_) {
    mutable_cache_lru_->Clear();
  }

  if (mutable_cache_) {
    mutable_cache_data_size_ = 0;
    if (!mutable_cache_->Clear()) {
      return false;
    }
  }

  return SetupStorage() == DefaultCache::StorageOpenResult::Success;
}

void DefaultCacheImpl::Compact() {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (mutable_cache_) {
    mutable_cache_->Compact();
  }
}

bool DefaultCacheImpl::Put(const std::string& key, const boost::any& value,
                           const Encoder& encoder, time_t expiry) {
  OLP_SDK_LOG_DEBUG_F(kLogTag, "Put(encoder), key=%s", key.c_str());
  std::lock_guard<std::mutex> lock(cache_lock_);
  OLP_SDK_LOG_DEBUG_F(kLogTag, "Put(encoder) mutex locked, key=%s", key.c_str());

  if (!is_open_) {
    OLP_SDK_LOG_DEBUG_F(kLogTag, "Put(encoder), cache is not opened, key=%s", key.c_str());

    return false;
  }

  auto encoded_item = encoder();
  if (memory_cache_) {
    const auto size = encoded_item.size();
    const bool result = memory_cache_->Put(
        key, value, GetExpiryForMemoryCache(key, expiry), size);
    if (!result && size > settings_.max_memory_cache_size && !mutable_cache_) {
      OLP_SDK_LOG_INFO_F(kLogTag,
                         "Failed to store value in memory cache %s, size %d",
                         key.c_str(), static_cast<int>(size));
    }
    OLP_SDK_LOG_DEBUG_F(kLogTag, "Put(encoder) memory cache done, key=%s", key.c_str());

  }

  return PutMutableCache(key, encoded_item, expiry);
}

bool DefaultCacheImpl::Put(const std::string& key,
                           const KeyValueCache::ValueTypePtr value,
                           time_t expiry) {
  OLP_SDK_LOG_DEBUG_F(kLogTag, "Put, key=%s", key.c_str());
  if (!value) {
    OLP_SDK_LOG_WARNING_F(kLogTag, "Put failed, value is an empty, key=%s", key.c_str());
    return false;
  }

  std::lock_guard<std::mutex> lock(cache_lock_);
  OLP_SDK_LOG_DEBUG_F(kLogTag, "Put mutex locked, key=%s", key.c_str());
  if (!is_open_) {
    OLP_SDK_LOG_WARNING_F(kLogTag, "Put failed, cache is not opened, key=%s", key.c_str());
    return false;
  }

  if (memory_cache_) {
    const auto size = value->size();
    const bool result = memory_cache_->Put(
        key, value, GetExpiryForMemoryCache(key, expiry), size);
    if (!result && size > settings_.max_memory_cache_size && !mutable_cache_) {
      OLP_SDK_LOG_INFO_F(kLogTag,
                         "Failed to store value in memory cache %s, size %d",
                         key.c_str(), static_cast<int>(size));
    }
    OLP_SDK_LOG_DEBUG_F(kLogTag, "Put memory cache done, key=%s",
                        key.c_str());
  }

  leveldb::Slice slice(reinterpret_cast<const char*>(value->data()),
                       value->size());
  return PutMutableCache(key, slice, expiry);
}

boost::any DefaultCacheImpl::Get(const std::string& key,
                                 const Decoder& decoder) {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!is_open_) {
    return boost::any();
  }

  if (memory_cache_) {
    auto value = memory_cache_->Get(key);
    if (!value.empty()) {
      PromoteKeyLru(key);
      return value;
    }
  }

  auto disc_cache = GetFromDiscCache(key);

  if (disc_cache) {
    auto decoded_item = decoder(disc_cache->first);
    if (memory_cache_) {
      auto expiry = disc_cache->second;
      memory_cache_->Put(key, decoded_item,
                         GetExpiryForMemoryCache(key, expiry),
                         disc_cache->first.size());
    }
    return decoded_item;
  }

  return boost::any();
}

KeyValueCache::ValueTypePtr DefaultCacheImpl::Get(const std::string& key) {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!is_open_) {
    return nullptr;
  }

  if (memory_cache_) {
    auto value = memory_cache_->Get(key);
    if (!value.empty()) {
      PromoteKeyLru(key);
      return boost::any_cast<KeyValueCache::ValueTypePtr>(value);
    }
  }

  KeyValueCache::ValueTypePtr value = nullptr;
  time_t expiry = KeyValueCache::kDefaultExpiry;

  auto result = GetFromDiskCache(key, value, expiry);
  if (result && value) {
    if (memory_cache_) {
      memory_cache_->Put(key, value, GetExpiryForMemoryCache(key, expiry),
                         value->size());
    }

    return value;
  }
  return nullptr;
}

bool DefaultCacheImpl::Remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(cache_lock_);

  if (!is_open_) {
    return false;
  }

  // In case the key is protected do not remove it
  if (protected_keys_.IsProtected(key)) {
    OLP_SDK_LOG_INFO_F(kLogTag,
                       "Remove() called on a protected key, ignoring, key='%s'",
                       key.c_str());

    return false;
  }

  // protected data could be removed by user
  if (memory_cache_) {
    memory_cache_->Remove(key);
  }

  RemoveKeyLru(key);

  if (mutable_cache_) {
    uint64_t removed_data_size = 0;
    PurgeDiskItem(key, *mutable_cache_, removed_data_size);

    mutable_cache_data_size_ -= removed_data_size;
  }

  return true;
}

bool DefaultCacheImpl::RemoveKeysWithPrefix(const std::string& key) {
  OLP_SDK_LOG_DEBUG_F(kLogTag, "RemoveKeysWithPrefix start, key=%s", key.c_str());

  std::lock_guard<std::mutex> lock(cache_lock_);
  OLP_SDK_LOG_DEBUG_F(kLogTag, "RemoveKeysWithPrefix mutex locked, key=%s",
                      key.c_str());


  if (!is_open_) {
    OLP_SDK_LOG_DEBUG_F(kLogTag, "RemoveKeysWithPrefix cache not opened, key=%s",
                        key.c_str());

    return false;
  }

  auto filter = [&](const std::string& cache_key) {
    return protected_keys_.IsProtected(cache_key);
  };

  if (memory_cache_) {
    memory_cache_->RemoveKeysWithPrefix(key, filter);
    OLP_SDK_LOG_DEBUG_F(kLogTag, "RemoveKeysWithPrefix removed from memory, key=%s",
                        key.c_str());

  }

  // No need to check here for protected key as these are not added to LRU from
  // the start
  RemoveKeysWithPrefixLru(key);
  OLP_SDK_LOG_DEBUG_F(kLogTag, "RemoveKeysWithPrefix removed from lru, key=%s", key.c_str());


  if (mutable_cache_) {
    uint64_t removed_data_size = 0;
    auto result =
        mutable_cache_->RemoveKeysWithPrefix(key, removed_data_size, filter);
    mutable_cache_data_size_ -= removed_data_size;
    OLP_SDK_LOG_DEBUG_F(kLogTag, "RemoveKeysWithPrefix removed from mutable cache, done, key=%s",
                        key.c_str());

    return result;
  }

  OLP_SDK_LOG_DEBUG_F(kLogTag, "RemoveKeysWithPrefix done, key=%s",
                      key.c_str());

  return true;
}

bool DefaultCacheImpl::Contains(const std::string& key) const {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!is_open_) {
    return false;
  }

  if (memory_cache_ && memory_cache_->Contains(key)) {
    return true;
  }

  // if lru exist check if key is there
  if (mutable_cache_lru_) {
    auto it = mutable_cache_lru_->FindNoPromote(key);
    if (it != mutable_cache_lru_->end()) {
      ValueProperties props = it->value();
      props.expiry -= olp::cache::InMemoryCache::DefaultTimeProvider()();
      return (props.expiry > 0);
      // if lru exist, but key not found, this case possible only for protected
      // keys
    } else if (protected_keys_.IsProtected(key)) {
      return mutable_cache_ && mutable_cache_->Contains(key);
    }

    // check in mutable cache only if lru does not exist
  } else if (mutable_cache_ && mutable_cache_->Contains(key)) {
    return (GetRemainingExpiryTime(key, *mutable_cache_) > 0) ||
           protected_keys_.IsProtected(key);
  }

  if (protected_cache_ && protected_cache_->Contains(key)) {
    return (GetRemainingExpiryTime(key, *protected_cache_) > 0);
  }

  return false;
}

bool DefaultCacheImpl::AddKeyLru(std::string key, const leveldb::Slice& value) {
  // do not add protected keys to lru, this applies to all keys with some
  // protected prefix, do not add internal keys
  if (mutable_cache_lru_ && !protected_keys_.IsProtected(key) &&
      !IsInternalKey(key)) {
    // remove the prefix to restore original key
    const bool expiration_key = IsExpiryKey(key);
    if (expiration_key) {
      key.resize(key.size() - kExpirySuffixLength);
    }

    ValueProperties props;

    auto iterator = mutable_cache_lru_->FindNoPromote(key);
    if (iterator != mutable_cache_lru_->end()) {
      props = iterator->value();
    }

    if (expiration_key) {
      // value.data() could point to a value without null character at the
      // end, this could cause exception in std::stol. This is fixed by
      // constructing a string, (We rely on small string optimization here).
      std::string timestamp(value.data(), value.size());
      props.expiry = std::stol(timestamp.c_str());
    } else {
      props.size = value.size();
    }

    auto result = mutable_cache_lru_->InsertOrAssign(key, props);
    return result.second;
  }
  return false;
}

void DefaultCacheImpl::InitializeLru() {
  if (!mutable_cache_) {
    return;
  }

  OLP_SDK_LOG_INFO_F(kLogTag, "Initializing mutable LRU cache");

  mutable_cache_data_size_ = 0;

  mutable_cache_lru_ =
      std::make_unique<DiskLruCache>(settings_.max_disk_storage);

  const auto start = std::chrono::steady_clock::now();
  auto it = mutable_cache_->NewIterator(leveldb::ReadOptions());

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    auto key = it->key().ToString();
    const auto& value = it->value();

    // Here we count both expiry keys and regular keys
    mutable_cache_data_size_ += key.size() + value.size();

    AddKeyLru(key, value);
  }

  OLP_SDK_LOG_INFO_F(
      kLogTag, "LRU cache initialized, items=%zu, time=%" PRId64 " ms",
      mutable_cache_lru_->Size(), GetElapsedTime(start));
}

bool DefaultCacheImpl::RemoveKeyLru(const std::string& key) {
  if (mutable_cache_lru_) {
    return mutable_cache_lru_->Erase(key);
  }
  return false;
}

void DefaultCacheImpl::RemoveKeysWithPrefixLru(const std::string& key) {
  if (!mutable_cache_lru_) {
    return;
  }

  for (auto it = mutable_cache_lru_->begin();
       it != mutable_cache_lru_->end();) {
    auto const& element_key = it.key();
    if (element_key.size() >= key.size() &&
        std::equal(key.begin(), key.end(), element_key.begin())) {
      it = mutable_cache_lru_->Erase(it);
      continue;
    }

    ++it;
  }
}

bool DefaultCacheImpl::PromoteKeyLru(const std::string& key) {
  if (mutable_cache_lru_) {
    auto it = mutable_cache_lru_->Find(key);
    return it != mutable_cache_lru_->end() || protected_keys_.IsProtected(key);
  }

  return true;
}

uint64_t DefaultCacheImpl::MaybeEvictData() {
  if (!mutable_cache_ || !mutable_cache_lru_) {
    return 0;
  }

  const auto max_size = kMaxDiskUsedThreshold * settings_.max_disk_storage;
  if (mutable_cache_data_size_ < max_size) {
    return 0;
  }

  const auto start = std::chrono::steady_clock::now();
  int64_t left_to_evict =
      mutable_cache_data_size_ -
      std::llroundl(settings_.max_disk_storage * kMinDiskUsedThreshold);
  uint64_t evicted = 0u;
  auto count = 0u;

  const auto call_evict_method =
      [&](std::function<EvictionResult(DefaultCacheImpl*, leveldb::WriteBatch&,
                                       uint64_t)>
              evict_method) {
        EvictionResult eviction_result;
        uint64_t current_eviction_target;

        do {
          // At this point left_to_evict can't be below zero.
          current_eviction_target =
              static_cast<uint64_t>(left_to_evict) < eviction_portion_
                  ? left_to_evict
                  : eviction_portion_;
          auto eviction_batch = std::make_unique<leveldb::WriteBatch>();
          eviction_result =
              evict_method(this, *eviction_batch, current_eviction_target);

          // Apply intermediate batch
          const auto apply_result =
              mutable_cache_->ApplyBatch(std::move(eviction_batch));

          if (!apply_result.IsSuccessful()) {
            OLP_SDK_LOG_WARNING_F(
                kLogTag,
                "MaybeEvictData(): failed to apply batch, error_code=%d, "
                "error_message=%s",
                static_cast<int>(apply_result.GetError().GetErrorCode()),
                apply_result.GetError().GetMessage().c_str());
            break;
          }

          mutable_cache_->Compact();

          evicted += eviction_result.size;
          count += eviction_result.count;
          left_to_evict -= eviction_result.size;
        } while (eviction_result.size >= current_eviction_target &&
                 left_to_evict > 0);
      };

  // Evict expired data first
  call_evict_method(std::mem_fn(&DefaultCacheImpl::EvictExpiredDataPortion));

  // If after expired data eviction the desired size isn't reached yet, eviction
  // of not expired data is required.
  if (left_to_evict > 0) {
    call_evict_method(std::mem_fn(&DefaultCacheImpl::EvictDataPortion));
  }

  OLP_SDK_LOG_INFO_F(kLogTag,
                     "Evicted from mutable cache, items=%" PRId32
                     ", time=%" PRId64 "ms, size=%" PRIu64,
                     count, GetElapsedTime(start), evicted);

  return evicted;
}

DefaultCacheImpl::EvictionResult DefaultCacheImpl::EvictExpiredDataPortion(
    leveldb::WriteBatch& batch, uint64_t target_eviction_size) {
  uint64_t evicted = 0u;
  auto count = 0u;
  const auto current_time = olp::cache::InMemoryCache::DefaultTimeProvider()();

  // Protected elements are not stored in lru, so do not need to check
  for (auto it = mutable_cache_lru_->begin();
       it != mutable_cache_lru_->end() && evicted < target_eviction_size;) {
    const auto& key = it->key();
    const auto& properties = it->value();

    const bool expired = (properties.expiry - current_time) <= 0;

    if (!expired) {
      ++it;
      continue;
    }

    // Remove the key
    batch.Delete(key);
    evicted += key.size() + properties.size;

    // Remove the key's expiry
    auto expiry_key = CreateExpiryKey(key);
    batch.Delete(expiry_key);
    evicted += expiry_key.size() + kExpiryValueSize;

    ++count;

    if (memory_cache_) {
      memory_cache_->Remove(key);
    }

    it = mutable_cache_lru_->Erase(it);
  }

  OLP_SDK_LOG_DEBUG_F(kLogTag,
                      "EvictExpiredDataPortion(): Evicted successfully, "
                      "count=%d, evicted=%" PRIu64,
                      count, evicted);
  return {count, evicted};
}

DefaultCacheImpl::EvictionResult DefaultCacheImpl::EvictDataPortion(
    leveldb::WriteBatch& batch, uint64_t target_eviction_size) {
  uint64_t evicted = 0u;
  auto count = 0u;

  // Protected elements are not stored in lru, so do not need to check
  for (auto it = mutable_cache_lru_->rbegin();
       it != mutable_cache_lru_->rend() && evicted < target_eviction_size;) {
    const auto& key = it->key();
    const auto& properties = it->value();

    // Remove the key
    evicted += key.size() + properties.size;
    batch.Delete(key);

    // Remove the key's expiry
    if (IsExpiryValid(properties.expiry)) {
      const auto expiry_key = CreateExpiryKey(key);
      evicted += expiry_key.size() + kExpiryValueSize;
      batch.Delete(expiry_key);
    }

    ++count;

    if (memory_cache_) {
      memory_cache_->Remove(it->key());
    }

    mutable_cache_lru_->Erase(it);
    it = mutable_cache_lru_->rbegin();
  }

  OLP_SDK_LOG_DEBUG_F(
      kLogTag,
      "EvictDataPortion(): Evicted successfully, count=%u, evicted=%" PRIu64,
      count, evicted);
  return {count, evicted};
}

int64_t DefaultCacheImpl::MaybeUpdatedProtectedKeys(
    leveldb::WriteBatch& batch) {
  if (protected_keys_.IsDirty()) {
    auto prev_size = protected_keys_.Size();
    auto value = protected_keys_.Serialize();
    // calculate key size, as it is be written for the first time
    auto key_size = (prev_size > 0 ? 0 : strlen(kProtectedKeys));
    if (value->size() > 0) {
      leveldb::Slice slice(reinterpret_cast<const char*>(value->data()),
                           value->size());
      batch.Put(kProtectedKeys, slice);
      return (key_size + protected_keys_.Size() - prev_size);
    } else if (prev_size > 0) {
      // delete key, as protected list is empty
      batch.Delete(kProtectedKeys);
      return -1 * static_cast<int64_t>((prev_size + strlen(kProtectedKeys)));
    }
  }

  return 0;
}

bool DefaultCacheImpl::PutMutableCache(const std::string& key,
                                       const leveldb::Slice& value,
                                       time_t expiry) {
  if (!mutable_cache_) {
    OLP_SDK_LOG_DEBUG_F(kLogTag, "No mutable cache, skip, key=%s",
                          key.c_str());
    return true;
  }

  // can't put new item if cache is full and eviction disabled
  const auto item_size = value.size();
  const auto expected_size = mutable_cache_data_size_ + item_size + key.size() +
                             key.size() + kExpirySuffixLength +
                             kExpiryValueSize;
  if (!mutable_cache_lru_ && expected_size > settings_.max_disk_storage) {
    OLP_SDK_LOG_WARNING_F(kLogTag, "Mutable cache is full, key=%s",
                          key.c_str());
    return false;
  }

  uint64_t added_data_size = 0u;
  auto batch = std::make_unique<leveldb::WriteBatch>();
  batch->Put(key, value);
  added_data_size += key.size() + item_size;

  if (IsExpiryValid(expiry)) {
    expiry += olp::cache::InMemoryCache::DefaultTimeProvider()();
    added_data_size += StoreExpiry(key, *batch, expiry);
  }

  auto removed_data_size = MaybeEvictData();
  auto updated_data_size = MaybeUpdatedProtectedKeys(*batch);

  auto result = mutable_cache_->ApplyBatch(std::move(batch));
  if (!result.IsSuccessful()) {
    OLP_SDK_LOG_WARNING_F(kLogTag,
                          "Failed to apply batch, key=%s",
                          key.c_str());
    return false;
  }
  mutable_cache_data_size_ += added_data_size;
  mutable_cache_data_size_ -= removed_data_size;
  mutable_cache_data_size_ += updated_data_size;

  // do not add protected keys to lru
  if (mutable_cache_lru_ && !protected_keys_.IsProtected(key)) {
    ValueProperties props;
    props.size = item_size;
    props.expiry = expiry;
    const auto result = mutable_cache_lru_->InsertOrAssign(key, props);
    if (result.first == mutable_cache_lru_->end() && !result.second) {
      OLP_SDK_LOG_WARNING_F(
          kLogTag, "Failed to store value in mutable LRU cache, key %s",
          key.c_str());
      return false;
    }
  }

  OLP_SDK_LOG_DEBUG_F(
      kLogTag, "Put to mutable cache done, key=%s", key.c_str());
  return true;
}

DefaultCache::StorageOpenResult DefaultCacheImpl::SetupStorage() {
  auto result = DefaultCache::Success;

  memory_cache_.reset();
  mutable_cache_.reset();
  mutable_cache_lru_.reset();
  protected_cache_.reset();
  protected_keys_ = ProtectedKeyList();
  mutable_cache_data_size_ = 0;

  if (settings_.max_memory_cache_size > 0) {
    memory_cache_.reset(new InMemoryCache(settings_.max_memory_cache_size));
  }

  if (settings_.disk_path_mutable) {
    result = SetupMutableCache();
  }

  if (result != StorageOpenResult::Success) {
    return result;
  }

  if (settings_.disk_path_protected) {
    result = SetupProtectedCache();
  }

  return result;
}

DefaultCache::StorageOpenResult DefaultCacheImpl::SetupProtectedCache() {
  protected_cache_ = std::make_unique<DiskCache>();

  // Storage settings for protected cache are different. We want to specify the
  // max_file_size greater than the manifest file size. Or else leveldb will try
  // to repair the cache.
  StorageSettings protected_storage_settings;
  protected_storage_settings.max_file_size = 32 * 1024 * 1024;

  // In case user requested read-write acccess we will try to open protected
  // cache as read-write also to prevent high RAM usage when cache is recovering
  // from uncompacted close.
  OpenOptions open_mode = settings_.openOptions;
  bool is_read_only = (settings_.openOptions & ReadOnly) == ReadOnly;
  if (!is_read_only) {
    if (utils::Dir::IsReadOnly(settings_.disk_path_protected.get())) {
      OLP_SDK_LOG_INFO_F(kLogTag,
                         "R/W permission missing, opening protected cache in "
                         "r/o mode, disk_path_protected='%s'",
                         settings_.disk_path_protected.get().c_str());
      open_mode = static_cast<OpenOptions>(open_mode | OpenOptions::ReadOnly);
    }
  }

  auto status = protected_cache_->Open(
      settings_.disk_path_protected.get(), settings_.disk_path_protected.get(),
      protected_storage_settings, open_mode, false);
  if (status != OpenResult::Success) {
    OLP_SDK_LOG_ERROR_F(kLogTag, "Failed to open protected cache %s",
                        settings_.disk_path_protected.get().c_str());

    protected_cache_.reset();
    return ToStorageOpenResult(status);
  }

  return DefaultCache::Success;
}

DefaultCache::StorageOpenResult DefaultCacheImpl::SetupMutableCache() {
  auto storage_settings = CreateStorageSettings(settings_);

  mutable_cache_ = std::make_unique<DiskCache>();
  auto status = mutable_cache_->Open(settings_.disk_path_mutable.get(),
                                     settings_.disk_path_mutable.get(),
                                     storage_settings, settings_.openOptions);
  if (status == OpenResult::Repaired) {
    OLP_SDK_LOG_INFO(kLogTag, "Mutable cache was repaired");
  } else if (status == OpenResult::Fail) {
    OLP_SDK_LOG_ERROR_F(kLogTag, "Failed to open the mutable cache %s",
                        settings_.disk_path_mutable.get().c_str());

    mutable_cache_.reset();
    return StorageOpenResult::OpenDiskPathFailure;
  }

  // read protected keys
  KeyValueCache::ValueTypePtr value = nullptr;
  auto result = mutable_cache_->Get(kProtectedKeys, value);
  if (result && value) {
    if (!protected_keys_.Deserialize(value)) {
      OLP_SDK_LOG_WARNING(kLogTag, "Deserialize protected keys failed");
    }
  }

  if (settings_.max_disk_storage != kMaxDiskSize &&
      settings_.eviction_policy == EvictionPolicy::kLeastRecentlyUsed) {
    InitializeLru();
  } else {
    mutable_cache_data_size_ = mutable_cache_->Size();
  }

  return DefaultCache::Success;
}

void DefaultCacheImpl::DestroyCache(DefaultCache::CacheType type) {
  if (type == DefaultCache::CacheType::kMutable) {
    if (mutable_cache_ && protected_keys_.IsDirty()) {
      auto batch = std::make_unique<leveldb::WriteBatch>();
      MaybeUpdatedProtectedKeys(*batch);
      auto result = mutable_cache_->ApplyBatch(std::move(batch));
      OLP_SDK_LOG_INFO_F(kLogTag,
                         "Close(): store list of protected keys, result=%s",
                         result.IsSuccessful() ? "true" : "false");
    }

    mutable_cache_.reset();
    mutable_cache_lru_.reset();
    protected_keys_ = ProtectedKeyList();
    mutable_cache_data_size_ = 0;
  } else {
    protected_cache_.reset();
  }
}

bool DefaultCacheImpl::GetFromDiskCache(const std::string& key,
                                        KeyValueCache::ValueTypePtr& value,
                                        time_t& expiry) {
  // Make sure we do not get a dirty entry
  value = nullptr;
  expiry = KeyValueCache::kDefaultExpiry;

  if (protected_cache_) {
    auto result = protected_cache_->Get(key, value);
    if (result && value && !value->empty()) {
      expiry = GetRemainingExpiryTime(key, *protected_cache_);
      if (expiry > 0) {
        return true;
      }
      value = nullptr;
    }
  }

  if (mutable_cache_) {
    expiry = GetRemainingExpiryTime(key, *mutable_cache_);

    if (expiry > 0 || protected_keys_.IsProtected(key)) {
      // Entry didn't expire yet, we can still use it
      if (!PromoteKeyLru(key)) {
        // If not found in LRU or not protected no need to look in disk cache
        // either.
        OLP_SDK_LOG_DEBUG_F(kLogTag,
                            "Key not found in LRU, and not protected, key='%s'",
                            key.c_str());
        return false;
      }

      auto result = mutable_cache_->Get(key, value);
      return result && value;
    }

    // Data expired in cache -> remove, but not protected keys
    uint64_t removed_data_size = 0u;
    PurgeDiskItem(key, *mutable_cache_, removed_data_size);
    mutable_cache_data_size_ -= removed_data_size;
    RemoveKeyLru(key);
  }

  return false;
}

boost::optional<std::pair<std::string, time_t>>
DefaultCacheImpl::GetFromDiscCache(const std::string& key) {
  KeyValueCache::ValueTypePtr value = nullptr;
  time_t expiry = KeyValueCache::kDefaultExpiry;
  auto result = GetFromDiskCache(key, value, expiry);
  if (result && value) {
    return std::make_pair(std::string(value->begin(), value->end()), expiry);
  }

  return boost::none;
}

std::string DefaultCacheImpl::GetExpiryKey(const std::string& key) const {
  return CreateExpiryKey(key);
}

bool DefaultCacheImpl::Protect(const DefaultCache::KeyListType& keys) {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!mutable_cache_) {
    return false;
  }
  auto start = std::chrono::steady_clock::now();
  auto result = protected_keys_.Protect(keys, [&](const std::string& key) {
    if (!RemoveKeyLru(key)) {
      RemoveKeysWithPrefixLru(key);
    }
  });
  if (memory_cache_ && result) {
    memory_cache_->Clear();
  }

  OLP_SDK_LOG_INFO_F(kLogTag,
                     "Protect, time=%" PRId64 " ms, added keys size=%" PRIu64
                     ", total size=%" PRIu64,
                     GetElapsedTime(start),
                     static_cast<std::uint64_t>(keys.size()),
                     protected_keys_.Count());
  return result;
}

bool DefaultCacheImpl::Release(const DefaultCache::KeyListType& keys) {
  std::lock_guard<std::mutex> lock(cache_lock_);
  if (!mutable_cache_) {
    return false;
  }
  auto start = std::chrono::steady_clock::now();
  auto result = protected_keys_.Release(keys);

  if (!result) {
    return result;
  }

  for (const auto& key : keys) {
    if (memory_cache_) {
      if (!memory_cache_->Remove(key)) {
        memory_cache_->RemoveKeysWithPrefix(key);
      }
    }
    if (mutable_cache_) {
      auto it = mutable_cache_->NewIterator(leveldb::ReadOptions());
      it->Seek(key);
      while (it->Valid()) {
        auto cached_key = it->key().ToString();
        if (cached_key.size() >= key.size() &&
            std::equal(key.begin(), key.end(), cached_key.begin())) {
          AddKeyLru(cached_key, it->value());
        } else {
          break;
        }
        it->Next();
      }
    }
  }

  OLP_SDK_LOG_INFO_F(kLogTag,
                     "Release, time=%" PRId64 " ms, released keys size=%" PRIu64
                     ", total size=%" PRIu64,
                     GetElapsedTime(start),
                     static_cast<std::uint64_t>(keys.size()),
                     protected_keys_.Count());
  return result;
}

bool DefaultCacheImpl::IsProtected(const std::string& key) const {
  std::lock_guard<std::mutex> lock(cache_lock_);
  return protected_keys_.IsProtected(key);
}

time_t DefaultCacheImpl::GetExpiryForMemoryCache(const std::string& key,
                                                 const time_t& expiry) const {
  // reset expiry if key is protected for memory cache
  if (protected_keys_.IsProtected(key)) {
    return KeyValueCache::kDefaultExpiry;
  }
  return expiry;
}

void DefaultCacheImpl::SetEvictionPortion(uint64_t size) {
  eviction_portion_ = size;
}

uint64_t DefaultCacheImpl::Size(CacheType type) const {
  if (type == CacheType::kMutable) {
    return mutable_cache_data_size_;
  }

  return protected_cache_ ? protected_cache_->Size() : 0;
}

uint64_t DefaultCacheImpl::Size(uint64_t new_size) {
  std::lock_guard<std::mutex> lock(cache_lock_);

  if (!is_open_ || !mutable_cache_ || !mutable_cache_lru_) {
    return 0u;
  }

  // Increase max size
  if (new_size >= settings_.max_disk_storage) {
    settings_.max_disk_storage = new_size;
    return 0u;
  }

  settings_.max_disk_storage = new_size;

  const auto evicted = MaybeEvictData();

  mutable_cache_data_size_ -= evicted;
  mutable_cache_->Compact();
  return evicted;
}

}  // namespace cache
}  // namespace olp
