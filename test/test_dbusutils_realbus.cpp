#include "config.h"

#include "gtest/gtest.h"
#undef FAIL

#define private public
#define protected public
#include "../src/base_item_updater.hpp"
#undef private
#undef protected

#include <systemd/sd-bus.h>

#include <any>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace nvidia::software::updater;

namespace
{

constexpr auto inventoryService = "xyz.openbmc_project.Inventory.Manager";
constexpr auto inventoryPath = "/xyz/openbmc_project/inventory/system/device0";
constexpr auto modelMismatchPath =
    "/xyz/openbmc_project/inventory/system/model_mismatch";
constexpr auto manufacturerMismatchPath =
    "/xyz/openbmc_project/inventory/system/manufacturer_mismatch";
constexpr auto emptyManufacturerPath =
    "/xyz/openbmc_project/inventory/system/empty_manufacturer";
constexpr auto softwarePath = "/xyz/openbmc_project/software/other/test";
constexpr auto chassisService = "xyz.openbmc_project.State.Chassis";
constexpr auto chassisPath = "/xyz/openbmc_project/state/chassis0";
constexpr auto chassisIface = "xyz.openbmc_project.State.Chassis";
constexpr auto testService = "xyz.openbmc_project.TestService";
constexpr auto managerPath = "/test/manager";

using DbusVariant = std::variant<std::string, bool, uint8_t, uint16_t, int16_t,
                                 uint32_t, int32_t, uint64_t, int64_t, double>;

struct OwnedMessage
{
    sdbusplus::message::message msg;
};

sd_bus_message* makeRawSignal(sdbusplus::bus_t& bus, const char* path)
{
    auto msg = bus.new_signal(path, "a.b", "TestSignal");
    return msg.release();
}

void finalizeMessage(OwnedMessage& owned, sd_bus_message* rawMsg)
{
    sd_bus_message_seal(rawMsg, 0, 0);
    sd_bus_message_rewind(rawMsg, true);
    owned.msg = sdbusplus::message::message(rawMsg, std::false_type{});
}

OwnedMessage makeInterfacesAddedMessage(sdbusplus::bus_t& bus,
                                        const char* signalPath,
                                        const char* objectPath,
                                        const char* interface)
{
    OwnedMessage owned;
    auto* rawMsg = makeRawSignal(bus, signalPath);

    if (sd_bus_message_append_basic(rawMsg, 'o', objectPath) < 0)
    {
        throw std::runtime_error("Failed to append object path");
    }
    if (sd_bus_message_open_container(rawMsg, 'a', "{sa{sv}}") < 0 ||
        sd_bus_message_open_container(rawMsg, 'e', "sa{sv}") < 0 ||
        sd_bus_message_append_basic(rawMsg, 's', interface) < 0 ||
        sd_bus_message_open_container(rawMsg, 'a', "{sv}") < 0 ||
        sd_bus_message_close_container(rawMsg) < 0 ||
        sd_bus_message_close_container(rawMsg) < 0 ||
        sd_bus_message_close_container(rawMsg) < 0)
    {
        throw std::runtime_error("Failed to append InterfacesAdded payload");
    }

    finalizeMessage(owned, rawMsg);
    return owned;
}

void appendProperty(sd_bus_message* rawMsg, const char* key,
                    const std::variant<std::string, bool>& value)
{
    if (sd_bus_message_open_container(rawMsg, 'e', "sv") < 0)
    {
        throw std::runtime_error("Failed to open property entry");
    }

    if (sd_bus_message_append_basic(rawMsg, 's', key) < 0)
    {
        throw std::runtime_error("Failed to append property name");
    }

    if (std::holds_alternative<std::string>(value))
    {
        const auto& str = std::get<std::string>(value);
        const char* raw = str.c_str();
        if (sd_bus_message_open_container(rawMsg, 'v', "s") < 0 ||
            sd_bus_message_append_basic(rawMsg, 's', raw) < 0 ||
            sd_bus_message_close_container(rawMsg) < 0)
        {
            throw std::runtime_error("Failed to append string property");
        }
    }
    else
    {
        int boolean = std::get<bool>(value);
        if (sd_bus_message_open_container(rawMsg, 'v', "b") < 0 ||
            sd_bus_message_append_basic(rawMsg, 'b', &boolean) < 0 ||
            sd_bus_message_close_container(rawMsg) < 0)
        {
            throw std::runtime_error("Failed to append boolean property");
        }
    }

    if (sd_bus_message_close_container(rawMsg) < 0)
    {
        throw std::runtime_error("Failed to close property entry");
    }
}

OwnedMessage makePropertiesChangedMessage(sdbusplus::bus_t& bus,
                                          const char* path,
                                          const char* interface,
                                          const Properties& properties)
{
    OwnedMessage owned;
    auto* rawMsg = makeRawSignal(bus, path);

    if (sd_bus_message_append_basic(rawMsg, 's', interface) < 0 ||
        sd_bus_message_open_container(rawMsg, 'a', "{sv}") < 0)
    {
        throw std::runtime_error("Failed to start PropertiesChanged payload");
    }

    for (const auto& [key, value] : properties)
    {
        appendProperty(rawMsg, key.c_str(), value);
    }

    if (sd_bus_message_close_container(rawMsg) < 0 ||
        sd_bus_message_open_container(rawMsg, 'a', "s") < 0 ||
        sd_bus_message_close_container(rawMsg) < 0)
    {
        throw std::runtime_error("Failed to finish PropertiesChanged payload");
    }

    finalizeMessage(owned, rawMsg);
    return owned;
}

OwnedMessage makeJobRemovedMessage(sdbusplus::bus_t& bus, const char* path,
                                   uint32_t jobId, const char* jobPath,
                                   const char* unit, const char* result)
{
    OwnedMessage owned;
    auto* rawMsg = makeRawSignal(bus, path);
    if (sd_bus_message_append(rawMsg, "uoss", jobId, jobPath, unit, result) < 0)
    {
        throw std::runtime_error("Failed to append JobRemoved payload");
    }
    finalizeMessage(owned, rawMsg);
    return owned;
}

class RealBusUpdater : public BaseItemUpdater
{
  public:
    RealBusUpdater(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, "NVIDIA:Model-A:uuid-1", ITEM_IFACE, "Updater",
                        "bus", "svc@.service", false, inventoryService)
    {}

