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

#pragma once

struct udev;
struct udev_monitor;
struct udev_device;

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

/**
 * @class UdevMonitor
 * @brief Monitors USB device add events via libudev
 *
 * Shared instance for all USBRcmResources. Created lazily via getUdevMonitor().
 * Only triggers callbacks on device add events; removal is handled via MCTP
 * D-Bus signals.
 */
class UdevMonitor
{
  public:
    using Callback = std::function<void()>;

    explicit UdevMonitor(sdeventplus::Event& event);
    ~UdevMonitor();

    UdevMonitor(const UdevMonitor&) = delete;
    UdevMonitor& operator=(const UdevMonitor&) = delete;

    /**
     * @brief Register a callback for USB device add events on a specific port
     *
     * @param portPath USB port path to monitor (e.g., "1-1.1.1.2")
     * @param callback Function to call when device is added
     */
    void registerCallback(const std::string& portPath, Callback callback);

    /**
     * @brief Unregister callback for a USB port
     *
     * @param portPath USB port path to stop monitoring
     */
    void unregisterCallback(const std::string& portPath);

  private:
    struct udev* udev = nullptr;
    struct udev_monitor* monitor = nullptr;
    std::unique_ptr<sdeventplus::source::IO> ioSource;
    std::unordered_map<std::string, Callback> callbacks;

    /**
     * @brief Handle incoming udev events
     */
    void handleUdevEvent();

    /**
     * @brief Extract USB port path from udev device
     *
     * @param dev udev device
     * @return USB port path string, empty if not extractable
     */
    std::string extractPortPath(struct udev_device* dev);
};
