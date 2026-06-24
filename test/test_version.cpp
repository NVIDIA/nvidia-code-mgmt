/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#include <systemd/sd-bus.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#define private public
#define protected public
#include "../src/version.hpp"
#undef private
#undef protected

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using ::testing::_;
using ::testing::NiceMock;
using namespace nvidia::software::updater;
namespace softwareServer = sdbusplus::xyz::openbmc_project::Software::server;

namespace
{

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

class FakeItemUpdaterUtils : public ItemUpdaterUtils
{
  public:
    std::vector<std::string> inventoryPaths{};
    std::string serviceName{"xyz.openbmc_project.FakeUpdater"};
    std::string updaterName{"TestUpdater"};
    std::string dbusService{"xyz.openbmc_project.FakeInventory"};
    TargetFilter filter{TargetFilterType::UpdateAll, {}};
    mutable int cleanupCalls = 0;
    int readExistingFirmWareCalls = 0;
    mutable std::string lastCleanupPath;
    mutable std::string lastVerifyPath;
    std::string lastUpdateInventoryPath;
    std::string lastUpdateImagePath;
    std::string lastUpdateVersion;
    TargetFilter lastUpdateFilter{TargetFilterType::UpdateNone, {}};
    bool updateTogether = false;
    bool inventorySupportedValue = false;
    bool verifyNeeded = false;
    bool verifyResult = true;
    uint32_t timeout = 5;

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        return inventoryPaths;
    }

    std::string getServiceName() const override
    {
        return serviceName;
    }

    void cleanupImageUploadDir(const std::filesystem::path& path,
                               Version*) const override
    {
        ++cleanupCalls;
        lastCleanupPath = path.string();
    }

    bool updateAllTogether() const override
    {
        return updateTogether;
    }

    std::string getUpdateServiceWithArgs(
        const std::string& inventoryPath, const std::string& imagePath,
        const std::string& version,
        const TargetFilter& targetFilter) const override
    {
        auto* self = const_cast<FakeItemUpdaterUtils*>(this);
        self->lastUpdateInventoryPath = inventoryPath;
        self->lastUpdateImagePath = imagePath;
        self->lastUpdateVersion = version;
        self->lastUpdateFilter = targetFilter;
        return serviceName;
    }

    std::string getName() const override
    {
        return updaterName;
    }

    void readExistingFirmWare() override
    {
        ++readExistingFirmWareCalls;
    }

    std::string getDbusService(const std::string&, const std::string&) override
    {
        return dbusService;
    }

    TargetFilter
        applyTargetFilters(const std::vector<sdbusplus::object_path>&) override
    {
        return filter;
    }

    uint32_t getTimeout() override
    {
        return timeout;
    }

    bool inventorySupported() override
    {
        return inventorySupportedValue;
    }

    bool needVerify() const override
    {
        return verifyNeeded;
    }

    bool doVerify(const std::string& imagePath) const override
    {
        lastVerifyPath = imagePath;
        return verifyResult;
    }
};

class VersionTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    FakeItemUpdaterUtils itemUpdaterUtils;
    std::string erasedVersionId;

    std::unique_ptr<Version> makeVersion(
        const std::string& filePath = "/tmp/test-image.bin",
        const Version::Status& activationStatus = Version::Status::Ready,
        const std::string& uniqueId = "uuid-1")
    {
        return std::make_unique<Version>(
            bus, "1.2.3", "/xyz/openbmc_project/software/test", uniqueId,
            "TestUpdater_123", filePath, activationStatus, "Model-A", "NVIDIA",
            [this](std::string versionId) { erasedVersionId = versionId; },
            nullptr, &itemUpdaterUtils);
    }
};

} // namespace

// ========================== ActivationProgress ==========================

TEST_F(VersionTest, ActivationProgressConstruct)
{
    ActivationProgress progress(bus, "/xyz/openbmc_project/software/test");
    EXPECT_EQ(progress.progress(), 0);
}

