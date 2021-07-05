#!/bin/bash -e
#
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

echo ">>> Installing mock server SSL certificate into OS... >>>"
curl https://raw.githubusercontent.com/mock-server/mockserver/master/mockserver-core/src/main/resources/org/mockserver/socket/CertificateAuthorityCertificate.pem --output mock-server-cert.cer
certutil -enterprise -f -v -addstore "Root" mock-server-cert.cer
certutil -enterprise -f -v -addstore "CA" mock-server-cert.cer


[[ -d "build" ]] && rm -rf build
mkdir build && cd build
cmake .. -G "Visual Studio 16 2019" -A "x64" \
        -DCMAKE_BUILD_TYPE=$BUILD_TYPE
cmake --build . --config $BUILD_TYPE
