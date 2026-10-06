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

#pragma once

#include "config.h"

#include "igx.hpp"

#include <sdbusplus/bus/match.hpp>
#include <sdeventplus/event.hpp>

using namespace nvidia::igx::device;

namespace nvidia::igx::manager
{

class IgxManager
{
  public:
    IgxManager() = delete;
    ~IgxManager() = default;
    IgxManager(const IgxManager&) = delete;
    IgxManager& operator=(const IgxManager&) = delete;
    IgxManager(IgxManager&&) = delete;
    IgxManager& operator=(IgxManager&&) = delete;

    IgxManager(sdbusplus::bus_t& bus, std::string basePath);

  private:
    // Resolve the platform identity from com.Nvidia.Platform and (re)register
    // the inventory object. Registers the path-specific object only when
    // IdentityReady is true; re-registers only on an actual path change to
    // avoid ObjectMapper churn (ARCHITECTURE §10a).
    void resolveAndRegister();

    sdbusplus::bus_t& bus;
    std::string basePath;
    std::string currentName;

    std::unique_ptr<Igx> orinInvs;
    // Re-resolve when the daemon publishes a (real) identity, and re-subscribe
    // when it restarts -- D-Bus signals do not replay (§10a/NG-7).
    std::unique_ptr<sdbusplus::bus::match_t> platformPropsMatch;
    std::unique_ptr<sdbusplus::bus::match_t> platformOwnerMatch;
};

} // namespace nvidia::igx::manager