TEST_F(VersionTest, ActivationProgressSetGet)
{
    ActivationProgress progress(bus, "/xyz/openbmc_project/software/test2");
    progress.progress(50);
    EXPECT_EQ(progress.progress(), 50);
}

// ========================== Inline helper classes ==========================

TEST_F(VersionTest, SoftwareVersionSetsPurpose)
{
    SoftwareVersion version(bus, "/xyz/openbmc_project/software/sw");
    EXPECT_EQ(version.purpose(),
              softwareServer::Version::VersionPurpose::Other);
}

TEST_F(VersionTest, HelperObjectsConstructWithoutThrowing)
{
    SoftwareSettings settings(bus, "/xyz/openbmc_project/software/settings");
    UpdatePolicy policy(bus, "/xyz/openbmc_project/software/policy");
    DeviceSKU sku(bus, "/xyz/openbmc_project/software/sku");

    policy.forceUpdate(true);
    EXPECT_TRUE(policy.forceUpdate());
    (void)settings;
    (void)sku;
}

TEST_F(VersionTest, DeleteInvokesEraseCallback)
{
    auto version = makeVersion();
    version->deleteObject->delete_();
    EXPECT_EQ(erasedVersionId, "TestUpdater_123");
}

TEST_F(VersionTest, DeleteWithoutEraseCallbackDoesNothing)
{
    Version version(bus, "1.2.3", "/xyz/openbmc_project/software/noerase",
                    "uuid-1", "TestUpdater_123", "/tmp/test-image.bin",
                    Version::Status::Ready, "Model-A", "NVIDIA", {}, nullptr,
                    &itemUpdaterUtils);

    EXPECT_NO_THROW(version.deleteObject->delete_());
    EXPECT_TRUE(erasedVersionId.empty());
}

// ========================== activation/requestedActivation
// ==========================

TEST_F(VersionTest, ActivationResetsProgressForNonActivatingStates)
{
    auto version = makeVersion();
    version->activationProgress =
        std::make_unique<ActivationProgress>(bus, version->getObjectPath());

    EXPECT_EQ(version->activation(Version::Status::Ready),
              Version::Status::Ready);
    EXPECT_EQ(version->activationProgress, nullptr);
}

TEST_F(VersionTest, ActivationWithEmptyPathKeepsPreviousStatus)
{
    auto version = makeVersion("", Version::Status::Ready);
    EXPECT_EQ(version->activation(Version::Status::Activating),
              Version::Status::Ready);
}

TEST_F(VersionTest, VersionAccessorsReturnConstructionValues)
{
    auto version = makeVersion("/tmp/test-image.bin", Version::Status::Ready,
                               "uuid-accessors");

    EXPECT_EQ(version->getManufacturer(), "NVIDIA");
    EXPECT_EQ(version->getModel(), "Model-A");
    EXPECT_EQ(version->getVersionString(), "1.2.3");
    EXPECT_EQ(version->getObjectPath(), "/xyz/openbmc_project/software/test");
    EXPECT_EQ(version->uuid(), "uuid-accessors");
}

TEST_F(VersionTest, ActivationFailsWhenVerificationFails)
{
    itemUpdaterUtils.verifyNeeded = true;
    itemUpdaterUtils.verifyResult = false;

    auto version = makeVersion("/tmp/to-verify.bin");
    EXPECT_EQ(version->activation(Version::Status::Activating),
              Version::Status::Failed);
    EXPECT_EQ(itemUpdaterUtils.lastVerifyPath, "/tmp/to-verify.bin");
}

TEST_F(VersionTest, ActivationFailsWhenNoInventoryPathExists)
{
    auto version = makeVersion("/tmp/test-image.bin");
    EXPECT_EQ(version->activation(Version::Status::Activating),
              Version::Status::Failed);
}

