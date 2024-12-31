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

#include "erot_resource.hpp"

bool ERoTResource::isApBootFinished(const std::vector<uint8_t>& status)
{
    bool isApBootCompleted = getBit(status, AP0_BOOT_COMPLETE_BIT);
    bool isApBootCompleteTimeout =
        getBit(status, AP0_BOOT_COMPLETE_TIMEOUT_BIT);

    if (isApBootCompleteTimeout)
    {
        lg2::error("AP boot complete timeout");
    }

    return isApBootCompleted || isApBootCompleteTimeout;
}

mctp_vdm::requester::Coroutine ERoTResource::updateBootStatusAsync()
{
    std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
    if (!lock.owns_lock())
    {
        lg2::error("BootStatus refresh already in progress for EID={EID}",
                   "EID", fetchEid());
        co_return 0;
    }
    if (MCTPDiscoveryResource::isDeviceEnumerated() and
        MCTPDiscoveryResource::checkForEnabledMCTPEids())
    {
        const mctp_vdm::Message* responseMsg = nullptr;
        size_t responseLen = 0;
        auto eid = fetchEid();
        co_await mctpVdmHelper->queryBootStatus(eid, responseMsg, responseLen);

        if (responseMsg != nullptr)
        {
            std::vector<uint8_t> status(responseMsg->payload + 1,
                                        responseMsg->payload + responseLen);
            bootStatus->bootStatus(status);

            if (!isApBootFinished(status))
            {
                apBootStatusTimer->start(std::chrono::microseconds(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::seconds(apBootCompleteRetryInterval))));
            }
        }
    }
    else
    {
        bootStatus->bootStatus({0});
    }

    co_return 0;
}

std::vector<uint8_t> ERoTResource::getBootStatus() const noexcept
{
    return bootStatus->bootStatus();
}
