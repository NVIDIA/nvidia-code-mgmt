/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#include "../src/base_controller.hpp"
#include "../src/base_item_updater.hpp"
#include "../src/debug_token_erase.hpp"
#include "../src/debug_token_install.hpp"

#include <systemd/sd-event.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using ::testing::NiceMock;
using namespace nvidia::software::updater;

namespace
{

using DbusVariant = std::variant<std::string, bool, uint8_t, uint16_t, int16_t,
                                 uint32_t, int32_t, uint64_t, int64_t, double>;

class ScopedTempDir
{
  public:
    ScopedTempDir()
    {
        std::array<char, 64> templ{};
        std::snprintf(templ.data(), templ.size(), "/tmp/updaterXXXXXX");
        char* created = mkdtemp(templ.data());
        if (created == nullptr)
        {
            throw std::runtime_error("Failed to create temporary directory");
        }
        dir = created;
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::filesystem::path path() const
    {
        return dir;
    }

  private:
    std::filesystem::path dir;
};

void writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << content;
}

struct OwnedMessage
{
    sdbusplus::message::message msg;
};

template <typename... Args>
OwnedMessage makeSignalMessage(sdbusplus::bus_t& bus, const char* path,
                               Args&&... args)
{
    OwnedMessage owned;
    auto raw = bus.new_signal(path, "a.b", "TestSignal");
    auto* rawMsg = raw.release();
    owned.msg = sdbusplus::message::message(rawMsg, std::false_type{});
    owned.msg.append(std::forward<Args>(args)...);
    sd_bus_message_seal(rawMsg, 0, 0);
    sd_bus_message_rewind(rawMsg, true);
    return owned;
}

class ProcessCaptureUpdater : public BaseItemUpdater
{
  public:
    struct InitiateCall
    {
        std::string objPath;
        std::string filePath;
        std::string versionStr;
        std::string versionId;
        std::string uniqueIdentifier;
    };

    ProcessCaptureUpdater(sdbusplus::bus_t& bus,
                          const std::string& supportedDevices =
                              "NVIDIA:Model-A:uuid-1|NVIDIA:Model-B:uuid-2") :
        BaseItemUpdater(bus, supportedDevices, ITEM_IFACE, "Updater", "bus",
                        "svc@.service", false, "inventory.bus")
    {}

    using BaseItemUpdater::getPathsToMonitor;
    using BaseItemUpdater::processImage;
    using BaseItemUpdater::readExistingFirmWare;

    std::string idToReturn{"Version123"};
    int initiateReturn = 0;
    InitiateCall lastInitiate{};
    std::vector<InitiateCall> initiateCalls;
    std::vector<std::string> inventoryPaths;
    std::vector<std::string> readDeviceCalls;

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        return inventoryPaths;
    }

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
        return idToReturn;
    }

    int initiateUpdateImage(const std::string& objPath,
                            const std::string& filePath,
                            const std::string& versionStr,
                            const std::string& versionId,
                            const std::string& uniqueIdentifier) override
    {
        lastInitiate = {objPath, filePath, versionStr, versionId,
                        uniqueIdentifier};
        initiateCalls.push_back(lastInitiate);
        return initiateReturn;
    }

    void readDeviceDetails(std::string& path) override
    {
        readDeviceCalls.push_back(path);
    }
};

class TestableItemUpdater : public BaseItemUpdater
{
  public:
    TestableItemUpdater(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, "NVIDIA:Model-A:uuid-1", ITEM_IFACE, "Updater",
                        "bus", "svc@.service", false, "inventory.bus")
    {}

