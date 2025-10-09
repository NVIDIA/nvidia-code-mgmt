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

#include "config.h"

#include "mctp_endpoint_discovery.hpp"

#include "constants.hpp"
#include "types.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace mctp_vdm
{

using namespace dbus;

template <typename T>
MctpDiscovery<T>::MctpDiscovery(
    sdbusplus::bus::bus& bus, mctp_socket::Handler<T>& handler,
    std::initializer_list<MctpDiscoveryHandlerIntf*> list) :
    bus(bus), mctpEndpointAddedSignal(
                  bus,
                  sdbusplus::bus::match::rules::interfacesAdded(
                      "/xyz/openbmc_project/mctp"),
                  std::bind_front(&MctpDiscovery::discoverEndpoints, this)),
    handler(handler), handlers(list)
{
    dbus::ObjectValueTree objects;
    std::set<dbus::Service> mctpCtrlServices;
    mctp::Infos mctpInfos;

    try
    {
        const dbus::Interfaces ifaceList{"xyz.openbmc_project.MCTP.Endpoint"};
        auto method = bus.new_method_call(mapper::Service, mapper::Path,
                                          mapper::Interface, "GetSubTree");

        method.append("/xyz/openbmc_project/mctp", 0, ifaceList);
        auto reply = bus.call(method);
        GetSubTreeResponse getSubTreeResponse;
        reply.read(getSubTreeResponse);
        for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
        {
            for (const auto& [serviceName, interfaces] : mapperServiceMap)
            {
                mctpCtrlServices.emplace(serviceName);
            }
        }
    }
    catch (const std::exception& e)
    {
        handleMctpEndpoints(mctpInfos);
        return;
    }

    for (const auto& service : mctpCtrlServices)
    {
        dbus::ObjectValueTree objects{};
        try
        {
            auto method = bus.new_method_call(
                service.c_str(), "/xyz/openbmc_project/mctp",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
            auto reply = bus.call(method);
            reply.read(objects);
            for (const auto& [objectPath, interfaces] : objects)
            {
                populateMctpInfo(interfaces, mctpInfos);
            }
        }
        catch (const std::exception& e)
        {
            continue;
        }
    }
    handleMctpEndpoints(mctpInfos);
}

template <typename T>
void MctpDiscovery<T>::populateMctpInfo(const dbus::InterfaceMap& interfaces,
                                        mctp::Infos& mctpInfos)
{
    mctp::UUID uuid{};
    int type = 0;
    int protocol = 0;
    std::vector<uint8_t> address{};

    try
    {
        for (const auto& [intfName, properties] : interfaces)
        {
            if (intfName == mctp::UUIDInterface)
            {
                uuid = std::get<std::string>(properties.at("UUID"));
            }

            if (intfName == unixSocketIntfName)
            {
                type = std::get<uint8_t>(properties.at("Type"));
                protocol = std::get<size_t>(properties.at("Protocol"));
                address =
                    std::get<std::vector<uint8_t>>(properties.at("Address"));
            }
        }

        if (uuid.empty() || address.empty() || !type)
        {
            return;
        }

        if (interfaces.contains(mctpEndpointIntfName))
        {
            const auto& properties = interfaces.at(mctpEndpointIntfName);
            if (properties.contains("EID") &&
                properties.contains("SupportedMessageTypes") &&
                properties.contains("MediumType"))
            {
                auto eid = std::get<size_t>(properties.at("EID"));
                auto mctpTypes = std::get<std::vector<uint8_t>>(
                    properties.at("SupportedMessageTypes"));
                auto mediumType =
                    std::get<std::string>(properties.at("MediumType"));
                auto networkId = std::get<size_t>(properties.at("NetworkId"));
                if (std::find(mctpTypes.begin(), mctpTypes.end(),
                              mctp_vdm::MessageType) != mctpTypes.end())
                {
                    handler.registerMctpEndpoint(eid, type, protocol, address);
                    mctpInfos.emplace_back(
                        std::make_tuple(eid, uuid, mediumType, networkId));
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Error while getting properties.", "ERROR", e);
    }
}

template <typename T>
void MctpDiscovery<T>::discoverEndpoints(sdbusplus::message::message& msg)
{
    mctp::Infos mctpInfos;

    sdbusplus::message::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);
    std::string obPath = objPath;
    populateMctpInfo(interfaces, mctpInfos);

    handleMctpEndpoints(mctpInfos);
}

template <typename T>
void MctpDiscovery<T>::handleMctpEndpoints(const mctp::Infos& mctpInfos)
{
    for (MctpDiscoveryHandlerIntf* handler : handlers)
    {
        if (handler)
        {
            handler->handleMctpEndpoints(mctpInfos);
        }
    }
}

} // namespace mctp_vdm

#ifdef MCTP_IN_KERNEL
using TRequest = mctp_vdm::requester::InKernelRequest;
#else
using TRequest = mctp_vdm::requester::DaemonRequest;
#endif

template class mctp_vdm::MctpDiscovery<TRequest>;