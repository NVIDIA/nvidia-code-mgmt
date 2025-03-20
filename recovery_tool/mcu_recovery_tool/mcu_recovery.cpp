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

#include <libusb-1.0/libusb.h>
#include <unistd.h>

#include <gpiod.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// Structure to hold MCU information
struct MCUInfo
{
    std::string usbPort;
    std::string resetGpioName;
    std::string recoveryGpioName;
    uint16_t recoveryPid;
    uint16_t functionalPid;
};

// Function to parse JSON file
std::map<std::string, MCUInfo> parseJsonFile(const std::string& jsonFilePath)
{
    std::ifstream file(jsonFilePath);
    json j;
    file >> j;

    std::map<std::string, MCUInfo> mcuMap;
    for (auto& [key, value] : j["usb"].items())
    {
        // Skip the non-MCU entries
        if (value.contains("_comment") &&
            value["_comment"].get<std::string>().find("MCU") !=
                std::string::npos)
        {
            MCUInfo info;
            info.usbPort = key;
            info.resetGpioName = value["reset_gpio_name"];
            info.recoveryGpioName = value["recovery_gpio_name"];
            info.recoveryPid = std::stoi(
                value["recovery_product_id"].get<std::string>(), nullptr, 16);
            info.functionalPid =
                std::stoi(value["product_id"].get<std::string>(), nullptr, 16);
            mcuMap[key] = info;
        }
    }
    return mcuMap;
}

std::string toHexString(uint16_t value)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(4)
        << value;
    return oss.str();
}

std::string getFullPortPath(libusb_device* dev)
{
    uint8_t ports[8] = {0};
    int pathLen = libusb_get_port_numbers(dev, ports, sizeof(ports));
    if (pathLen < 0)
    {
        std::cerr << "Error getting port numbers: "
                  << libusb_error_name(pathLen) << std::endl;
        return "";
    }

    std::string path = std::to_string(libusb_get_bus_number(dev));
    for (int i = 0; i < pathLen; ++i)
    {
        path += "-" + std::to_string(ports[i]);
    }
    return path;
}

// Function to find a device by USB port
libusb_device* findDeviceByPort(libusb_context* context,
                                const std::string& usbPort)
{
    libusb_device** deviceList = nullptr;
    ssize_t deviceCount = libusb_get_device_list(context, &deviceList);
    libusb_device* foundDevice = nullptr;

    if (deviceCount < 0)
    {
        std::cerr << "Failed to get USB device list" << std::endl;
        return nullptr;
    }

    for (ssize_t i = 0; i < deviceCount; ++i)
    {
        libusb_device* device = deviceList[i];
        if (usbPort == getFullPortPath(device))
        {
            foundDevice = device;
            break;
        }
    }

    libusb_free_device_list(deviceList, 1);
    return foundDevice;
}

// Function to handle recovery flow
void performRecoveryFlow(const std::map<std::string, MCUInfo>& mcuMap,
                         [[maybe_unused]] const std::string& binaryFilePath,
                         [[maybe_unused]] bool forceUpdate)
{
    libusb_context* context = nullptr;
    if (libusb_init(&context) < 0)
    {
        std::cerr << "Failed to initialize libusb" << std::endl;
        return;
    }

    for (const auto& [usbPort, mcuInfo] : mcuMap)
    {
        libusb_device* device = findDeviceByPort(context, usbPort);
        if (!device)
        {
            std::cerr << "Device not found on port " << usbPort
                      << ", forcing recovery" << std::endl;
            // Implement GPIO manipulation to force recovery
            // Example: set recovery pin, assert reset pin, deassert reset pin
        }
        else
        {
            std::cout << "Device found on port " << usbPort
                      << ", performing recovery" << std::endl;
            // Use blhost to perform the recovery
            // Example: system("blhost -V --usb-bus-device ...
            // flash-erase-all"); Example: system("blhost --usb-bus-device ...
            // receive-sb-file ...");
        }
    }

    libusb_exit(context);
}