    using BaseItemUpdater::activationMatches;
    using BaseItemUpdater::applyTargetFilters;
    using BaseItemUpdater::cleanupImageUploadDir;
    using BaseItemUpdater::createSoftwareObject;
    using BaseItemUpdater::createVersion;
    using BaseItemUpdater::erase;
    using BaseItemUpdater::getBusName;
    using BaseItemUpdater::getDbusService;
    using BaseItemUpdater::getIdProperty;
    using BaseItemUpdater::getServiceName;
    using BaseItemUpdater::getTimeout;
    using BaseItemUpdater::getUpdateServiceWithArgs;
    using BaseItemUpdater::getUUID;
    using BaseItemUpdater::initiateUpdateImage;
    using BaseItemUpdater::insertToUUIDMap;
    using BaseItemUpdater::inventoryObjectStatusMap;
    using BaseItemUpdater::needVerify;
    using BaseItemUpdater::newDeviceAdded;
    using BaseItemUpdater::onInventoryChanged;
    using BaseItemUpdater::onInventoryChangedMsg;
    using BaseItemUpdater::onReqActivationChanged;
    using BaseItemUpdater::onReqActivationChangedMsg;
    using BaseItemUpdater::pathIsValidDevice;
    using BaseItemUpdater::updateAllTogether;
    using BaseItemUpdater::validateTarget;
    using BaseItemUpdater::versions;

    std::string versionForPath{"Version123"};
    std::string manufacturerForPath{"NVIDIA"};
    std::string modelForPath{"Model-A"};
    std::string idToReturn{"Version123"};
    std::vector<std::string> inventoryPaths;
    std::vector<std::string> readDeviceCalls;
    bool inventorySupportedValue = true;
    bool pathValidValue = true;
    bool forceEmptyValidatedTarget = false;

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        return inventoryPaths;
    }

    std::string getVersion(const std::string&) const override
    {
        return versionForPath;
    }

    std::string getManufacturer(const std::string&) const override
    {
        return manufacturerForPath;
    }

    std::string getModel(const std::string&) const override
    {
        return modelForPath;
    }

    std::string getServiceArgs(const std::string&, const std::string&,
                               const std::string&,
                               const TargetFilter&) const override
    {
        return "args";
    }

    std::string getIdProperty(const std::string&) override
    {
        return idToReturn;
    }

    bool inventorySupported() override
    {
        return inventorySupportedValue;
    }

    bool pathIsValidDevice(std::string& p) override
    {
        readDeviceCalls.push_back("validate:" + p);
        return pathValidValue;
    }

    std::string validateTarget(const sdbusplus::object_path& target) override
    {
        if (forceEmptyValidatedTarget)
        {
            return {};
        }
        return BaseItemUpdater::validateTarget(target);
    }

    void readDeviceDetails(std::string& path) override
    {
        readDeviceCalls.push_back(path);
    }

    std::size_t versionCount() const
    {
        return versions.size();
    }

    std::size_t activationMatchCount() const
    {
        return activationMatches.size();
    }
};

class FilteringItemUpdater : public BaseItemUpdater
{
  public:
    FilteringItemUpdater(sdbusplus::bus_t& bus,
                         const std::string& service = "svc@.service") :
        BaseItemUpdater(bus, "NVIDIA:Model-A:uuid-1", ITEM_IFACE, "Updater",
                        "bus", service, false, "inventory.bus")
    {}

    using BaseItemUpdater::getItemUpdaterInventoryPaths;
    using BaseItemUpdater::getUpdateServiceWithArgs;

    bool pathValidValue = true;

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

    bool pathIsValidDevice(std::string& path) override
    {
        (void)path;
        return pathValidValue;
    }
};

class ControllerUpdater : public BaseItemUpdater
{
  public:
    ControllerUpdater(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, "NVIDIA:Model-A:uuid-1", ITEM_IFACE, "Updater",
                        "bus", "svc@.service", false, "inventory.bus")
    {}

    int processResult = 0;
    int processCalls = 0;
    int watchCalls = 0;
    int readExistingCalls = 0;
    std::filesystem::path lastPath;

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        return {};
    }

    std::string getVersion(const std::string&) const override
    {
        return {};
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

    int processImage(std::filesystem::path& path) override
    {
        ++processCalls;
        lastPath = path;
        return processResult;
    }

    void watchNewlyAddedDevice() override
    {
        ++watchCalls;
    }

    void readExistingFirmWare() override
    {
        ++readExistingCalls;
    }
};