TEST_F(VersionTest, RequestedActivationFinishesImmediatelyForUpdateNone)
{
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};
    itemUpdaterUtils.filter = {TargetFilterType::UpdateNone, {}};

    auto version = makeVersion("/tmp/test-image.bin");
    EXPECT_EQ(version->requestedActivation(
                  softwareServer::Activation::RequestedActivations::Active),
              softwareServer::Activation::RequestedActivations::Active);
    EXPECT_EQ(version->activation(), Version::Status::Active);
    EXPECT_EQ(itemUpdaterUtils.cleanupCalls, 1);
}

TEST_F(VersionTest, RequestedActivationRetriesAfterTerminalFailure)
{
    // With no device inventory, startActivation() bails to Failed. A terminal
    // failure must not latch RequestedActivation at Active, otherwise the
    // guard in requestedActivation() would treat the next Active request as a
    // duplicate and silently drop the retry. The first attempt should clean up
    // once, and a second Active request should be allowed to run again.
    auto version = makeVersion("/tmp/test-image.bin");

    version->requestedActivation(
        softwareServer::Activation::RequestedActivations::Active);
    EXPECT_EQ(version->activation(), Version::Status::Failed);
    auto cleanupCalls = itemUpdaterUtils.cleanupCalls;

    version->requestedActivation(
        softwareServer::Activation::RequestedActivations::Active);
    EXPECT_GT(itemUpdaterUtils.cleanupCalls, cleanupCalls);
}

TEST_F(VersionTest, RequestedActivationFromActivatingStateDoesNotRestart)
{
    // A device must be present so the transition into Activating succeeds
    // instead of bailing on the empty-inventory cleanup path. The version is
    // driven into Activating AFTER construction, since startActivation()
    // dereferences updatePolicy, which is only built once the constructor
    // finishes.
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};
    itemUpdaterUtils.inventorySupportedValue = false;

    auto version = makeVersion("/tmp/test-image.bin");
    EXPECT_EQ(version->activation(Version::Status::Activating),
              Version::Status::Activating);

    EXPECT_EQ(version->requestedActivation(
                  softwareServer::Activation::RequestedActivations::Active),
              softwareServer::Activation::RequestedActivations::Active);
    EXPECT_EQ(itemUpdaterUtils.cleanupCalls, 0);
    EXPECT_EQ(itemUpdaterUtils.readExistingFirmWareCalls, 0);
}

TEST_F(VersionTest, RequestedActivationRestartsFromFailedState)
{
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};
    itemUpdaterUtils.inventorySupportedValue = false;

    auto version = makeVersion("/tmp/test-image.bin", Version::Status::Failed);
    EXPECT_EQ(version->requestedActivation(
                  softwareServer::Activation::RequestedActivations::Active),
              softwareServer::Activation::RequestedActivations::Active);
    EXPECT_EQ(version->activation(), Version::Status::Activating);
}

TEST_F(VersionTest, RequestedActivationRestartsFromActiveState)
{
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};
    itemUpdaterUtils.inventorySupportedValue = false;

    auto version = makeVersion("/tmp/test-image.bin", Version::Status::Active);
    EXPECT_EQ(version->requestedActivation(
                  softwareServer::Activation::RequestedActivations::Active),
              softwareServer::Activation::RequestedActivations::Active);
    EXPECT_EQ(version->activation(), Version::Status::Activating);
}

TEST_F(VersionTest, StartActivationHonorsUpdateAllTogether)
{
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0",
                                       "/xyz/openbmc_project/inventory/gpu1"};
    itemUpdaterUtils.updateTogether = true;
    itemUpdaterUtils.inventorySupportedValue = false;

    auto version = makeVersion("/tmp/test-image.bin");
    EXPECT_EQ(version->startActivation(), Version::Status::Activating);
    ASSERT_NE(version->activationProgress, nullptr);
    EXPECT_EQ(version->activationProgress->progress(), 10);
    EXPECT_EQ(version->deviceQueue.size(), 1u);
    EXPECT_EQ(version->progressStep, 80u);
}

