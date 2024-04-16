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

#include "recoverytool_interface.hpp"

namespace recovery_tool
{

namespace interface
{

std::vector<std::unique_ptr<CommandInterface>> commands;

class GetDeviceStatus : public CommandInterface
{
  public:
    ~GetDeviceStatus() = default;
    GetDeviceStatus() = delete;
    GetDeviceStatus(const GetDeviceStatus&) = delete;
    GetDeviceStatus(GetDeviceStatus&&) = default;
    GetDeviceStatus& operator=(const GetDeviceStatus&) = delete;
    GetDeviceStatus& operator=(GetDeviceStatus&&) = default;

    using CommandInterface::CommandInterface;

    void exec() override
    {
        try
        {
            recovery_tool::OCPRecoveryTool ocpRecoveryToolObj(
                busAddress, slaveAddress, verbose, emulation);
            nlohmann::json jsonResponse =
                ocpRecoveryToolObj.getDeviceStatusJson();
            std::cout << jsonResponse.dump(4) << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error in GetDeviceStatus: " << e.what() << "\n";
        }
    }
};

class GetRecoveryStatus : public CommandInterface
{
  public:
    ~GetRecoveryStatus() = default;
    GetRecoveryStatus() = delete;
    GetRecoveryStatus(const GetRecoveryStatus&) = delete;
    GetRecoveryStatus(GetRecoveryStatus&&) = default;
    GetRecoveryStatus& operator=(const GetRecoveryStatus&) = delete;
    GetRecoveryStatus& operator=(GetRecoveryStatus&&) = default;

    using CommandInterface::CommandInterface;

    void exec() override
    {
        try
        {
            recovery_tool::OCPRecoveryTool ocpRecoveryToolObj(
                busAddress, slaveAddress, verbose, emulation);
            nlohmann::json jsonResponse =
                ocpRecoveryToolObj.getRecoveryStatusJson();
            std::cout << jsonResponse.dump(4) << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error in GetRecoveryStatus: " << e.what() << "\n";
        }
    }
};

class PerformRecovery : public CommandInterface
{
  private:
    std::vector<std::string> imagePaths;

  public:
    ~PerformRecovery() = default;
    PerformRecovery() = delete;
    PerformRecovery(const PerformRecovery&) = delete;
    PerformRecovery(PerformRecovery&&) = default;
    PerformRecovery& operator=(const PerformRecovery&) = delete;
    PerformRecovery& operator=(PerformRecovery&&) = default;

    using CommandInterface::CommandInterface;

    explicit PerformRecovery(int busAddress, int slaveAddress, CLI::App* app) :
        CommandInterface(busAddress, slaveAddress, app)
    {
        app->add_option(
            "-i,--images", imagePaths,
            "List of image paths (e.g., -i /path/to/cms0 /path/to/cms1)");
    }

    void exec() override
    {
        try
        {
            recovery_tool::OCPRecoveryTool ocpRecoveryToolObj(
                busAddress, slaveAddress, verbose, emulation);
            nlohmann::json jsonResponse =
                ocpRecoveryToolObj.performRecovery(imagePaths);
            std::cout << jsonResponse.dump(4) << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error in PerformRecovery: " << e.what() << "\n";
        }
    }
};

void registerCommand(CLI::App& app)
{
    int busAddress;
    int slaveAddress;

    auto getDeviceStatusCmd =
        app.add_subcommand("GetDeviceStatus", "Get the device status");
    commands.push_back(std::make_unique<GetDeviceStatus>(
        busAddress, slaveAddress, getDeviceStatusCmd));

    auto getRecoveryStatusCmd =
        app.add_subcommand("GetRecoveryStatus", "Get the recovery status");
    commands.push_back(std::make_unique<GetRecoveryStatus>(
        busAddress, slaveAddress, getRecoveryStatusCmd));

    auto performRecoveryCmd =
        app.add_subcommand("PerformOCPRecovery", "Perform OCP recovery");
    commands.push_back(std::make_unique<PerformRecovery>(
        busAddress, slaveAddress, performRecoveryCmd));
}

} // namespace interface
} // namespace recovery_tool

int main(int argc, char** argv)
{
    try
    {
        CLI::App app{"OCP Recovery Tool for OpenBMC"};
        app.require_subcommand(1)->ignore_case();

        recovery_tool::interface::registerCommand(app);

        CLI11_PARSE(app, argc, argv);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return -1;
    }
}