class NoUuidUpdater : public BaseItemUpdater
{
  public:
    NoUuidUpdater(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, "", ITEM_IFACE, "Updater", "bus", "svc@.service",
                        false, "inventory.bus")
    {}

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        return {};
    }

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
};

class ThrowingWatchUpdater : public ControllerUpdater
{
  public:
    using ControllerUpdater::ControllerUpdater;

    std::vector<std::filesystem::path> getPathsToMonitor() const override
    {
        throw std::runtime_error("watch setup failed");
    }
};

class WatchingUpdater : public ControllerUpdater
{
  public:
    using ControllerUpdater::ControllerUpdater;

    std::vector<std::filesystem::path> pathsToMonitor;

    std::vector<std::filesystem::path> getPathsToMonitor() const override
    {
        return pathsToMonitor;
    }
};

extern "C" int __wrap_sd_event_loop(sd_event*)
{
    return 0;
}

class BaseUpdaterControllerTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
};

} // namespace

TEST_F(BaseUpdaterControllerTest, GetPathsToMonitorParsesSupportedDevices)
{
    ProcessCaptureUpdater updater(bus);

    auto paths = updater.getPathsToMonitor();
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], std::filesystem::path(IMG_UPLOAD_DIR_BASE) / "Updater" /
                            "uuid-1");
    EXPECT_EQ(paths[1], std::filesystem::path(IMG_UPLOAD_DIR_BASE) / "Updater" /
                            "uuid-2");
}

TEST_F(BaseUpdaterControllerTest, GetPathsToMonitorThrowsWhenNoDevicesExist)
{
    ProcessCaptureUpdater updater(bus, "");
    EXPECT_THROW(static_cast<void>(updater.getPathsToMonitor()),
                 std::runtime_error);
}

TEST_F(BaseUpdaterControllerTest, BaseItemUpdaterAccessorHelpersExposeDefaults)
{
    TestableItemUpdater updater(bus);

    EXPECT_EQ(updater.getName(), "Updater");
    EXPECT_EQ(updater.getImageUploadDir(),
              std::string(IMG_UPLOAD_DIR_BASE) + "Updater/");
    EXPECT_EQ(updater.getBusName(), "bus");
    EXPECT_EQ(updater.getServiceName(), "svc@.service");
    EXPECT_FALSE(updater.updateAllTogether());
    EXPECT_EQ(updater.getTimeout(), NON_PLDM_DEFAULT_TIMEOUT);
    EXPECT_TRUE(updater.inventorySupported());
    EXPECT_FALSE(updater.needVerify());
    std::string devicePath = "/xyz/device";
    EXPECT_TRUE(updater.pathIsValidDevice(devicePath));
    EXPECT_EQ(updater.validateTarget(sdbusplus::object_path(
                  "/xyz/openbmc_project/inventory/gpu0")),
              "gpu0");
    EXPECT_EQ(updater.getIdProperty("ignored"), "Version123");
    EXPECT_EQ(updater.getUpdateServiceWithArgs(
                  "/xyz/openbmc_project/inventory/gpu0", "/tmp/image.bin",
                  "1.2.3", {TargetFilterType::UpdateAll, {}}),
              "svc@args.service");
    EXPECT_EQ(updater.getUUID("Model-A", "NVIDIA"), "uuid-1");
    EXPECT_EQ(updater.getUUID("Model-A", "Other"), "uuid-1");
    EXPECT_EQ(updater.getUUID("Missing", "Missing"), "uuid-1");

    updater.insertToUUIDMap("uuid-9", "Model-Z", "ACME");
    EXPECT_EQ(updater.getUUID("Model-Z", "ACME"), "uuid-9");
}

TEST_F(BaseUpdaterControllerTest, ProcessImageFailsWhenVersionIdIsMissing)
{
    ProcessCaptureUpdater updater(bus);
    updater.idToReturn.clear();

    auto imagePath = std::filesystem::path(updater.getImageUploadDir()) /
                     "uuid-1" / "firmware.bin";
    EXPECT_EQ(updater.processImage(imagePath), -1);
}

