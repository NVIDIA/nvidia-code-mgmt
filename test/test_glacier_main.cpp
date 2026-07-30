#include "dbusutils.hpp"
#include "message_registry.hpp"

#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"

extern "C" unsigned int fake_sleep(unsigned int)
{
    return 0;
}

#define GlacierRecoveryCommands FakeGlacierRecoveryCommands
#include "../recovery_tool/glacier_recovery_tool/glacier_recovery_commands.hpp"
#undef GlacierRecoveryCommands

using RecoveryResult =
    glacier_recovery_tool::glacier_recovery_commands::RecoveryResult;
using FakeGlacierRecoveryCommands = glacier_recovery_tool::
    glacier_recovery_commands::FakeGlacierRecoveryCommands;

std::pair<uint32_t, uint32_t> getI2CBusAndAddress(const std::string& objPath,
                                                  const std::string& interface);
int glacier_service_main(int argc, char** argv);

namespace fake_glacier_service
{

struct Behavior
{
    bool unlockResult = true;
    bool throwOnUnlock = false;
    bool throwOnInit = false;
    bool throwOnPerform = false;
    std::vector<RecoveryResult> initResults{RecoveryResult::Ok};
    RecoveryResult performResult = RecoveryResult::Ok;
};

inline std::deque<Behavior> queuedBehaviors{};
inline std::unordered_map<const FakeGlacierRecoveryCommands*, Behavior>
    behaviorsByInstance{};
inline std::unordered_map<const FakeGlacierRecoveryCommands*, size_t>
    initIndexByInstance{};

} // namespace fake_glacier_service

namespace glacier_recovery_tool::glacier_recovery_commands
{

FakeGlacierRecoveryCommands::FakeGlacierRecoveryCommands(int busAdd,
                                                         int slaveAdd,
                                                         bool verbose) :
    busAddress(busAdd), slaveAddress(slaveAdd), verbose(verbose)
{
    if (fake_glacier_service::queuedBehaviors.empty())
    {
        fake_glacier_service::behaviorsByInstance[this] =
            fake_glacier_service::Behavior{};
    }
    else
    {
        fake_glacier_service::behaviorsByInstance[this] =
            fake_glacier_service::queuedBehaviors.front();
        fake_glacier_service::queuedBehaviors.pop_front();
    }
    fake_glacier_service::initIndexByInstance[this] = 0;
}

bool FakeGlacierRecoveryCommands::unlockI2CDevice()
{
    auto& behavior = fake_glacier_service::behaviorsByInstance.at(this);
    if (behavior.throwOnUnlock)
    {
        throw std::runtime_error("unlock failed");
    }
    return behavior.unlockResult;
}

RecoveryResult FakeGlacierRecoveryCommands::performInitialization()
{
    auto& behavior = fake_glacier_service::behaviorsByInstance.at(this);
    if (behavior.throwOnInit)
    {
        throw std::runtime_error("init failed");
    }

    const auto index = fake_glacier_service::initIndexByInstance.at(this);
    if (index >= behavior.initResults.size())
    {
        return behavior.initResults.empty() ? RecoveryResult::Ok
                                            : behavior.initResults.back();
    }

    fake_glacier_service::initIndexByInstance[this] = index + 1;
    return behavior.initResults[index];
}

RecoveryResult
    FakeGlacierRecoveryCommands::performGlacierRecovery(const std::string&)
{
    auto& behavior = fake_glacier_service::behaviorsByInstance.at(this);
    if (behavior.throwOnPerform)
    {
        throw std::runtime_error("perform failed");
    }
    return behavior.performResult;
}

std::string
    FakeGlacierRecoveryCommands::recoveryResultToStr(RecoveryResult result)
{
    return "result-" + std::to_string(static_cast<int>(result));
}

} // namespace glacier_recovery_tool::glacier_recovery_commands

