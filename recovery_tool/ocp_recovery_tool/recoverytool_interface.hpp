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

#pragma once

#include "recoverytool_utils.hpp"

#include <CLI/CLI.hpp>

namespace recovery_tool
{

namespace interface

{
/**
 * @class CommandInterface
 * @brief Provides an interface for command operations.
 *
 */
class CommandInterface
{

  public:
    /**
     * @brief Constructs a new CommandInterface object.
     *
     * Initializes the object with provided parameters and sets up CLI options.
     *
     * @param busAddr Bus address for the command operation.
     * @param slaveAddr Slave address for the device.
     * @param app Pointer to the CLI app to add options to.
     */
    explicit CommandInterface(int busAddr, int slaveAddr, CLI::App* app) :
        busAddress(busAddr), slaveAddress(slaveAddr), verbose(false),
        emulation(false)
    {
        app->add_option("-b,--bus", busAddress, "Bus address")->required();
        app->add_option("-s,--slave", slaveAddress, "Slave address")
            ->required();
        app->add_flag("-v,--verbose", verbose, "Verbose output");
        app->add_flag("-e,--emulation", emulation, "To test emulation setup");
        app->callback([&]() { exec(); });
    }

    /**
     * @brief Virtual destructor to ensure correct cleanup in derived classes.
     */
    virtual ~CommandInterface() = default;

    /**
     * @brief Pure virtual method for command execution.
     *
     * Derived classes must provide an implementation.
     */
    virtual void exec() = 0;

  protected:
    int busAddress;
    int slaveAddress;
    bool verbose;
    bool emulation;
};

/**
 * @brief Registers all subcommands for the recovery tool.
 * @param app Reference to the CLI app to register the commands.
 */
void registerCommand(CLI::App& app);
} // namespace interface

} // namespace recovery_tool