TEST_F(BaseUpdaterControllerTest, ProcessImagePassesExpectedArgumentsToInitiate)
{
    ProcessCaptureUpdater updater(bus);
    updater.initiateReturn = 7;

    auto imagePath = std::filesystem::path(updater.getImageUploadDir()) /
                     "uuid-1" / "1.2.3.bin";
    EXPECT_EQ(updater.processImage(imagePath), 7);
    EXPECT_EQ(updater.lastInitiate.objPath,
              std::string(SOFTWARE_OBJPATH) + "/Version123");
    EXPECT_EQ(updater.lastInitiate.filePath, imagePath.string());
    EXPECT_EQ(updater.lastInitiate.versionStr, "1.2.3");
    EXPECT_EQ(updater.lastInitiate.versionId, "Version123");
    EXPECT_EQ(updater.lastInitiate.uniqueIdentifier, "uuid-1");
}

TEST_F(BaseUpdaterControllerTest, ReadExistingFirmwareVisitsKnownInventoryPaths)
{
    ProcessCaptureUpdater updater(bus);
    updater.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0",
                              "/xyz/openbmc_project/inventory/gpu1"};

    updater.readExistingFirmWare();
    ASSERT_EQ(updater.readDeviceCalls.size(), 2u);
    EXPECT_EQ(updater.readDeviceCalls[0],
              "/xyz/openbmc_project/inventory/gpu0");
    EXPECT_EQ(updater.readDeviceCalls[1],
              "/xyz/openbmc_project/inventory/gpu1");
}

TEST_F(BaseUpdaterControllerTest, ReadExistingFirmwareProcessesPersistentFiles)
{
    ProcessCaptureUpdater updater(bus);
    const auto persistDir = std::filesystem::path(IMG_DIR_PERSIST);
    const auto imagePath = persistDir / "test-base-updater-controller.bin";

    std::error_code ec;
    std::filesystem::create_directories(persistDir, ec);
    if (ec)
    {
        GTEST_SKIP() << "persistent image directory is not writable: "
                     << ec.message();
    }
    writeFile(imagePath, "payload");

    updater.readExistingFirmWare();

    auto found = std::find_if(
        updater.initiateCalls.begin(), updater.initiateCalls.end(),
        [&imagePath](const ProcessCaptureUpdater::InitiateCall& call) {
            return call.filePath == imagePath.string();
        });
    EXPECT_NE(found, updater.initiateCalls.end());

    std::filesystem::remove(imagePath);
}

TEST_F(BaseUpdaterControllerTest,
       CreateSoftwareObjectAddsVersionAndEraseRemovesIt)
{
    TestableItemUpdater updater(bus);

    updater.createSoftwareObject("/xyz/openbmc_project/inventory/gpu0",
                                 "Version123");
    EXPECT_EQ(updater.versionCount(), 1u);
    EXPECT_TRUE(updater.versions.contains("Version123"));

    updater.erase("Version123");
    EXPECT_EQ(updater.versionCount(), 0u);
}

TEST_F(BaseUpdaterControllerTest, EraseIgnoresMissingVersion)
{
    TestableItemUpdater updater(bus);
    updater.erase("missing-version");
    EXPECT_EQ(updater.versionCount(), 0u);
}

TEST_F(BaseUpdaterControllerTest, CreateSoftwareObjectFallsBackToDefaultUuid)
{
    TestableItemUpdater updater(bus);
    updater.modelForPath = "OtherModel";

    updater.createSoftwareObject("/xyz/openbmc_project/inventory/gpu0",
                                 "Version123");
    EXPECT_EQ(updater.versionCount(), 1u);
}

TEST_F(BaseUpdaterControllerTest, CreateSoftwareObjectSkipsWhenUuidIsMissing)
{
    NoUuidUpdater updater(bus);

    updater.createSoftwareObject("/xyz/openbmc_project/inventory/gpu0",
                                 "Version123");
}

