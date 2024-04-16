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


#include "base_controller.hpp"

#include "version.hpp"
#include "watch.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{

using namespace phosphor::logging;
namespace fs = std::filesystem;

int BaseController::processImage(const std::string& imagePath)
{
    if (!fs::is_regular_file(imagePath))
    {
        // report and Log Event
        log<level::ERR>("Error file does not exist",
                        entry("FILENAME=%s", imagePath.c_str()));
        return -1;
    }

    // Get version
    std::filesystem::path p(imagePath);
    if (p.stem().empty()) // Stem has version
    {
        // Log and report error
        log<level::ERR>("Error unable to read version from image file");
        return -1;
    }
    if (abstractItemUpdater_->processImage(p) < 0)
    {
        // TODO Log an event then remove file
        fs::remove_all(p.string());
        auto msg = "Removing " + p.string();
        log<phosphor::logging::level::ERR>(msg.c_str());
    }
    return 0;
}

int BaseController::startWatching(sdbusplus::bus::bus& bus, sd_event* loop)
{
    // On fly
    try
    {

        nvidia::software::updater::Watch watch(
            loop, abstractItemUpdater_->getPathsToMonitor(),
            std::bind(std::mem_fn(&BaseController::processImage), this,
                      std::placeholders::_1));
        bus.attach_event(loop, SD_EVENT_PRIORITY_NORMAL);
        sd_event_loop(loop);
    }
    catch (std::exception& e)
    {
        using namespace phosphor::logging;
        log<level::ERR>(e.what());
        return -1;
    }
    return 0;
}
int BaseController::processExistingImages()
{
    abstractItemUpdater_->watchNewlyAddedDevice();
    abstractItemUpdater_->readExistingFirmWare();
    return 0;
}
} // namespace updater
} // namespace software
} // namespace nvidia
