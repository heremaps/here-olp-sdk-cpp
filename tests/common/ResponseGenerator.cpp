/*
 * Copyright (C) 2020 HERE Europe B.V.
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

#include "ResponseGenerator.h"

#include <string>
#include <vector>

#include <olp/dataservice/read/model/Partitions.h>
#include <olp/dataservice/read/model/VersionResponse.h>
#include "ApiDefaultResponses.h"
#include "PathGenerator.h"
#include "ReadDefaultResponses.h"
#include "generated/model/Api.h"
// clang-format off
#include "generated/serializer/ApiSerializer.h"
#include "generated/serializer/VersionResponseSerializer.h"
#include "generated/serializer/PartitionsSerializer.h"
#include "generated/serializer/JsonSerializer.h"
// clang-format on

namespace {
template <class T>
std::string serialize(std::vector<T> data) {
  std::string str = "[";
  for (const auto& el : data) {
    str.append(olp::serializer::serialize(el));
    str.append(",");
  }
  str[str.length() - 1] = ']';
  return str;
}
}  // namespace

std::string ResponseGenerator::ResourceApis(const std::string& catalog) {
  return serialize(
      mockserver::ApiDefaultResponses::GenerateResourceApisResponse(catalog));
}

std::string ResponseGenerator::Version(uint32_t version) {
  return olp::serializer::serialize(
      mockserver::ReadDefaultResponses::GenerateVersionResponse(version));
}

std::string ResponseGenerator::Partitions(
    const olp::dataservice::read::model::Partitions& partitions_response) {
  return olp::serializer::serialize(partitions_response);
}
