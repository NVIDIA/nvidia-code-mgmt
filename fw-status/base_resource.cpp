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

#include "base_resource.hpp"

#include <phosphor-logging/device_error_log.hpp>

void BaseResource::commitRecoveryModeError(uint8_t eid)
{
    std::string name = path.substr(path.find_last_of('/') + 1);
    if (name.empty())
    {
        name = "EID_" + std::to_string(eid);
    }

    std::map<std::string, std::string> additionalData = {
        {"REDFISH_MESSAGE_ID", "NvidiaUpdate.1.0.FirmwareInRecovery"},
        {"REDFISH_MESSAGE_ARGS", name},
        {"REDFISH_RESOLUTION", "Perform device FW recovery"},
        {"REDFISH_SEVERITY",
         "xyz.openbmc_project.Logging.Entry.Level.Critical"},
        {"REDFISH_ORIGIN_OF_CONDITION", name}};

    lg2::info("Committing recovery mode error for device {DEVICE} (EID: {EID})",
              "DEVICE", name, "EID", eid);

    try
    {
        nv::lg2::CommitDeviceError(
            eid, nv::lg2::ErrorCode::Recovery::IN_RECOVERY,
            nv::lg2::ErrorClass::Recovery, additionalData);
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to commit recovery mode error for device {DEVICE}",
                   "DEVICE", name);
    }
}
