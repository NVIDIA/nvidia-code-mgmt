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

#include <set>
#include <string>

/**
 * Installs CAK via the nsmd D-Bus interface (useDefaultDbus mode).
 * Post-install verification polls the LPC snooper boot-progress property
 * rather than DOTState because nsmd does not reset DOTState on power-off.
 */
class DbusInstaller : public DotInstaller
{
  public:
    explicit DbusInstaller(const Config& config,
                           std::shared_ptr<sdbusplus::asio::connection> bus) :
        DotInstaller(config, std::move(bus))
    {}

  protected:
    asio::awaitable<bool> shouldSkip() override;
    asio::awaitable<void> doInstall(const json& payload) override;
    asio::awaitable<void> doVerify() override;
    asio::awaitable<void> doL1Reset() override;

  private:
    asio::awaitable<void> install(const json& payload,
                                  std::set<std::string>& donePaths);
    asio::awaitable<std::string> pollAsyncStatus(const std::string& asyncPath);
    asio::awaitable<void> waitForCakComplete();
    asio::awaitable<std::string> readCakInitState();
    static std::string toDbusAuthScheme(const std::string& scheme);
};