TEST_F(BaseUpdaterControllerTest, CreateSoftwareObjectDoesNotDuplicateVersion)
{
    TestableItemUpdater updater(bus);

    updater.createSoftwareObject("/xyz/openbmc_project/inventory/gpu0",
                                 "Version123");
    updater.createSoftwareObject("/xyz/openbmc_project/inventory/gpu0",
                                 "Version123");
    EXPECT_EQ(updater.versionCount(), 1u);
}

TEST_F(BaseUpdaterControllerTest, InitiateUpdateImageAddsReadyVersion)
{
    TestableItemUpdater updater(bus);

    EXPECT_EQ(updater.initiateUpdateImage(
                  "/xyz/openbmc_project/software/Version123", "/tmp/image.bin",
                  "Version123", "Version123", "uuid-1"),
              0);
    EXPECT_EQ(updater.versionCount(), 1u);
    EXPECT_EQ(updater.activationMatchCount(), 1u);
    ASSERT_TRUE(updater.versions.contains("Version123"));
    EXPECT_EQ(updater.versions.at("Version123")->activation(),
              Version::Status::Ready);
}

TEST_F(BaseUpdaterControllerTest, InitiateUpdateImageReplacesExistingVersion)
{
    TestableItemUpdater updater(bus);
    updater.versions.emplace(
        "Version123",
        updater.createVersion("/xyz/openbmc_project/software/Version123",
                              "Version123", "Version123", "uuid-1",
                              "/tmp/image.bin", Version::Status::Ready));

    EXPECT_EQ(updater.initiateUpdateImage(
                  "/xyz/openbmc_project/software/Version123", "/tmp/image.bin",
                  "Version123", "Version123", "uuid-1"),
              0);
    EXPECT_EQ(updater.versionCount(), 1u);
}

TEST_F(BaseUpdaterControllerTest, InitiateUpdateImageRejectsActivatingVersion)
{
    TestableItemUpdater updater(bus);
    updater.versions.emplace(
        "Version123",
        updater.createVersion("/xyz/openbmc_project/software/Version123",
                              "Version123", "Version123", "uuid-1",
                              "/tmp/image.bin", Version::Status::Ready));
    updater.versions.at("Version123")
        ->VersionInherit::activation(Version::Status::Activating);

    EXPECT_EQ(updater.initiateUpdateImage(
                  "/xyz/openbmc_project/software/Version123", "/tmp/image.bin",
                  "Version123", "Version123", "uuid-1"),
              -1);
    EXPECT_EQ(updater.versionCount(), 1u);
}

TEST_F(BaseUpdaterControllerTest, OnInventoryChangedCreatesSoftwareObject)
{
    TestableItemUpdater updater(bus);
    Properties properties{{PRESENT, true},
                          {MODEL, std::string("Model-A")},
                          {MANUFACTURER, std::string("NVIDIA")}};

    updater.onInventoryChanged("/xyz/openbmc_project/inventory/gpu0",
                               properties);
    EXPECT_EQ(updater.versionCount(), 1u);
}

TEST_F(BaseUpdaterControllerTest, NewDeviceAddedIgnoresMalformedMessages)
{
    TestableItemUpdater updater(bus);
    auto message = makeSignalMessage(bus, "/xyz/openbmc_project/inventory",
                                     std::string("bad"));

    EXPECT_NO_THROW(updater.newDeviceAdded(message.msg));
    EXPECT_TRUE(updater.readDeviceCalls.empty());
}

TEST_F(BaseUpdaterControllerTest, OnInventoryChangedWaitsForMissingFields)
{
    TestableItemUpdater updater(bus);
    Properties properties{{PRESENT, true}};

    updater.onInventoryChanged("/xyz/openbmc_project/inventory/gpu0",
                               properties);
    EXPECT_EQ(updater.versionCount(), 0u);
}

TEST_F(BaseUpdaterControllerTest, OnInventoryChangedIgnoresUnrelatedProperties)
{
    TestableItemUpdater updater(bus);

    updater.onInventoryChanged("/xyz/openbmc_project/inventory/gpu0", {});
    EXPECT_EQ(updater.versionCount(), 0u);
}

