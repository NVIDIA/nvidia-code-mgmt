// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
/*
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

#include "mcu_recovery_model.hpp"

#include <sdbusplus/bus.hpp>

#include <streambuf>
#include <string>

namespace nvidia::software::updater::mcu
{

// BOARD_PART_NUMBER lookup over xyz.openbmc_project.FruDevice objects.
FruLookup dbusFruLookup(sdbusplus::bus_t& bus);

// systemd StartUnit in "replace" mode (systemctl --no-block start).
StartUnit dbusStartUnit(sdbusplus::bus_t& bus);

// Stream buffer sending each line to the journal through lg2.
class JournalBuf : public std::streambuf
{
  public:
    explicit JournalBuf(bool error) : error(error)
    {}

  protected:
    int overflow(int c) override;

  private:
    bool error;
    std::string line;
};

} // namespace nvidia::software::updater::mcu