    using BaseItemUpdater::getDbusService;
    using BaseItemUpdater::getItemUpdaterInventoryPaths;
    using BaseItemUpdater::newDeviceAdded;
    using BaseItemUpdater::onInventoryChangedMsg;
    using BaseItemUpdater::onReqActivationChangedMsg;
    using BaseItemUpdater::readDeviceDetails;
    using BaseItemUpdater::watchNewlyAddedDevice;

    std::string getVersion(const std::string&) const override
    {
        return "Version123";
    }

    std::string getManufacturer(const std::string&) const override
    {
        return "NVIDIA";
    }

    std::string getModel(const std::string&) const override
    {
        return "Model-A";
    }

    std::string getServiceArgs(const std::string&, const std::string&,
                               const std::string&,
                               const TargetFilter&) const override
    {
        return "args";
    }

    std::string getIdProperty(const std::string&) override
    {
        return "Version123";
    }

    bool inventorySupported() override
    {
        return false;
    }

    void readDeviceDetails(std::string& path) override
    {
        readDeviceCalls.push_back(path);
    }

    size_t versionCount() const
    {
        return versions.size();
    }

    Version* findVersion(const std::string& id) const
    {
        auto it = versions.find(id);
        return (it == versions.end()) ? nullptr : it->second.get();
    }

    std::vector<std::string> readDeviceCalls;
};

class FilteringRealBusUpdater : public RealBusUpdater
{
  public:
    using BaseItemUpdater::getItemUpdaterInventoryPaths;
    using RealBusUpdater::RealBusUpdater;