namespace
{

using nvidia::software::updater::fakeManagedObjects;
using nvidia::software::updater::InterfaceMap;
using nvidia::software::updater::PropertyMap;

constexpr auto glacierCrisisObjInterfaceName =
    "xyz.openbmc_project.Configuration.GlacierCrisisRecovery";
constexpr auto gpioErotObjInterfaceName =
    "xyz.openbmc_project.Configuration.GPIOERoTRecovery";
constexpr auto unrelatedObjInterfaceName =
    "xyz.openbmc_project.Configuration.SomeOtherRecovery";

InterfaceMap makeGlacierInterfaces(bool recoverable, bool hidden,
                                   uint64_t bus = 6, uint64_t address = 0x42)
{
    return {{glacierCrisisObjInterfaceName,
             PropertyMap{{"isRecoverable", recoverable},
                         {"HiddenByFPGA", hidden},
                         {"I2CBus", bus},
                         {"I2CAddress", address}}}};
}

InterfaceMap makeGpioInterfaces(bool hidden, uint64_t bus = 4,
                                uint64_t address = 0x2A)
{
    return {{gpioErotObjInterfaceName, PropertyMap{{"HiddenByFPGA", hidden},
                                                   {"I2CBus", bus},
                                                   {"I2CAddress", address}}}};
}

InterfaceMap makeUnrelatedInterfaces(uint64_t bus = 4, uint64_t address = 0x2A)
{
    return {{unrelatedObjInterfaceName, PropertyMap{{"HiddenByFPGA", false},
                                                    {"I2CBus", bus},
                                                    {"I2CAddress", address}}}};
}

void addManagedObject(const std::string& path, const InterfaceMap& interfaces)
{
    fakeManagedObjects[std::filesystem::path(path)] = interfaces;
}

void resetFakes()
{
    fakeManagedObjects.clear();
    resetFakeMessageRegistryCalls();
    fake_glacier_service::queuedBehaviors.clear();
    fake_glacier_service::behaviorsByInstance.clear();
    fake_glacier_service::initIndexByInstance.clear();
}

class GlacierMainTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        resetFakes();
    }

    void TearDown() override
    {
        resetFakes();
    }
};

TEST_F(GlacierMainTest, HelperFunctionsCoverDeviceSelectionAndDbusLookup)
{
    addManagedObject("/xyz/openbmc_project/inventory/dev0",
                     makeGlacierInterfaces(true, false, 9, 0x55));
    EXPECT_EQ(getI2CBusAndAddress("/xyz/openbmc_project/inventory/dev0",
                                  glacierCrisisObjInterfaceName),
              (std::pair<uint32_t, uint32_t>{9, 0x55}));
}

TEST_F(GlacierMainTest, MainHandlesMissingArgsAndNoDevices)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* badArgv[] = {arg0};
    char* argv[] = {arg0, image};

    EXPECT_EQ(glacier_service_main(1, badArgv), -1);
    EXPECT_TRUE(fakeMessageRegistryCalls.empty());

    EXPECT_EQ(glacier_service_main(2, argv), -1);
    ASSERT_EQ(fakeMessageRegistryCalls.size(), 1u);
    EXPECT_TRUE(fakeMessageRegistryCalls.front().resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls.front().errorCode, noDevicesFound);
    EXPECT_EQ(fakeMessageRegistryCalls.front().deviceName,
              "GlacierCrisisRecovery");
}

TEST_F(GlacierMainTest, MainHandlesUnlockFailureAndInitBranches)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject("/xyz/openbmc_project/inventory/dev0",
                     makeGlacierInterfaces(true, true));
    addManagedObject("/xyz/openbmc_project/inventory/dev1",
                     makeGpioInterfaces(false));
    addManagedObject("/xyz/openbmc_project/inventory/dev2",
                     makeGlacierInterfaces(false, false));

    fake_glacier_service::queuedBehaviors.push_back(
        {.unlockResult = false, .initResults = {RecoveryResult::Ok}});
    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::FirmwareNotInRecovery}});

    EXPECT_EQ(glacier_service_main(2, argv), 0);
    ASSERT_EQ(fakeMessageRegistryCalls.size(), 2u);
    EXPECT_EQ(fakeMessageRegistryCalls[0].errorCode, deviceNotResponding);
    EXPECT_EQ(fakeMessageRegistryCalls[0].deviceName, "dev0");
    EXPECT_EQ(fakeMessageRegistryCalls[1].messageId, firmwareNotInRecovery);
    EXPECT_EQ(fakeMessageRegistryCalls[1].deviceName, "dev1");
}

