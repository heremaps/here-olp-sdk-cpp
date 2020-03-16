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

#pragma once

#include <string>

#include <olp/core/http/NetworkTypes.h>

namespace olp {
namespace http {
/**
 * @brief Network internal utilities
 */
class NetworkUtils {
 public:
  static char SimpleToUpper(char c);
  static bool CaseInsensitiveCompare(const std::string& str1,
                                     const std::string& str2,
                                     size_t offset = 0);
  static bool CaseInsensitiveStartsWith(const std::string& str1,
                                        const std::string& str2,
                                        size_t offset = 0);
  static size_t CaseInsensitiveFind(const std::string& str1,
                                    const std::string& str2, size_t offset = 0);

  /**
   * @brief Extract the user agent from the headers.
   *
   * User agent is removed from the headers.
   *
   * @param headers The input headers.
   *
   * @return The user agent or an empty string if there is no user agent.
   */
  static std::string ExtractUserAgent(Headers& headers);
};  // class NetworkUtils

std::string HttpErrorToString(int error);

}  // namespace http
}  // namespace olp