TEST_F(VersionTest, ActivationProcessesMultipleDevicesAndCompletes)
{
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0",
                                       "/xyz/openbmc_project/inventory/gpu1"};

    auto version = makeVersion("/tmp/test-image.bin");
    EXPECT_EQ(version->activation(Version::Status::Activating),
              Version::Status::Activating);
    ASSERT_NE(version->activationProgress, nullptr);
    EXPECT_EQ(version->activationProgress->progress(), 10);
    EXPECT_EQ(version->deviceQueue.size(), 2u);

    version->onUpdateDone();
    ASSERT_NE(version->activationProgress, nullptr);
    EXPECT_EQ(version->activationProgress->progress(), 50);
    EXPECT_EQ(version->deviceQueue.size(), 1u);

    version->onUpdateDone();
    EXPECT_EQ(version->activation(), Version::Status::Active);
    EXPECT_EQ(version->activationProgress, nullptr);
    EXPECT_EQ(version->deviceQueue.size(), 0u);
    EXPECT_EQ(itemUpdaterUtils.cleanupCalls, 1);
}

TEST_F(VersionTest, OnUpdateFailedClearsQueueAndCleansUp)
{
    // Construct in Ready so the constructor does not run startActivation();
    // the queue and progress are set up by hand to isolate the single cleanup
    // performed by onUpdateFailed().
    auto version = makeVersion("/tmp/test-image.bin", Version::Status::Ready);
    version->activationProgress =
        std::make_unique<ActivationProgress>(bus, version->getObjectPath());
    version->deviceQueue.push("/xyz/openbmc_project/inventory/gpu0");

    version->onUpdateFailed();

    EXPECT_EQ(version->activation(), Version::Status::Failed);
    EXPECT_EQ(version->activationProgress, nullptr);
    EXPECT_EQ(version->deviceQueue.size(), 0u);
    EXPECT_EQ(itemUpdaterUtils.cleanupCalls, 1);
    EXPECT_EQ(itemUpdaterUtils.readExistingFirmWareCalls, 1);
}

TEST_F(VersionTest, DoUpdateWithEmptyQueueFinishesActivation)
{
    auto version = makeVersion("/tmp/test-image.bin", Version::Status::Ready);
    version->activationProgress =
        std::make_unique<ActivationProgress>(bus, version->getObjectPath());

    EXPECT_TRUE(version->doUpdate());
    EXPECT_EQ(version->activation(), Version::Status::Active);
    EXPECT_EQ(itemUpdaterUtils.cleanupCalls, 1);
}

TEST_F(VersionTest, ActivationVerifiesImageAndContinuesWhenVerificationSucceeds)
{
    itemUpdaterUtils.verifyNeeded = true;
    itemUpdaterUtils.verifyResult = true;
    itemUpdaterUtils.inventorySupportedValue = false;
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};

    auto version = makeVersion("/tmp/to-verify.bin");
    EXPECT_EQ(version->activation(Version::Status::Activating),
              Version::Status::Activating);
    EXPECT_EQ(itemUpdaterUtils.lastVerifyPath, "/tmp/to-verify.bin");
}

TEST_F(VersionTest, OnUpdateDoneWithoutProgressDoesNothing)
{
    auto version = makeVersion("/tmp/test-image.bin", Version::Status::Ready);

    version->onUpdateDone();
    EXPECT_EQ(version->activation(), Version::Status::Ready);
}

TEST_F(VersionTest, ActivationFailsWhenSystemdStartFails)
{
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};
    itemUpdaterUtils.inventorySupportedValue = false;

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, _, _, nullptr))
        .WillRepeatedly(::testing::Return(-EIO));

    auto version = makeVersion("/tmp/test-image.bin");
    EXPECT_EQ(version->activation(Version::Status::Activating),
              Version::Status::Failed);
    EXPECT_EQ(itemUpdaterUtils.cleanupCalls, 1);
    EXPECT_EQ(itemUpdaterUtils.readExistingFirmWareCalls, 1);
}