TEST_F(GlacierMainTest, MainHandlesInitPerformAndExceptionFailures)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject("/xyz/openbmc_project/inventory/dev0",
                     makeGlacierInterfaces(true, false));
    addManagedObject("/xyz/openbmc_project/inventory/dev1",
                     makeGlacierInterfaces(true, false));
    addManagedObject("/xyz/openbmc_project/inventory/dev2",
                     makeGlacierInterfaces(true, false));

    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::FailedToReadData}});
    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::Ok},
         .performResult = RecoveryResult::FailedToReadHeader});
    fake_glacier_service::queuedBehaviors.push_back(
        {.throwOnInit = true, .initResults = {RecoveryResult::Ok}});

    EXPECT_EQ(glacier_service_main(2, argv), -1);
    ASSERT_EQ(fakeMessageRegistryCalls.size(), 4u);
    EXPECT_TRUE(fakeMessageRegistryCalls[0].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[0].errorCode,
              static_cast<ErrorCode>(RecoveryResult::FailedToReadData));
    EXPECT_EQ(fakeMessageRegistryCalls[1].messageId, recoveryStarted);
    EXPECT_TRUE(fakeMessageRegistryCalls[2].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[2].errorCode,
              static_cast<ErrorCode>(RecoveryResult::FailedToReadHeader));
    EXPECT_TRUE(fakeMessageRegistryCalls[3].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[3].errorCode, deviceRecoveryFailed);
}

TEST_F(GlacierMainTest, MainHandlesPostRecoveryOutcomes)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject("/xyz/openbmc_project/inventory/dev0",
                     makeGlacierInterfaces(true, false));
    addManagedObject("/xyz/openbmc_project/inventory/dev1",
                     makeGlacierInterfaces(true, false));
    addManagedObject("/xyz/openbmc_project/inventory/dev2",
                     makeGlacierInterfaces(true, false));

    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::Ok,
                         RecoveryResult::FirmwareNotInRecovery}});
    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::Ok, RecoveryResult::Ok}});
    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::Ok, RecoveryResult::FailedToReadData,
                         RecoveryResult::FailedToReadData,
                         RecoveryResult::FailedToReadData}});

    EXPECT_EQ(glacier_service_main(2, argv), -1);

    ASSERT_EQ(fakeMessageRegistryCalls.size(), 6u);
    EXPECT_EQ(fakeMessageRegistryCalls[0].messageId, recoveryStarted);
    EXPECT_EQ(fakeMessageRegistryCalls[1].messageId, recoverySuccessful);
    EXPECT_EQ(fakeMessageRegistryCalls[2].messageId, recoveryStarted);
    EXPECT_TRUE(fakeMessageRegistryCalls[3].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[3].errorCode, deviceRecoveryFailed);
    EXPECT_EQ(fakeMessageRegistryCalls[4].messageId, recoveryStarted);
    EXPECT_TRUE(fakeMessageRegistryCalls[5].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[5].errorCode,
              static_cast<ErrorCode>(RecoveryResult::FailedToReadData));
}

TEST_F(GlacierMainTest, MainHandlesUnlockAndPerformExceptions)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject("/xyz/openbmc_project/inventory/dev0",
                     makeGlacierInterfaces(true, true));
    addManagedObject("/xyz/openbmc_project/inventory/dev1",
                     makeGlacierInterfaces(true, false));

    fake_glacier_service::queuedBehaviors.push_back(
        {.throwOnUnlock = true, .initResults = {RecoveryResult::Ok}});
    fake_glacier_service::queuedBehaviors.push_back(
        {.throwOnPerform = true, .initResults = {RecoveryResult::Ok}});

    EXPECT_EQ(glacier_service_main(2, argv), -1);

    ASSERT_EQ(fakeMessageRegistryCalls.size(), 3u);
    EXPECT_TRUE(fakeMessageRegistryCalls[0].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[0].errorCode, deviceRecoveryFailed);
    EXPECT_EQ(fakeMessageRegistryCalls[0].deviceName, "dev0");
    EXPECT_EQ(fakeMessageRegistryCalls[1].messageId, recoveryStarted);
    EXPECT_EQ(fakeMessageRegistryCalls[1].deviceName, "dev1");
    EXPECT_TRUE(fakeMessageRegistryCalls[2].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[2].errorCode, deviceRecoveryFailed);
    EXPECT_EQ(fakeMessageRegistryCalls[2].deviceName, "dev1");
}