TEST_F(BaseUpdaterControllerTest, OnInventoryChangedWaitsForManufacturer)
{
    TestableItemUpdater updater(bus);
    Properties properties{{PRESENT, true}, {MODEL, std::string("Model-A")}};

    updater.onInventoryChanged("/xyz/openbmc_project/inventory/gpu0",
                               properties);
    EXPECT_EQ(updater.versionCount(), 0u);
}

TEST_F(BaseUpdaterControllerTest, OnInventoryChangedReturnsWhenDeviceNotPresent)
{
    TestableItemUpdater updater(bus);
    Properties properties{{PRESENT, false}};

    updater.onInventoryChanged("/xyz/openbmc_project/inventory/gpu0",
                               properties);
    EXPECT_EQ(updater.versionCount(), 0u);
}

TEST_F(BaseUpdaterControllerTest, OnInventoryChangedLogsWhenVersionIsEmpty)
{
    TestableItemUpdater updater(bus);
    updater.versionForPath.clear();
    Properties properties{{PRESENT, true},
                          {MODEL, std::string("Model-A")},
                          {MANUFACTURER, std::string("NVIDIA")}};

    updater.onInventoryChanged("/xyz/openbmc_project/inventory/gpu0",
                               properties);
    EXPECT_EQ(updater.versionCount(), 0u);
}

TEST_F(BaseUpdaterControllerTest, OnReqActivationChangedTriggersVersionHandler)
{
    TestableItemUpdater updater(bus);
    updater.inventorySupportedValue = false;
    updater.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};

    updater.versions.emplace(
        "Version123",
        updater.createVersion("/xyz/openbmc_project/software/Version123",
                              "Version123", "Version123", "uuid-1", "",
                              Version::Status::Ready));

    Properties properties{{"RequestedActivation",
                           NSActivation::convertRequestedActivationsToString(
                               NSActivation::RequestedActivations::Active)}};
    updater.onReqActivationChanged("/xyz/openbmc_project/software/Version123",
                                   properties);
    EXPECT_TRUE(updater.versions.contains("Version123"));
}

TEST_F(BaseUpdaterControllerTest,
       OnReqActivationChangedIgnoresNonActiveRequests)
{
    TestableItemUpdater updater(bus);
    updater.inventorySupportedValue = false;
    updater.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};

    updater.versions.emplace(
        "Version123",
        updater.createVersion("/xyz/openbmc_project/software/Version123",
                              "Version123", "Version123", "uuid-1",
                              "/tmp/image.bin", Version::Status::Ready));

    updater.onReqActivationChanged(
        "/xyz/openbmc_project/software/Version123",
        Properties{{"RequestedActivation",
                    NSActivation::convertRequestedActivationsToString(
                        NSActivation::RequestedActivations::None)}});

    EXPECT_EQ(updater.versions.at("Version123")->activation(),
              Version::Status::Ready);
}

TEST_F(BaseUpdaterControllerTest,
       ApplyTargetFiltersHandlesEmptyAndSelectedTargets)
{
    TestableItemUpdater updater(bus);

    auto allTargets = updater.applyTargetFilters({});
    EXPECT_EQ(allTargets.type, TargetFilterType::UpdateAll);
    EXPECT_TRUE(allTargets.targets.empty());

    std::vector<sdbusplus::object_path> targets{
        sdbusplus::object_path("/xyz/openbmc_project/inventory/gpu0"),
        sdbusplus::object_path("/xyz/openbmc_project/inventory/gpu1")};
    auto selected = updater.applyTargetFilters(targets);
    EXPECT_EQ(selected.type, TargetFilterType::UpdateSelected);
    ASSERT_EQ(selected.targets.size(), 2u);
    EXPECT_EQ(selected.targets[0], "gpu0");
    EXPECT_EQ(selected.targets[1], "gpu1");
}

TEST_F(BaseUpdaterControllerTest,
       ApplyTargetFiltersReturnsUpdateNoneWhenTargetsDoNotValidate)
{
    TestableItemUpdater updater(bus);
    updater.forceEmptyValidatedTarget = true;

    std::vector<sdbusplus::object_path> targets{
        sdbusplus::object_path("/xyz/openbmc_project/inventory/gpu0")};
    auto filtered = updater.applyTargetFilters(targets);

    EXPECT_EQ(filtered.type, TargetFilterType::UpdateNone);
    EXPECT_TRUE(filtered.targets.empty());
}

