/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
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

#include "usb_i2c_mapper.hpp"

#include <iostream>

namespace recovery_tool
{

namespace usb_i2c
{

int getI2CBusFromUSBPort(const std::string& usbPort, bool verbose)
{
    std::filesystem::path basePath = usbDevicePath + usbPort;
    if (!std::filesystem::exists(basePath))
    {
        if (verbose)
        {
            std::cerr << "USB port path does not exist: " << basePath.string()
                      << "\n";
        }
        return -1;
    }

    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(basePath))
    {
        if (entry.is_directory())
        {
            std::string dirname = entry.path().filename().string();
            if (dirname.starts_with(i2cBusPrefix))
            {
                std::string busNumStr = dirname.substr(i2cBusPrefixLength);
                try
                {
                    uint64_t busNum = std::stoull(busNumStr);

                    if (verbose)
                    {
                        std::cout << "Found I2C bus " << busNumStr
                                  << " in USB port path "
                                  << entry.path().string() << "\n";
                    }
                    return busNum;
                }
                catch (const std::exception& e)
                {
                    continue;
                }
            }
        }
    }

    // Check subfolders that match the USB port pattern
    for (const auto& entry : std::filesystem::directory_iterator(basePath))
    {
        if (entry.is_directory())
        {
            std::string dirname = entry.path().filename().string();
            if (dirname.starts_with(usbPort + ":"))
            {
                // Search for i2c folders in this interface subfolder
                for (const auto& subEntry :
                     std::filesystem::directory_iterator(entry.path()))
                {
                    if (subEntry.is_directory())
                    {
                        std::string subDirname =
                            subEntry.path().filename().string();
                        if (subDirname.starts_with(i2cBusPrefix))
                        {
                            std::string busNumStr =
                                subDirname.substr(i2cBusPrefixLength);
                            try
                            {
                                uint64_t busNum = std::stoull(busNumStr);
                                if (verbose)
                                {
                                    std::cout << "Found I2C bus " << busNumStr
                                              << " in USB interface path "
                                              << subEntry.path().string()
                                              << "\n";
                                }
                                return busNum;
                            }
                            catch (const std::exception& e)
                            {
                                continue;
                            }
                        }
                    }
                }
            }
        }
    }

    if (verbose)
    {
        std::cerr << "No valid I2C bus found in USB port path: "
                  << basePath.string() << "\n";
    }
    return -1;
}

} // namespace usb_i2c
} // namespace recovery_tool