#include "config.h"

#include "version.hpp"

#include "xyz/openbmc_project/Common/error.hpp"

#include <openssl/sha.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>

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

void Delete::delete_()
{
    if (parent.eraseCallback)
    {
        parent.eraseCallback(parent.getVersionId());
    }
}

////////////////////////////////Activation

namespace softwareServer = sdbusplus::xyz::openbmc_project::Software::server;

using namespace phosphor::logging;
using sdbusplus::exception::SdBusError;
using SoftwareActivation = softwareServer::Activation;

auto Version::activation(Activations value) -> Activations
{
    if (value == Status::Activating)
    {
        value = startActivation();
		std::cerr << "At "<< __func__ <<":" << __LINE__ <<":" << std::endl;
    }
    else
    {
        activationBlocksTransition.reset();
        activationProgress.reset();
		std::cerr << "At "<< __func__ <<":" << __LINE__ <<":" << std::endl;
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
	std::cerr << "At " <<__func__ << ":" << __LINE__ << std::endl;

    if (newStateUnit == deviceUpdateUnit)
    {
        if (newStateResult == "done")
        {
			std::cerr << "At " <<__func__<< ":" << __LINE__ << std::endl;
            onUpdateDone();
        }
        if (newStateResult == "failed" || newStateResult == "dependency")
        {
            onUpdateFailed();
			std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
        }
    }
}

void Version::removeAssociation(const std::string& path)
{
    // TODO remove this method later
    (void)path;
}

bool Version::doUpdate(const std::string& inventoryPath)
{
    currentUpdatingDevice = inventoryPath;
    deviceUpdateUnit = getUpdateService(currentUpdatingDevice);
	std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    try
    {
        auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                          SYSTEMD_INTERFACE, "StartUnit");
        method.append(deviceUpdateUnit, "replace");
        bus.call_noreply(method);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
        return true;
    }
    catch (const SdBusError& e)
    {
        log<level::ERR>("Error staring service", entry("ERROR=%s", e.what()));
        onUpdateFailed();
        return false;
    }
}

bool Version::doUpdate()
{
    // When the queue is empty, all updates are done
    if (deviceQueue.empty())
    {
		std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
        finishActivation();
		std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
        return true;
    }

	std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
    // Do the update on a device
    const auto& device = deviceQueue.front();
	std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
    return doUpdate(device);
}

void Version::onUpdateDone()
{
    auto progress = activationProgress->progress() + progressStep;
	std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
    activationProgress->progress(progress);

    // Update the activation association
    auto assocs = associations();
    assocs.emplace_back(ACTIVATION_FWD_ASSOCIATION, ACTIVATION_REV_ASSOCIATION,
                        currentUpdatingDevice);

    associations(assocs);

    auto eps = endpoints();
    eps.emplace_back(sdbusplus::message::object_path(currentUpdatingDevice));
    endpoints(eps);

    activationListener->onUpdateDone(getVersionId(), currentUpdatingDevice);
    currentUpdatingDevice.clear();
	std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;

    deviceQueue.pop();
	std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
    doUpdate(); // Update the next device
	std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
}

