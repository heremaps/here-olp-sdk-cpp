# Copyright (C) 2019-2021 HERE Europe B.V.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
# License-Filename: LICENSE

if (ANDROID OR IOS)
    message(STATUS "Currently the performance test runner for mobile platforms is not supported")
    return()
endif()

set(OLP_SDK_PERFORMANCE_TESTS_SOURCES
    ./MemoryTest.cpp
    ./MemoryTestBase.h
    ./NetworkWrapper.h
    ./PrefetchTest.cpp
)

add_executable(olp-cpp-sdk-performance-tests ${OLP_SDK_PERFORMANCE_TESTS_SOURCES})
target_link_libraries(olp-cpp-sdk-performance-tests
    PRIVATE
        custom-params
        gtest_main
        olp-cpp-sdk-authentication
        olp-cpp-sdk-dataservice-read
)
