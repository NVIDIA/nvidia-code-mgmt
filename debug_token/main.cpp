/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "config.h"

#include "update_debug_token.hpp"

#include <getopt.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <exception>

using namespace phosphor::logging;
const int32_t EraseToken = 0;
const int32_t InstallToken = 1;

int main(int argc, char** argv)
{
    int operation;
    std::string version;
    std::string debugTokenPath;
    if (argc < 3)
    {
        log<level::ERR>("Invalid number of arguments");
        return -1;
    }
    try
    {
        operation = std::stoi(argv[1]);
        version = argv[2];
        if (operation == EraseToken)
        {
            auto bus = sdbusplus::bus::new_default();
            std::unique_ptr<UpdateDebugToken> updateDebugToken =
                std::make_unique<UpdateDebugToken>(bus);
            if (updateDebugToken->eraseDebugToken() != 0)
            {
                log<level::ERR>("Debug Token: Erase Failed");
                updateDebugToken->createMessageRegistry(
                    transferFailed, DEBUG_TOKEN_ERASE_NAME, version);
            }
            else
            {
                log<level::INFO>("Debug Token: Erase Success");
                // for erase token log entry not required since it will clutter
                // the message registry for all updates
            }
        }
        else if (operation == InstallToken)
        {
            if (argc != 4)
            {
                log<level::ERR>("Invalid number of arguments");
                return -1;
            }
            debugTokenPath = argv[3];
            auto bus = sdbusplus::bus::new_default();
            std::unique_ptr<UpdateDebugToken> updateDebugToken =
                std::make_unique<UpdateDebugToken>(bus);
            auto tokenInstallStatus =
                updateDebugToken->installDebugToken(debugTokenPath);
            if (tokenInstallStatus ==
                DebugTokenInstallStatus::DebugTokenInstallFailed)
            {
                log<level::ERR>("Debug Token: Install failed");
                updateDebugToken->createMessageRegistry(
                    transferFailed, DEBUG_TOKEN_INSTALL_NAME, version);
            }
            else if (tokenInstallStatus ==
                     DebugTokenInstallStatus::DebugTokenInstallNone)
            {
                log<level::ERR>(
                    "Debug Token: No matching serial numbers for install token");
                updateDebugToken->createMessageRegistry(
                    transferFailed, DEBUG_TOKEN_INSTALL_NAME, version);
            }
            else
            {
                log<level::INFO>("Debug Token: Install success");
                updateDebugToken->createMessageRegistry(
                    updateSuccessful, DEBUG_TOKEN_INSTALL_NAME, version);
            }
        }
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("Argument error", entry("ERROR=%s", e.what()));
        return -1;
    }
    return 0;
}