TEST_F(BaseUpdaterControllerTest,
       CleanupImageUploadDirRemovesFilesAndUpdatesPath)
{
    TestableItemUpdater updater(bus);
    ScopedTempDir tempDir;
    auto dirPath = tempDir.path() / "image";
    writeFile(dirPath / "payload.bin", "data");

    auto version = updater.createVersion(
        "/xyz/openbmc_project/software/Version123", "Version123", "Version123",
        "uuid-1", dirPath.string(), Version::Status::Ready);

    updater.cleanupImageUploadDir(dirPath, version.get());
    EXPECT_FALSE(std::filesystem::exists(dirPath / "payload.bin"));
    EXPECT_EQ(version->path(), (dirPath / "na.img").string());
}

TEST_F(BaseUpdaterControllerTest,
       CleanupImageUploadDirLeavesNestedDirectoriesIntact)
{
    TestableItemUpdater updater(bus);
    ScopedTempDir tempDir;
    auto dirPath = tempDir.path() / "image";
    auto nestedDir = dirPath / "nested";
    writeFile(dirPath / "payload.bin", "data");
    writeFile(nestedDir / "nested.bin", "nested");

    auto version = updater.createVersion(
        "/xyz/openbmc_project/software/Version123", "Version123", "Version123",
        "uuid-1", dirPath.string(), Version::Status::Ready);

    updater.cleanupImageUploadDir(dirPath, version.get());
    // The directory tree is preserved so inotify watches (bound to directory
    // inodes) survive, but every staged image file is removed recursively,
    // including files nested inside component subdirectories.
    EXPECT_TRUE(std::filesystem::exists(nestedDir));
    EXPECT_FALSE(std::filesystem::exists(nestedDir / "nested.bin"));
    EXPECT_FALSE(std::filesystem::exists(dirPath / "payload.bin"));
}

TEST_F(BaseUpdaterControllerTest,
       GetUpdateServiceWithArgsAssertsWithoutAtMarker)
{
#ifdef NDEBUG
    GTEST_SKIP() << "assertions are disabled";
#else
    FilteringItemUpdater updater(bus, "svc.service");

    EXPECT_DEATH(static_cast<void>(updater.getUpdateServiceWithArgs(
                     "/xyz/openbmc_project/inventory/gpu0", "/tmp/image.bin",
                     "1.2.3", {TargetFilterType::UpdateAll, {}})),
                 "");
#endif
}

TEST_F(BaseUpdaterControllerTest, InvokeActivationRequestsUpdateOnVersion)
{
    TestableItemUpdater updater(bus);
    updater.inventorySupportedValue = false;
    updater.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};

    auto version = updater.createVersion(
        "/xyz/openbmc_project/software/Version123", "Version123", "Version123",
        "uuid-1", "/tmp/image.bin", Version::Status::Ready);

    updater.invokeActivation(version);
    EXPECT_EQ(version->activation(), Version::Status::Activating);
}

TEST_F(BaseUpdaterControllerTest, BaseControllerRemovesFileWhenUpdaterFails)
{
    ScopedTempDir tempDir;
    auto imagePath = tempDir.path() / "fw.bin";
    writeFile(imagePath, "payload");

    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<ControllerUpdater>(bus);
    auto* rawUpdater = static_cast<ControllerUpdater*>(updater.get());
    rawUpdater->processResult = -1;
    BaseController controller(updater);

    EXPECT_EQ(controller.processImage(imagePath.string()), 0);
    EXPECT_EQ(rawUpdater->processCalls, 1);
    EXPECT_FALSE(std::filesystem::exists(imagePath));
}

TEST_F(BaseUpdaterControllerTest, BaseControllerGetNameDelegatesToUpdater)
{
    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<ControllerUpdater>(bus);
    BaseController controller(updater);

    EXPECT_EQ(controller.getName(), "Updater");
}

