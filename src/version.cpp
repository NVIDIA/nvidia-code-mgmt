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

#include "config.h"

#include "version.hpp"

#include "xyz/openbmc_project/Common/error.hpp"

#include <openssl/sha.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{

using namespace sdbusplus::xyz::openbmc_project::Common::Error;

const std::string transferFailed{"Update.1.0.TransferFailed"};

void Delete::delete_()
{
    if (parent.eraseCallback)
    {
        parent.eraseCallback(parent.getVersionId());
    }
}

////////////////////////////////Activation

namespace softwareServer = sdbusplus::xyz::openbmc_project::Software::server;
namespace LoggingServer = sdbusplus::xyz::openbmc_project::Logging::server;
using sdbusplus::exception::SdBusError;
using SoftwareActivation = softwareServer::Activation;

auto Version::activation(Activations value) -> Activations
{
    if (value == Status::Activating)
    {
        value = startActivation();
    }
    else
    {
        activationProgress.reset();
    }

    return SoftwareActivation::activation(value);
}

auto Version::requestedActivation(RequestedActivations value)
    -> RequestedActivations
{
    if ((value == SoftwareActivation::RequestedActivations::Active) &&
        (SoftwareActivation::requestedActivation() !=
         SoftwareActivation::RequestedActivations::Active))
    {
        if ((activation() == Status::Ready) ||
            (activation() == Status::Failed) || activation() == Status::Active)
        {
            activation(Status::Activating);
        }
    }
    return SoftwareActivation::requestedActivation(value);
}

void Version::unitStateChange(sdbusplus::message::message& msg)
{
    uint32_t newStateID{};
    sdbusplus::message::object_path newStateObjPath;
    std::string newStateUnit{};
    std::string newStateResult{};

    // Read the msg and populate each variable
    msg.read(newStateID, newStateObjPath, newStateUnit, newStateResult);

    if (newStateUnit == deviceUpdateUnit)
    {
        if (newStateResult == "done")
        {
            onUpdateDone();
        }
        if (newStateResult == "failed" || newStateResult == "dependency")
        {
            onUpdateFailed();
        }
    }
}


bool Version::doUpdate()
{
    // When the queue is empty, all updates are done
    if (deviceQueue.empty())
    {
        finishActivation();
        return true;
    }

    // Do the update on a device
    const auto& device = deviceQueue.front();
    currentUpdatingDevice = device;
    deviceUpdateUnit = getUpdateService(currentUpdatingDevice);
    return itemUpdaterUtils->doUpdate(this, deviceUpdateUnit);
}

void Version::onUpdateDone()
{
    if (activationProgress)
    {
        auto progress = activationProgress->progress() + progressStep;
        activationProgress->progress(progress);

        deviceQueue.pop();
        doUpdate(); // Update the next device
    }
}

void Version::onUpdateFailed()
{
    logTransferFailed(itemUpdaterUtils->getName(), extendedVersion());
    log<level::ERR>("Failed to udpate device",
                    entry("device=%s", deviceQueue.front().c_str()));
    std::queue<std::string>().swap(deviceQueue); // Clear the queue
    activation(Status::Failed);
    std::filesystem::remove(path());
    itemUpdaterUtils->readExistingFirmWare();
    requestedActivation(SoftwareActivation::RequestedActivations::None);
}

Version::Status Version::startActivation()
{
    // Check if the activation has file path
    if (path().empty())
    {
        log<level::WARNING>("No image for the activation, skipped",
                            entry("VERSION_ID=%s", getVersionId().c_str()));
        return activation(); // Return the previous activation status
    }

    auto devicePaths = itemUpdaterUtils->getItemUpdaterInventoryPaths();
    if (devicePaths.empty())
    {
        log<level::WARNING>("No device inventory found");
        return Status::Failed;
    }
    // apply target filtering
    targetFilter =
        itemUpdaterUtils->applyTargetFilters(updatePolicy->targets());
    for (const auto& p : devicePaths)
    {
        if (isCompatible(p))
        {
            deviceQueue.push(p);
            if (itemUpdaterUtils->updateAllTogether())
            {
                log<level::NOTICE>("Updating all devices under",
                                   entry("device=%s", p.c_str()));
                break;
            }
        }
        else
        {
            log<level::NOTICE>("device not compatible",
                               entry("device=%s", p.c_str()));
        }
    }
    if (deviceQueue.empty())
    {
        log<level::WARNING>("No device compatible with the software");
        progressStep = 90;
    }
    else
    {
        progressStep = 80 / deviceQueue.size();
    }

    if (!activationProgress)
    {
        activationProgress = std::make_unique<ActivationProgress>(bus, objPath);
    }
    if (targetFilter.type == TargetFilterType::UpdateNone)
    {
        finishActivation();
        return Status::Active;
    }
    if (doUpdate())
    {
        activationProgress->progress(10);
        return Status::Activating;
    }
    else
    {
        return Status::Failed;
    }
}