// Function to handle reset flow
int performResetFlow(const std::map<std::string, MCUInfo>& mcuMap)
{
    libusb_context* context = nullptr;
    if (libusb_init(&context) < 0)
    {
        std::cerr << "Failed to initialize libusb" << std::endl;
        return -1;
    }

    // Copy the map to a list for easy removal of elements
    std::list<std::pair<std::string, MCUInfo>> mcuList(mcuMap.begin(),
                                                       mcuMap.end());

    // Open GPIO handles for each MCU reset pin
    std::map<std::string, gpiod::line> gpioLines;
    for (const auto& [usbPort, mcuInfo] : mcuMap)
    {
        try
        {
            auto line = gpiod::find_line(mcuInfo.resetGpioName);
            if (!line)
            {
                std::cerr << "GPIO line not found: " << mcuInfo.resetGpioName
                          << std::endl;
                continue;
            }
            // set the default value of the reset pin to 1
            line.request(
                {"mcu_recovery", gpiod::line_request::DIRECTION_OUTPUT, 0}, 1);
            gpioLines[usbPort] = line;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Failed to open GPIO: " << mcuInfo.resetGpioName
                      << " - " << e.what() << std::endl;
            continue;
        }
    }

    for (int i = 0; i < 11 && !mcuList.empty(); ++i)
    {
        for (auto it = mcuList.begin(); it != mcuList.end();)
        {
            const auto& [usbPort, mcuInfo] = *it;

            libusb_device* device = findDeviceByPort(context, usbPort);
            if (device)
            {
                libusb_device_descriptor desc;
                if (libusb_get_device_descriptor(device, &desc) == 0)
                {
                    if (mcuInfo.functionalPid == desc.idProduct)
                    {
                        std::cout << "MCU " << usbPort << " is functional"
                                  << std::endl;
                        // Remove MCU from list if MCU is functional
                        it = mcuList.erase(it);
                        // Release and remove the GPIO line to prevent
                        // unnecessary reset
                        gpioLines[usbPort].release();
                        gpioLines.erase(usbPort);
                        continue;
                    }
                    else
                    {
                        std::cerr << "MCU " << usbPort
                                  << " is not functional, retrying"
                                  << ". Current PID is "
                                  << toHexString(desc.idProduct)
                                  << " (expected: "
                                  << toHexString(mcuInfo.functionalPid) << ")"
                                  << std::endl;
                    }
                }
            }
            else
            {
                std::cerr << "Cannot find MCU on port " << usbPort
                          << ", retrying" << std::endl;
            }
            ++it;
        }
        // Put all MCUs into reset state
        for (const auto& [usbPort, line] : gpioLines)
        {
            line.set_value(0);
        }
        sleep(1);

        // Release all MCU reset pins
        for (const auto& [usbPort, line] : gpioLines)
        {
            line.set_value(1);
        }
        sleep(3);
    }

    int failedDevices = mcuList.size();
    if (failedDevices == 0)
    {
        std::cout << "All MCUs are functional." << std::endl;
    }
    else
    {
        std::cerr << "Some MCUs failed to become functional after 10 attempts:"
                  << std::endl;
        for (const auto& [usbPort, mcuInfo] : mcuList)
        {
            std::cerr << " - MCU on port " << usbPort << std::endl;
        }
    }

    // Release all GPIO lines
    for (const auto& [usbPort, line] : gpioLines)
    {
        line.release();
    }

    libusb_exit(context);
    return failedDevices;
}

int main(int argc, char* argv[])
{
    std::string binaryFilePath;
    std::string uuid;
    std::string jsonFilePath;
    bool forceUpdate = false;
    bool recoveryMode = false;
    bool resetMode = false;

    int opt;
    while ((opt = getopt(argc, argv, "f:u:dj:r")) != -1)
    {
        switch (opt)
        {
            case 'f':
                binaryFilePath = optarg;
                recoveryMode = true;
                break;
            case 'u':
                uuid = optarg;
                break;
            case 'd':
                forceUpdate = true;
                break;
            case 'j':
                jsonFilePath = optarg;
                break;
            case 'r':
                resetMode = true;
                break;
            default:
                std::cerr
                    << "Usage: " << argv[0]
                    << " -u <UUID> -j <JSON file> [-f <binary file>] [-d] [-r]"
                    << std::endl;
                return EXIT_FAILURE;
        }
    }

    if (uuid.empty() || jsonFilePath.empty() || (recoveryMode && resetMode) ||
        (!recoveryMode && !resetMode))
    {
        std::cerr
            << "Invalid arguments. Either -r or -f must be provided, and -u and -j are required."
            << std::endl;
        return EXIT_FAILURE;
    }

    auto mcuMap = parseJsonFile(jsonFilePath);

    if (recoveryMode)
    {
        performRecoveryFlow(mcuMap, binaryFilePath, forceUpdate);
        return EXIT_SUCCESS;
    }
    else if (resetMode)
    {
        return performResetFlow(mcuMap);
    }

    return EXIT_SUCCESS;
}
