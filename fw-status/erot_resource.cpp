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

#include "erot_resource.hpp"

mctp_vdm::requester::Coroutine ERoTResource::updateBootStatusAsync()
{
    if (MCTPDiscoveryResource::isDeviceEnumerated() and MCTPDiscoveryResource::checkForEnabledMCTPEids())
    {
        const mctp_vdm::Message* responseMsg = nullptr;
        size_t responseLen = 0;
        auto eid = fetchEid();
        co_await mctpVdmHelper->queryBootStatus(eid, responseMsg, responseLen);

        std::vector<uint8_t> status(responseMsg->payload + 1, responseMsg->payload + responseLen);
        bootStatus->bootStatus(status);
    }
    else
    {
        bootStatus->bootStatus({0});
    }
}

std::vector<uint8_t> ERoTResource::getBootStatus() const noexcept
{
    return bootStatus->bootStatus();
}