    bool pathValidValue = true;

    bool pathIsValidDevice(std::string& path) override
    {
        (void)path;
        return pathValidValue;
    }
};

class AssetRealBusUpdater : public RealBusUpdater
{
  public:
    using BaseItemUpdater::createVersion;
    using RealBusUpdater::RealBusUpdater;

    bool inventorySupported() override
    {
        return true;
    }
};

TEST(DBUSUtilsRealBus, ExercisesSuccessPaths)
{
    auto bus = sdbusplus::bus::new_default();
    DBUSUtils dbusUtils(bus);

    auto inventoryPaths = dbusUtils.getinventoryPath(ITEM_IFACE);
    ASSERT_EQ(inventoryPaths.size(), 1u);
    EXPECT_EQ(inventoryPaths.front(), inventoryPath);

    auto services = dbusUtils.getServices(inventoryPath, ITEM_IFACE);
    ASSERT_EQ(services.size(), 1u);
    EXPECT_EQ(services.front(), inventoryService);
    EXPECT_EQ(dbusUtils.getService(inventoryPath, ITEM_IFACE),
              inventoryService);

    auto property = std::any_cast<Value>(dbusUtils.getPropertyImpl(
        chassisService, chassisPath, chassisIface, "CurrentPowerState"));
    EXPECT_EQ(std::get<std::string>(property), hostOn);
    EXPECT_EQ(dbusUtils.getHostPwrStatus(), hostOn);

    auto softwareObjects = dbusUtils.getSoftwareObjects();
    ASSERT_EQ(softwareObjects.size(), 1u);
    EXPECT_EQ(softwareObjects.front(), softwarePath);

    std::string existingSoftware = softwarePath;
    EXPECT_TRUE(dbusUtils.findSoftwareObject(existingSoftware));
    std::string missingSoftware = std::string(softwarePath) + "-missing";
    EXPECT_FALSE(dbusUtils.findSoftwareObject(missingSoftware));

    auto managedObjects = dbusUtils.getManagedObjects(testService, managerPath);
    ASSERT_EQ(managedObjects.size(), 1u);

    std::map<std::string, std::string> addData{
        {"REDFISH_MESSAGE_ID", "Test.Message"}, {"namespace", "FWUpdate"}};
    Level infoLevel = Level::Informational;
    EXPECT_NO_THROW(dbusUtils.createLog("Test.Message", addData, infoLevel));
    EXPECT_NO_THROW(dbusUtils.createMessageRegistryResourceErrors(
        "Update.1.0.TransferFailed", "GPU_0", "Transfer failed",
        "Retry update"));
    EXPECT_NO_THROW(dbusUtils.startSystemUnit("example.service"));
    EXPECT_NO_THROW(dbusUtils.restartSystemUnit("example.service"));

    RealBusUpdater updater(bus);
    auto updaterInventoryPaths = updater.getItemUpdaterInventoryPaths();
    ASSERT_EQ(updaterInventoryPaths.size(), 1u);
    EXPECT_EQ(updaterInventoryPaths.front(), inventoryPath);
    EXPECT_EQ(updater.getDbusService(inventoryPath, ITEM_IFACE),
              inventoryService);
    EXPECT_EQ(updater.BaseItemUpdater::getTimeout(), NON_PLDM_DEFAULT_TIMEOUT);
    EXPECT_TRUE(updater.BaseItemUpdater::inventorySupported());
    EXPECT_FALSE(updater.BaseItemUpdater::needVerify());
    EXPECT_NO_THROW(updater.watchNewlyAddedDevice());

    auto deviceAdded = makeInterfacesAddedMessage(
        bus, "/xyz/openbmc_project/inventory", inventoryPath, ITEM_IFACE);
    updater.newDeviceAdded(deviceAdded.msg);

    std::string directInventoryPath = inventoryPath;
    updater.BaseItemUpdater::readDeviceDetails(directInventoryPath);
    EXPECT_EQ(updater.versionCount(), 1u);

    auto inventoryChanged = makePropertiesChangedMessage(
        bus, inventoryPath, ITEM_IFACE,
        Properties{{PRESENT, true},
                   {MODEL, std::string("Model-A")},
                   {MANUFACTURER, std::string("NVIDIA")}});
    updater.onInventoryChangedMsg(inventoryChanged.msg);

    auto* version = updater.findVersion("Version123");
    ASSERT_NE(version, nullptr);

    auto activationChanged = makePropertiesChangedMessage(
        bus, "/xyz/openbmc_project/software/Version123", ACTIVATE_INTERFACE,
        Properties{{"RequestedActivation",
                    NSActivation::convertRequestedActivationsToString(
                        NSActivation::RequestedActivations::Active)}});
    updater.onReqActivationChangedMsg(activationChanged.msg);

    EXPECT_EQ(version->activation(), Version::Status::Activating);

    auto jobRemoved = makeJobRemovedMessage(bus, SYSTEMD_PATH, 1,
                                            "/org/freedesktop/systemd1/job/1",
                                            "svc@args.service", "done");
    version->unitStateChange(jobRemoved.msg);
    EXPECT_EQ(version->activation(), Version::Status::Active);
}

TEST(DBUSUtilsRealBus, JobRemovedIgnoresDifferentUnit)
{
    auto bus = sdbusplus::bus::new_default();
    RealBusUpdater updater(bus);

    std::string directInventoryPath = inventoryPath;
    updater.BaseItemUpdater::readDeviceDetails(directInventoryPath);

    auto* version = updater.findVersion("Version123");
    ASSERT_NE(version, nullptr);

    auto activationChanged = makePropertiesChangedMessage(
        bus, "/xyz/openbmc_project/software/Version123", ACTIVATE_INTERFACE,
        Properties{{"RequestedActivation",
                    NSActivation::convertRequestedActivationsToString(
                        NSActivation::RequestedActivations::Active)}});
    updater.onReqActivationChangedMsg(activationChanged.msg);
    ASSERT_EQ(version->activation(), Version::Status::Activating);

    auto jobRemoved = makeJobRemovedMessage(bus, SYSTEMD_PATH, 1,
                                            "/org/freedesktop/systemd1/job/1",
                                            "svc@other.service", "done");
    version->unitStateChange(jobRemoved.msg);

    EXPECT_EQ(version->activation(), Version::Status::Activating);
}

TEST(DBUSUtilsRealBus, JobRemovedIgnoresUnhandledResult)
{
    auto bus = sdbusplus::bus::new_default();
    RealBusUpdater updater(bus);

    std::string directInventoryPath = inventoryPath;
    updater.BaseItemUpdater::readDeviceDetails(directInventoryPath);

    auto* version = updater.findVersion("Version123");
    ASSERT_NE(version, nullptr);

    auto activationChanged = makePropertiesChangedMessage(
        bus, "/xyz/openbmc_project/software/Version123", ACTIVATE_INTERFACE,
        Properties{{"RequestedActivation",
                    NSActivation::convertRequestedActivationsToString(
                        NSActivation::RequestedActivations::Active)}});
    updater.onReqActivationChangedMsg(activationChanged.msg);
    ASSERT_EQ(version->activation(), Version::Status::Activating);

    auto jobRemoved = makeJobRemovedMessage(bus, SYSTEMD_PATH, 1,
                                            "/org/freedesktop/systemd1/job/1",
                                            "svc@args.service", "running");
    version->unitStateChange(jobRemoved.msg);

    EXPECT_EQ(version->activation(), Version::Status::Activating);
}

TEST(DBUSUtilsRealBus, JobRemovedDependencyMarksActivationFailed)
{
    auto bus = sdbusplus::bus::new_default();
    RealBusUpdater updater(bus);

    std::string directInventoryPath = inventoryPath;
    updater.BaseItemUpdater::readDeviceDetails(directInventoryPath);

    auto* version = updater.findVersion("Version123");
    ASSERT_NE(version, nullptr);

    auto activationChanged = makePropertiesChangedMessage(
        bus, "/xyz/openbmc_project/software/Version123", ACTIVATE_INTERFACE,
        Properties{{"RequestedActivation",
                    NSActivation::convertRequestedActivationsToString(
                        NSActivation::RequestedActivations::Active)}});
    updater.onReqActivationChangedMsg(activationChanged.msg);
    ASSERT_EQ(version->activation(), Version::Status::Activating);

    auto jobRemoved = makeJobRemovedMessage(bus, SYSTEMD_PATH, 1,
                                            "/org/freedesktop/systemd1/job/1",
                                            "svc@args.service", "dependency");
    version->unitStateChange(jobRemoved.msg);

    EXPECT_EQ(version->activation(), Version::Status::Failed);
}

TEST(DBUSUtilsRealBus, JobRemovedFailedMarksActivationFailed)
{
    auto bus = sdbusplus::bus::new_default();
    RealBusUpdater updater(bus);

    std::string directInventoryPath = inventoryPath;
    updater.BaseItemUpdater::readDeviceDetails(directInventoryPath);

    auto* version = updater.findVersion("Version123");
    ASSERT_NE(version, nullptr);

    auto activationChanged = makePropertiesChangedMessage(
        bus, "/xyz/openbmc_project/software/Version123", ACTIVATE_INTERFACE,
        Properties{{"RequestedActivation",
                    NSActivation::convertRequestedActivationsToString(
                        NSActivation::RequestedActivations::Active)}});
    updater.onReqActivationChangedMsg(activationChanged.msg);
    ASSERT_EQ(version->activation(), Version::Status::Activating);

    auto jobRemoved = makeJobRemovedMessage(bus, SYSTEMD_PATH, 1,
                                            "/org/freedesktop/systemd1/job/1",
                                            "svc@args.service", "failed");
    version->unitStateChange(jobRemoved.msg);

    EXPECT_EQ(version->activation(), Version::Status::Failed);
}

TEST(DBUSUtilsRealBus, BaseItemUpdaterFiltersInvalidInventoryPath)
{
    auto bus = sdbusplus::bus::new_default();
    FilteringRealBusUpdater updater(bus);
    updater.pathValidValue = false;

    EXPECT_TRUE(updater.getItemUpdaterInventoryPaths().empty());
}

TEST(DBUSUtilsRealBus, VersionCompatibilityRejectsModelMismatch)
{
    auto bus = sdbusplus::bus::new_default();
    AssetRealBusUpdater updater(bus);

    auto version = updater.createVersion(
        "/xyz/openbmc_project/software/Version123", "Version123", "Version123",
        "uuid-1", "/tmp/image.bin", Version::Status::Ready);

    EXPECT_FALSE(version->isCompatible(modelMismatchPath));
}

TEST(DBUSUtilsRealBus, VersionCompatibilityRejectsManufacturerMismatch)
{
    auto bus = sdbusplus::bus::new_default();
    AssetRealBusUpdater updater(bus);

    auto version = updater.createVersion(
        "/xyz/openbmc_project/software/Version123", "Version123", "Version123",
        "uuid-1", "/tmp/image.bin", Version::Status::Ready);

    EXPECT_FALSE(version->isCompatible(manufacturerMismatchPath));
}

TEST(DBUSUtilsRealBus,
     VersionCompatibilityAllowsEmptyManufacturerWhenModelMatches)
{
    auto bus = sdbusplus::bus::new_default();
    AssetRealBusUpdater updater(bus);

    auto version = updater.createVersion(
        "/xyz/openbmc_project/software/Version123", "Version123", "Version123",
        "uuid-1", "/tmp/image.bin", Version::Status::Ready);

    EXPECT_TRUE(version->isCompatible(emptyManufacturerPath));
}

} // namespace