void Version::finishActivation()
{
    activationProgress->progress(100);
    // Reset RequestedActivations to none so that it could be activated in
    // future
    requestedActivation(SoftwareActivation::RequestedActivations::None);
    activation(Status::Active);
    //remove file
    std::filesystem::remove(path());
}
bool Version::isCompatible(const std::string& inventoryPath)
{
    std::string deviceManufacturer;
    std::string deviceModel;
    // few other device updater like retimer, debug token does not have
    // inventory, skip compatibility check for those
    if (!itemUpdaterUtils->inventorySupported())
    {
        return true;
    }
    try
    {
        auto service =
            itemUpdaterUtils->getDbusService(inventoryPath, ASSET_IFACE);
        deviceManufacturer = getProperty<std::string>(
            service.c_str(), inventoryPath.c_str(), ASSET_IFACE, MANUFACTURER);
        deviceModel = getProperty<std::string>(
            service.c_str(), inventoryPath.c_str(), ASSET_IFACE, MODEL);
    }
    // For Retimer model is hardcoded by GpuMgr. Ignore for retimer
    // For Retimer manufacturer is not populated by GpuMgr. Ignore for retimer
    catch (const std::exception& e)
    {
        log<level::INFO>("Error while getting inventory properties",
                         entry("ERROR=%s", e.what()));
        std::cerr << e.what() << '\n';
    }

    // ignore if deviceModel is empty from GPU manager else it should match
    if (!deviceModel.empty() && deviceModel != model)
    {
        // The model shall match
        log<level::ERR>("Device model not matching",
                        entry("SYSMODEL=%s", deviceModel.c_str()),
                        entry("CFGMODEL=%s", model.c_str()));
        ;
        return false;
    }
    if (!deviceManufacturer.empty())
    {
        // If device inventory has manufacturer property, it shall match
        log<level::ERR>("Device manufacturer not matching",
                        entry("SYSMNFCTR=%s", deviceManufacturer.c_str()),
                        entry("CFGMNFCTR=%s", manufacturer.c_str()));
        ;
        return deviceManufacturer == manufacturer;
    }
    return true;
}

void Version::storeImage()
{
    // Store image in persistent only store the latest one by removing old ones
    auto src = path();
    auto dst = std::filesystem::path(IMG_DIR_PERSIST) /
               itemUpdaterUtils->getName() / uuid();
    if (src == dst)
    {
        // This happens when updating an stored image, no need to store it again
        return;
    }
    try
    {
        std::filesystem::remove_all(dst);
        std::filesystem::create_directories(dst);
        std::filesystem::copy(src, dst);
        dst += std::filesystem::path(src).filename();
        path(dst.string()); // Update the FilePath interface
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        log<level::ERR>(
            "Error storing device image", entry("ERROR=%s", e.what()),
            entry("SRC=%s", src.c_str()), entry("DST=%s", dst.c_str()));
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("Error storing device image",
                        entry("ERROR=%s", e.what()));
    }
    // FIXME if not deleted then PLDM fail to extract image
    std::filesystem::remove(src);
}

std::string Version::getUpdateService(const std::string& inventoryPath)
{
    return itemUpdaterUtils->getUpdateServiceWithArgs(
        inventoryPath, path(), extendedVersion(),
        targetFilter);
}

void Version::createLog(const std::string& messageID,
                        std::map<std::string, std::string>& addData,
                        Level& level)
{
    static constexpr auto logObjPath = "/xyz/openbmc_project/logging";
    static constexpr auto logInterface = "xyz.openbmc_project.Logging.Create";
    static constexpr auto service = "xyz.openbmc_project.Logging";
    try
    {
        auto severity = LoggingServer::convertForMessage(level);
        auto method =
            bus.new_method_call(service, logObjPath, logInterface, "Create");
        method.append(messageID, severity, addData);
        bus.call_noreply(method);
    }
    catch (const std::exception& e)
    {
        std::cerr
            << "Failed to create D-Bus log entry for message registry, ERROR="
            << e.what() << "\n";
    }
}

void Version::logTransferFailed(const std::string& compName,
                                const std::string& compVersion)
{
    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = transferFailed;
    addData["REDFISH_MESSAGE_ARGS"] = (compVersion + "," + compName);
    // use separate container for fwupdate message registry
    addData["namespace"] = "FWUpdate";
    Level level = Level::Critical;
    createLog(transferFailed, addData, level);
    return;
}
} // namespace updater
} // namespace software
} // namespace nvidia
