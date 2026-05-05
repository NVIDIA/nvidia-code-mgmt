/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "dot_installer.hpp"

#include <boost/beast/http/verb.hpp>

#include <set>
#include <string>
#include <utility>
#include <vector>

/**
 * Installs CAK via HMC Redfish endpoints.
 * Uses Boost.Beast for async HTTP — no blocking curl calls.
 */
class HmcInstaller : public DotInstaller
{
  public:
    explicit HmcInstaller(const Config& config,
                          std::shared_ptr<sdbusplus::asio::connection> bus) :
        DotInstaller(config, std::move(bus))
    {}

  protected:
    asio::awaitable<bool> shouldSkip() override;
    asio::awaitable<void> doPreInstallCheck() override;
    asio::awaitable<void> doInstall(const json& payload) override;
    asio::awaitable<void> doVerify() override;
    asio::awaitable<void> doL1Reset() override;

  private:
    asio::awaitable<std::string> getCakInitState();
    asio::awaitable<void> waitForCakInit(const std::string& expected);
    asio::awaitable<bool> logSbiosFmcRecoveryStatus();
    asio::awaitable<void>
        sendToRemainingPaths(const std::string& payloadStr,
                             std::set<std::string>& donePaths);
    asio::awaitable<std::string> httpRequest(boost::beast::http::verb method,
                                             const std::string& path,
                                             const std::string& body);

    static std::string base64Encode(const std::string& input);
    std::pair<std::string, std::string> parseHost() const;
};
