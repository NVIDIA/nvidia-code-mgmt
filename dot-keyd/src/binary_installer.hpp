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

#include <string>
#include <vector>

/**
 * Installs CAK by invoking a configured helper executable.
 * No post-install verification — relies on the helper's exit code.
 * Uses async steady_timer for the waitpid loop to avoid blocking the
 * io_context.
 */
class BinaryInstaller : public DotInstaller
{
  public:
    explicit BinaryInstaller(const Config& config,
                             std::shared_ptr<sdbusplus::asio::connection> bus) :
        DotInstaller(config, std::move(bus))
    {}

  protected:
    asio::awaitable<void> doInstall(const json& payload) override;

  private:
    asio::awaitable<void> runHelper(const fs::path& cakFile);
    asio::awaitable<RunResult> execute(const std::vector<std::string>& command);
};