void Version::onUpdateFailed()
{
    // TODO: report an event
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

    if (itemUpdaterUtils->updateAllTogether() == false)
    {
		std::cerr << "At non-concurrent "<< __func__ <<":" << __LINE__ << std::endl;
        auto devicePaths = itemUpdaterUtils->getItemUpdaterInventoryPaths();
        if (devicePaths.empty())
        {
            log<level::WARNING>("No device inventory found");
            return Status::Failed;
        }
        for (const auto& p : devicePaths)
        {
            if (isCompatible(p))
            {
                if (isAssociated(p, associations()))
                {
                    log<level::NOTICE>(
                        "device already running the image, skipping",
                        entry("device=%s", p.c_str()));
                    continue;
                }
                deviceQueue.push(p);
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
    }
    else
    {
		std::cerr << "At concurrent "<< __func__ <<":" << __LINE__ << std::endl;
        deviceQueue.push("All"); // get this from itemUpdater
        progressStep = 80;
    }
    if (!activationProgress)
    {
        activationProgress = std::make_unique<ActivationProgress>(bus, objPath);
		std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
    }
    if (!activationBlocksTransition)
    {
        activationBlocksTransition =
            std::make_unique<ActivationBlocksTransition>(bus, objPath);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    }
    if (doUpdate())
    {
        activationProgress->progress(10);
		std::cerr << "At " <<__func__<< ":" << __LINE__ << std::endl;
        return Status::Activating;
    }
    else
    {
        return Status::Failed;
    }
}

void Version::finishActivation()
{
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    storeImage();
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    activationProgress->progress(100);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;

    createActiveAssociation(objPath);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    addFunctionalAssociation(objPath);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    addUpdateableAssociation(objPath);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    // Reset RequestedActivations to none so that it could be activated in
    // future
    requestedActivation(SoftwareActivation::RequestedActivations::None);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    activation(Status::Active);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
}
bool Version::isCompatible(const std::string& inventoryPath)
{
    auto service = getService(inventoryPath.c_str(), ASSET_IFACE);
    auto deviceManufacturer = getProperty<std::string>(
        service.c_str(), inventoryPath.c_str(), ASSET_IFACE, MANUFACTURER);
    auto deviceModel = getProperty<std::string>(
        service.c_str(), inventoryPath.c_str(), ASSET_IFACE, MODEL);

    if (deviceModel != model)
    {
        // The model shall match
        return false;
    }
    if (!deviceManufacturer.empty())
    {
        // If device inventory has manufacturer property, it shall match
        return deviceManufacturer == manufacturer;
    }
    return true;
}

void Version::storeImage()
{
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    // Store image in persistent only store the latest one by removing old ones
    auto src = path();
    auto dst = std::filesystem::path(IMG_DIR_PERSIST) /
               itemUpdaterUtils->getName() / uuid();
	std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    if (src == dst)
    {
        // This happens when updating an stored image, no need to store it again
        return;
    }
    try
    {
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
        std::filesystem::remove_all(dst);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
        std::filesystem::create_directories(dst);
		std::cerr << "At "<< __func__ <<":" << __LINE__ <<"SRC"<< src <<"DST " << dst << std::endl;
        //std::filesystem::copy(src, dst);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
        dst += std::filesystem::path(src).filename();
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
        path(dst.string()); // Update the FilePath interface
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        log<level::ERR>(
            "Error storing device image", entry("ERROR=%s", e.what()),
            entry("SRC=%s", src.c_str()), entry("DST=%s", dst.c_str()));
    }
	catch (const std::exception& e) 
	{
        log<level::ERR>(
            "Error storing device image", entry("ERROR=%s", e.what()));
	}
    // FIXME if not deleted then PLDM fail to extract image
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
    std::filesystem::remove(src);
		std::cerr << "At "<< __func__ <<":" << __LINE__ << std::endl;
}

std::string Version::getUpdateService(const std::string& inventoryPath)
{
		std::cerr << "At " <<__func__ <<":" << __LINE__ << std::endl;
    return itemUpdaterUtils->getUpdateServiceWithArgs(inventoryPath, path());
}

void ActivationBlocksTransition::enableRebootGuard()
{
    log<level::INFO>("device image activating - BMC reboots are disabled.");

    auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                      SYSTEMD_INTERFACE, "StartUnit");
    method.append("reboot-guard-enable.service", "replace");
    bus.call_noreply_noerror(method);
}

void ActivationBlocksTransition::disableRebootGuard()
{
    log<level::INFO>(
        "device activation has ended - BMC reboots are re-enabled.");

    auto method = bus.new_method_call(SYSTEMD_BUSNAME, SYSTEMD_PATH,
                                      SYSTEMD_INTERFACE, "StartUnit");
    method.append("reboot-guard-disable.service", "replace");
    //bus.call_noreply_noerror(method);
}

} // namespace updater
} // namespace software
} // namespace nvidia
