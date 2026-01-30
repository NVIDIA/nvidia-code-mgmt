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

#include "udev_monitor.hpp"

#include <libudev.h>

#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <stdexcept>

UdevMonitor::UdevMonitor(sdeventplus::Event& event)
{
    udev = udev_new();
    if (!udev)
    {
        throw std::runtime_error("Failed to create udev context");
    }

    monitor = udev_monitor_new_from_netlink(udev, "udev");
    if (!monitor)
    {
        udev_unref(udev);
        throw std::runtime_error("Failed to create udev monitor");
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(monitor, "usb",
                                                        "usb_device") < 0)
    {
        udev_monitor_unref(monitor);
        udev_unref(udev);
        throw std::runtime_error(
            "Failed to add udev monitor filter for USB devices");
    }

    if (udev_monitor_enable_receiving(monitor) < 0)
    {
        udev_monitor_unref(monitor);
        udev_unref(udev);
        throw std::runtime_error("Failed to enable udev monitor receiving");
    }

    int fd = udev_monitor_get_fd(monitor);
    ioSource = std::make_unique<sdeventplus::source::IO>(
        event, fd, EPOLLIN,
        [this](sdeventplus::source::IO&, int, uint32_t) { handleUdevEvent(); });

    lg2::info("UdevMonitor initialized for USB device events");
}

UdevMonitor::~UdevMonitor()
{
    ioSource.reset();
    if (monitor)
    {
        udev_monitor_unref(monitor);
    }
    if (udev)
    {
        udev_unref(udev);
    }
}

void UdevMonitor::registerCallback(const std::string& portPath,
                                   Callback callback)
{
    callbacks[portPath] = std::move(callback);
    lg2::info("Registered udev callback for USB port: {PORT}", "PORT",
              portPath);
}

void UdevMonitor::unregisterCallback(const std::string& portPath)
{
    callbacks.erase(portPath);
}

void UdevMonitor::handleUdevEvent()
{
    struct udev_device* dev = udev_monitor_receive_device(monitor);
    if (!dev)
    {
        return;
    }

    const char* action = udev_device_get_action(dev);
    if (!action)
    {
        udev_device_unref(dev);
        return;
    }

    std::string portPath = extractPortPath(dev);
    udev_device_unref(dev);

    if (portPath.empty())
    {
        return;
    }

    if (std::string(action) == "add" && callbacks.contains(portPath))
    {
        lg2::info("USB device added at port {PORT}", "PORT", portPath);
        callbacks[portPath]();
    }
    else
    {
        // Skip all other events (remove, change, bind, unbind, etc.)
        // Device removal is handled via MCTP D-Bus InterfacesRemoved signals
    }
}

std::string UdevMonitor::extractPortPath(struct udev_device* dev)
{
    const char* devpath = udev_device_get_devpath(dev);
    if (!devpath)
    {
        return {};
    }

    // Extract USB port path from sysfs path
    // Format: /sys/devices/.../usb1/1-1/1-1.1/1-1.1.1/1-1.1.1.2
    auto portPath = std::filesystem::path(devpath).filename().string();

    if (!portPath.empty())
    {
        return portPath;
    }
    return {};
}
