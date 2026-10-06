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

#include "igx_manager.hpp"

#include "platform_client.hpp"

#include <utility>

using namespace nvidia::igx::common;

namespace nvidia::igx::manager
{

IgxManager::IgxManager(sdbusplus::bus_t& bus, std::string basePath) :
    bus(bus), basePath(std::move(basePath))
{
    namespace rules = sdbusplus::bus::match::rules;

    // Re-resolve identity whenever the daemon's Info properties change (e.g. a
    // slow/cold boot transitions Degraded -> Ready after construction).
    platformPropsMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        rules::sender(nvidia::platform::service) +
            rules::propertiesChanged(nvidia::platform::objPath,
                                     nvidia::platform::infoIface),
        [this](sdbusplus::message::message&) { resolveAndRegister(); });

    // Re-resolve + re-subscribe when the daemon (re)acquires its name; D-Bus
    // signals do not replay across a restart (ARCHITECTURE §10a/NG-7).
    platformOwnerMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        rules::nameOwnerChanged() + rules::argN(0, nvidia::platform::service),
        [this](sdbusplus::message::message& msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            try
            {
                msg.read(name, oldOwner, newOwner);
            }
            catch (const std::exception&)
            {
                return;
            }
            if (name == nvidia::platform::service && !newOwner.empty())
            {
                resolveAndRegister();
            }
        });

    resolveAndRegister();
}

void IgxManager::resolveAndRegister()
{
    try
    {
        // platformShort() uses IdentityReady; empty means unknown/transient —
        // keep any existing inventory (do not churn through IGX_Host).
        std::string platformName = nvidia::platform::platformShort(bus);
        if (platformName.empty())
        {
            return;
        }

        std::string inventoryName;
        std::string inventoryPrettyName;
        if (platformName == "Thor")
        {
            inventoryName = "IGX_Thor";
            inventoryPrettyName = "IGX Thor";
        }
        else if (platformName == "Orin")
        {
            inventoryName = "IGX_Orin";
            inventoryPrettyName = "IGX Orin";
        }
        else
        {
            inventoryName = "IGX_Host";
            inventoryPrettyName = "IGX Host";
        }

        // Register once per distinct path; skip if unchanged so unrelated
        // property changes never tear down a live inventory object (§10a).
        if (igxInventory && currentName == inventoryName)
        {
            return;
        }
        igxInventory = std::make_unique<Igx>(
            bus, basePath + "/" + inventoryName, inventoryPrettyName);
        currentName = inventoryName;
        lg2::info("IGX inventory registered at {NAME}", "NAME", inventoryName);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create d-bus object for IGX inventory", "ERROR",
                   e.what());
    }
}

} // namespace nvidia::igx::manager
