/*
 * Copyright (C) 2019-2021 HERE Europe B.V.
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

#include <olp/core/logging/ConsoleAppender.h>
#include <olp/core/porting/platform.h>
#include <string>

#ifdef PORTING_PLATFORM_ANDROID
#include <android/log.h>
#include <algorithm>
#include <cmath>
#else
#include <iostream>
#endif

namespace olp {
namespace logging {
#ifdef PORTING_PLATFORM_ANDROID
static int convertLevel(Level level) {
  switch (level) {
    case Level::Trace:
      return ANDROID_LOG_VERBOSE;
    case Level::Debug:
      return ANDROID_LOG_DEBUG;
    case Level::Info:
      return ANDROID_LOG_INFO;
    case Level::Warning:
      return ANDROID_LOG_WARN;
    case Level::Error:
      return ANDROID_LOG_ERROR;
    case Level::Fatal:
      return ANDROID_LOG_FATAL;
    default:
      return ANDROID_LOG_VERBOSE;
  }
}

static void android_append_in_chunks(Level level, const char* tag,
                                     const std::string& message) {
  std::string::size_type MAX_LOGCAT_LINE_LENGTH_WO_SERVICE_INFO = 900;
  unsigned int chunk_num = int(std::ceil(
      double(message.size()) / MAX_LOGCAT_LINE_LENGTH_WO_SERVICE_INFO));

  for (unsigned int chunk = 0; chunk < chunk_num; ++chunk) {
    std::string::size_type from =
        chunk * MAX_LOGCAT_LINE_LENGTH_WO_SERVICE_INFO;
    std::string::size_type to =
        std::min(from + MAX_LOGCAT_LINE_LENGTH_WO_SERVICE_INFO, message.size());

    __android_log_print(convertLevel(level), tag, "%s",
                        message.substr(from, to - from).c_str());
  }
}
#endif

IAppender& ConsoleAppender::append(const LogMessage& message) {
  std::string formattedMessage = m_formatter.format(message);

#ifdef PORTING_PLATFORM_ANDROID
  android_append_in_chunks(message.level, message.tag, formattedMessage);
#else
  printf("%s\n", formattedMessage.c_str());
  //std::cout << formattedMessage << std::endl;
#endif
  return *this;
}

}  // namespace logging
}  // namespace olp