TEST_F(VersionTest, DoUpdateReturnsTrueWhenSystemdStartSucceeds)
{
    itemUpdaterUtils.inventorySupportedValue = false;

    auto version = makeVersion("/tmp/test-image.bin");
    EXPECT_TRUE(version->doUpdate("/xyz/openbmc_project/inventory/gpu0"));
    EXPECT_EQ(version->currentUpdatingDevice,
              "/xyz/openbmc_project/inventory/gpu0");
}

TEST_F(VersionTest, StartActivationReusesExistingActivationProgress)
{
    itemUpdaterUtils.inventoryPaths = {"/xyz/openbmc_project/inventory/gpu0"};
    itemUpdaterUtils.inventorySupportedValue = false;

    auto version = makeVersion("/tmp/test-image.bin");
    version->activationProgress =
        std::make_unique<ActivationProgress>(bus, version->getObjectPath());
    auto* progress = version->activationProgress.get();

    EXPECT_EQ(version->startActivation(), Version::Status::Activating);
    EXPECT_EQ(version->activationProgress.get(), progress);
    EXPECT_EQ(version->activationProgress->progress(), 10);
}

// ========================== compatibility / service helpers
// ==========================

TEST_F(VersionTest, IsCompatibleReturnsTrueWhenInventoryIsUnsupported)
{
    auto version = makeVersion();
    EXPECT_TRUE(version->isCompatible("/xyz/openbmc_project/inventory/gpu0"));
}