TEST_F(GlacierMainTest, MainCoversRetryTransitionsAndHiddenPropertyCatch)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject(
        "/xyz/openbmc_project/inventory/dev0",
        {{glacierCrisisObjInterfaceName,
          PropertyMap{{"isRecoverable", true},
                      {"I2CBus", static_cast<uint64_t>(6)},
                      {"I2CAddress", static_cast<uint64_t>(0x31)}}}});
    addManagedObject("/xyz/openbmc_project/inventory/dev1",
                     makeGpioInterfaces(false, 7, 0x32));
    addManagedObject("/xyz/openbmc_project/inventory/dev2",
                     makeGlacierInterfaces(true, false, 8, 0x33));

    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::Ok}});
    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::Ok, RecoveryResult::FailedToReadData,
                         RecoveryResult::FirmwareNotInRecovery}});
    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::Ok, RecoveryResult::FailedToReadData,
                         RecoveryResult::Ok}});

    EXPECT_EQ(glacier_service_main(2, argv), -1);

    ASSERT_EQ(fakeMessageRegistryCalls.size(), 5u);
    EXPECT_TRUE(fakeMessageRegistryCalls[0].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[0].errorCode, deviceRecoveryFailed);
    EXPECT_EQ(fakeMessageRegistryCalls[0].deviceName, "dev0");

    EXPECT_EQ(fakeMessageRegistryCalls[1].messageId, recoveryStarted);
    EXPECT_EQ(fakeMessageRegistryCalls[1].deviceName, "dev1");
    EXPECT_EQ(fakeMessageRegistryCalls[2].messageId, recoverySuccessful);
    EXPECT_EQ(fakeMessageRegistryCalls[2].deviceName, "dev1");

    EXPECT_EQ(fakeMessageRegistryCalls[3].messageId, recoveryStarted);
    EXPECT_EQ(fakeMessageRegistryCalls[3].deviceName, "dev2");
    EXPECT_TRUE(fakeMessageRegistryCalls[4].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[4].errorCode, deviceRecoveryFailed);
    EXPECT_EQ(fakeMessageRegistryCalls[4].deviceName, "dev2");
}

TEST_F(GlacierMainTest, MainSkipsNonGlacierDeviceAndRecoversHiddenDevice)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject("/xyz/openbmc_project/inventory/dev0",
                     makeUnrelatedInterfaces(4, 0x20));
    addManagedObject("/xyz/openbmc_project/inventory/dev1",
                     makeGpioInterfaces(true, 5, 0x21));

    fake_glacier_service::queuedBehaviors.push_back(
        {.unlockResult = true,
         .initResults = {RecoveryResult::Ok,
                         RecoveryResult::FirmwareNotInRecovery}});

    EXPECT_EQ(glacier_service_main(2, argv), 0);

    ASSERT_EQ(fakeMessageRegistryCalls.size(), 2u);
    EXPECT_EQ(fakeMessageRegistryCalls[0].messageId, recoveryStarted);
    EXPECT_EQ(fakeMessageRegistryCalls[0].deviceName, "dev1");
    EXPECT_EQ(fakeMessageRegistryCalls[1].messageId, recoverySuccessful);
    EXPECT_EQ(fakeMessageRegistryCalls[1].deviceName, "dev1");
}

TEST_F(GlacierMainTest, MainThrowsWhenRecoverablePropertyIsMissing)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject(
        "/xyz/openbmc_project/inventory/dev0",
        {{glacierCrisisObjInterfaceName,
          PropertyMap{{"HiddenByFPGA", false},
                      {"I2CBus", static_cast<uint64_t>(11)},
                      {"I2CAddress", static_cast<uint64_t>(0x41)}}}});

    fake_glacier_service::queuedBehaviors.push_back(
        {.initResults = {RecoveryResult::FirmwareNotInRecovery}});

    EXPECT_THROW(glacier_service_main(2, argv), std::out_of_range);
    EXPECT_TRUE(fakeMessageRegistryCalls.empty());
}

TEST_F(GlacierMainTest, MainCatchesHiddenPropertyTypeMismatch)
{
    char arg0[] = "glacier";
    char image[] = "image.bin";
    char* argv[] = {arg0, image};

    addManagedObject(
        "/xyz/openbmc_project/inventory/dev0",
        {{glacierCrisisObjInterfaceName,
          PropertyMap{{"isRecoverable", true},
                      {"HiddenByFPGA", static_cast<uint64_t>(1)},
                      {"I2CBus", static_cast<uint64_t>(12)},
                      {"I2CAddress", static_cast<uint64_t>(0x42)}}}});

    EXPECT_EQ(glacier_service_main(2, argv), -1);

    ASSERT_EQ(fakeMessageRegistryCalls.size(), 1u);
    EXPECT_TRUE(fakeMessageRegistryCalls[0].resourceError);
    EXPECT_EQ(fakeMessageRegistryCalls[0].errorCode, deviceRecoveryFailed);
    EXPECT_EQ(fakeMessageRegistryCalls[0].deviceName, "dev0");
}

} // namespace