TEST_F(BaseUpdaterControllerTest, BaseControllerDestructorRunsViaBasePointer)
{
    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<ControllerUpdater>(bus);
    auto* controller = new BaseController(updater);

    EXPECT_EQ(controller->getName(), "Updater");

    delete controller;
}

TEST_F(BaseUpdaterControllerTest, BaseControllerRejectsMissingImageFile)
{
    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<ControllerUpdater>(bus);
    BaseController controller(updater);

    EXPECT_EQ(controller.processImage("/tmp/does-not-exist.bin"), -1);
}

TEST_F(BaseUpdaterControllerTest, BaseControllerKeepsFileWhenUpdaterSucceeds)
{
    ScopedTempDir tempDir;
    auto imagePath = tempDir.path() / "fw.bin";
    writeFile(imagePath, "payload");

    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<ControllerUpdater>(bus);
    auto* rawUpdater = static_cast<ControllerUpdater*>(updater.get());
    BaseController controller(updater);

    EXPECT_EQ(controller.processImage(imagePath.string()), 0);
    EXPECT_EQ(rawUpdater->processCalls, 1);
    EXPECT_TRUE(std::filesystem::exists(imagePath));
}

TEST_F(BaseUpdaterControllerTest,
       BaseControllerProcessesExistingImagesThroughUpdater)
{
    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<ControllerUpdater>(bus);
    auto* rawUpdater = static_cast<ControllerUpdater*>(updater.get());
    BaseController controller(updater);

    EXPECT_EQ(controller.processExistingImages(), 0);
    EXPECT_EQ(rawUpdater->watchCalls, 1);
    EXPECT_EQ(rawUpdater->readExistingCalls, 1);
}

TEST_F(BaseUpdaterControllerTest,
       BaseControllerStartWatchingReturnsErrorOnException)
{
    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<ThrowingWatchUpdater>(bus);
    BaseController controller(updater);

    EXPECT_EQ(controller.startWatching(bus, nullptr), -1);
}

TEST_F(BaseUpdaterControllerTest,
       BaseControllerStartWatchingReturnsZeroOnSuccess)
{
    ScopedTempDir tempDir;
    auto watchPath = tempDir.path() / "watch";
    std::filesystem::create_directories(watchPath);

    std::unique_ptr<BaseItemUpdater> updater =
        std::make_unique<WatchingUpdater>(bus);
    auto* rawUpdater = static_cast<WatchingUpdater*>(updater.get());
    rawUpdater->pathsToMonitor = {watchPath};

    sd_event* loop = nullptr;
    ASSERT_GE(sd_event_default(&loop), 0);

    BaseController controller(updater);
    EXPECT_EQ(controller.startWatching(bus, loop), 0);

    sd_event_unref(loop);
}

TEST_F(BaseUpdaterControllerTest, DebugTokenHelpersFormatEscapedArguments)
{
    DebugTokenEraseItemUpdater eraseUpdater(bus);
    DebugTokenInstallItemUpdater installUpdater(bus);
    TargetFilter filter{TargetFilterType::UpdateAll, {}};

    EXPECT_EQ(eraseUpdater.getItemUpdaterInventoryPaths(),
              std::vector<std::string>{std::string(SOFTWARE_OBJPATH) + "/" +
                                       DEBUG_TOKEN_ERASE_NAME});
    EXPECT_EQ(installUpdater.getItemUpdaterInventoryPaths(),
              std::vector<std::string>{std::string(SOFTWARE_OBJPATH) + "/" +
                                       DEBUG_TOKEN_INSTALL_NAME});
    EXPECT_FALSE(eraseUpdater.inventorySupported());
    EXPECT_FALSE(installUpdater.inventorySupported());
    EXPECT_EQ(eraseUpdater.getServiceArgs("", "", "1/2", filter),
              "\\x200\\x201-2");
    EXPECT_EQ(
        installUpdater.getServiceArgs("", "/tmp/image.bin", "1/2", filter),
        "\\x201\\x201-2\\x20-tmp-image.bin\\x20");
}