TEST_F(VersionTest, IsCompatibleFallsBackToTrueWhenPropertyReadFails)
{
    itemUpdaterUtils.inventorySupportedValue = true;

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(sdbusMock,
                sd_bus_message_enter_container(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(testing::Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(testing::Return(1));
    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    int stringReads = 0;
    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly(
            [&stringReads](sd_bus_message*, char type, void* output) {
                if (type == 's')
                {
                    static const char* manufacturer = "NVIDIA";
                    static const char* model = "OtherModel";
                    *static_cast<const char**>(output) =
                        (stringReads++ == 0) ? manufacturer : model;
                }
                return 0;
            });

    auto version = makeVersion();
    EXPECT_TRUE(version->isCompatible("/xyz/openbmc_project/inventory/gpu0"));
}

TEST_F(VersionTest, IsCompatibleFallsBackToTrueWhenManufacturerReadFails)
{
    itemUpdaterUtils.inventorySupportedValue = true;

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillRepeatedly([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    EXPECT_CALL(sdbusMock,
                sd_bus_message_enter_container(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(testing::Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(testing::Return(1));
    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(1));

    int stringReads = 0;
    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly(
            [&stringReads](sd_bus_message*, char type, void* output) {
                if (type == 's')
                {
                    static const char* manufacturer = "ACME";
                    static const char* model = "Model-A";
                    *static_cast<const char**>(output) =
                        (stringReads++ == 0) ? manufacturer : model;
                }
                return 0;
            });

    auto version = makeVersion();
    EXPECT_TRUE(version->isCompatible("/xyz/openbmc_project/inventory/gpu0"));
}

TEST_F(VersionTest, GetUpdateServiceDelegatesToItemUpdaterUtils)
{
    auto version = makeVersion("/tmp/test-image.bin");
    itemUpdaterUtils.filter = {TargetFilterType::UpdateSelected, {"gpu0"}};
    version->targetFilter = itemUpdaterUtils.filter;

    EXPECT_EQ(version->getUpdateService("/xyz/openbmc_project/inventory/gpu0"),
              itemUpdaterUtils.serviceName);
    EXPECT_EQ(itemUpdaterUtils.lastUpdateInventoryPath,
              "/xyz/openbmc_project/inventory/gpu0");
    EXPECT_EQ(itemUpdaterUtils.lastUpdateImagePath, "/tmp/test-image.bin");
    EXPECT_EQ(itemUpdaterUtils.lastUpdateVersion, "1.2.3");
    EXPECT_EQ(itemUpdaterUtils.lastUpdateFilter.type,
              TargetFilterType::UpdateSelected);
}

TEST_F(VersionTest, StoreImageReturnsEarlyWhenImageIsAlreadyPersistent)
{
    const auto persistentPath = (std::filesystem::path(IMG_DIR_PERSIST) /
                                 itemUpdaterUtils.getName() / "uuid-1")
                                    .string();
    auto version =
        makeVersion(persistentPath, Version::Status::Ready, "uuid-1");

    version->storeImage();
    EXPECT_EQ(version->path(), persistentPath);
}

TEST_F(VersionTest, StoreImageCopiesImageToPersistentLocation)
{
    const auto sourceDir =
        std::filesystem::temp_directory_path() / "version-src";
    std::filesystem::create_directories(sourceDir);
    const auto sourcePath = sourceDir / "test-image.bin";
    {
        std::ofstream output(sourcePath);
        output << "payload";
    }

    itemUpdaterUtils.updaterName = "VersionStoreTest";
    const auto destRoot = std::filesystem::path(IMG_DIR_PERSIST) /
                          itemUpdaterUtils.getName() / "uuid-1";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(IMG_DIR_PERSIST),
                                        ec);
    if (ec)
    {
        GTEST_SKIP() << "persistent image directory is not writable: "
                     << ec.message();
    }
    std::filesystem::remove_all(destRoot);

    auto version =
        makeVersion(sourcePath.string(), Version::Status::Ready, "uuid-1");
    version->storeImage();

    auto expectedPath = destRoot;
    expectedPath += sourcePath.filename();
    EXPECT_EQ(version->path(), expectedPath.string());
    EXPECT_TRUE(std::filesystem::exists(destRoot / sourcePath.filename()));
    EXPECT_FALSE(std::filesystem::exists(sourcePath));

    std::filesystem::remove_all(destRoot.parent_path());
    std::filesystem::remove_all(sourceDir);
}

TEST_F(VersionTest, StoreImageHandlesFilesystemCopyFailure)
{
    itemUpdaterUtils.updaterName = "VersionStoreError";
    auto version = makeVersion("/tmp/nonexistent-version-image.bin",
                               Version::Status::Ready, "uuid-error");

    EXPECT_NO_THROW(version->storeImage());
    EXPECT_EQ(version->path(), "/tmp/nonexistent-version-image.bin");

    const auto destRoot = std::filesystem::path(IMG_DIR_PERSIST) /
                          itemUpdaterUtils.getName() / "uuid-error";
    std::filesystem::remove_all(destRoot.parent_path());
}

// ========================== logging helpers ==========================

TEST_F(VersionTest, CreateLogDoesNotThrow)
{
    auto version = makeVersion();
    std::map<std::string, std::string> addData{
        {"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
        {"REDFISH_MESSAGE_ARGS", "1.2.3,TestUpdater"},
        {"namespace", "FWUpdate"}};
    Level level = Level::Critical;

    EXPECT_NO_THROW(
        version->createLog("Update.1.0.TransferFailed", addData, level));
}

TEST_F(VersionTest, CreateLogHandlesSendFailure)
{
    EXPECT_CALL(sdbusMock, sd_bus_send(nullptr, nullptr, _))
        .WillRepeatedly(::testing::Return(-EIO));

    auto version = makeVersion();
    std::map<std::string, std::string> addData{
        {"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
        {"REDFISH_MESSAGE_ARGS", "1.2.3,TestUpdater"},
        {"namespace", "FWUpdate"}};
    Level level = Level::Critical;

    EXPECT_NO_THROW(
        version->createLog("Update.1.0.TransferFailed", addData, level));
}

TEST_F(VersionTest, LogTransferFailedDoesNotThrow)
{
    auto version = makeVersion();
    EXPECT_NO_THROW(version->logTransferFailed("TestUpdater", "1.2.3"));
}

TEST_F(VersionTest, LogTransferFailedUsesDebugTokenEraseMessage)
{
    itemUpdaterUtils.updaterName = DEBUG_TOKEN_ERASE_NAME;

    auto version = makeVersion();
    EXPECT_NO_THROW(
        version->logTransferFailed(DEBUG_TOKEN_ERASE_NAME, "1.2.3"));
}
