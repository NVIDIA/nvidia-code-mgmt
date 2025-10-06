/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "orin_manager.hpp"

using namespace nvidia::orin::common;

namespace nvidia::orin::manager
{

ORINManager::ORINManager(sdbusplus::bus::bus& bus, std::string basePath) :
    bus(bus)
{
    try
    {
        const auto baseinvInvPath = basePath + "/" + "IGX_Host";

        orinInvs = std::make_unique<Orin>(bus, baseinvInvPath);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create d-bus object for ORIN inventory", "ERROR",
                   e.what());
    }
}

} // namespace nvidia::orin::manager
