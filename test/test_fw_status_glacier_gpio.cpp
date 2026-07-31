#include "dbusutils.hpp"
#include "glacier_recovery_commands.hpp"
#include "gpiod.hpp"
#include "mctp_vdm_helper.hpp"

#include <unistd.h>

#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

using ::testing::NiceMock;

#define private public
#define protected public
#include "../fw-status/mcu_resource.hpp"

#include "../fw-status/ap_resource.cpp"
#include "../fw-status/erot_resource.cpp"
#include "../fw-status/gpio_resource.cpp"
#include "../fw-status/mctp_discovery_resource.cpp"
#undef protected
#undef private

namespace
{

mctp_vdm::requester::Coroutine completedBootStatusCoroutine(uint8_t rc)
{
    co_return rc;
}

mctp_vdm::requester::Coroutine suspendedBootStatusCoroutine(uint8_t rc)
{
    co_await std::suspend_always{};
    co_return rc;
}

class FWStatusGlacierGpioTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        test::fw_status_fake_dbus::reset();
        test::fw_status_fake_glacier::reset();
        test::fw_status_fake_gpio::reset();
        test::fw_status_fake_vdm::reset();
        test::fw_status_fake_dbus::hostPowerStatus =
            "xyz.openbmc_project.State.Chassis.PowerState.On";

        ASSERT_EQ(pipe(erotPipe), 0);
        ASSERT_EQ(pipe(apPipe), 0);
        test::fw_status_fake_gpio::lines["EROT_GPIO"] = {};
        test::fw_status_fake_gpio::lines["EROT_GPIO"].eventFd = erotPipe[0];
        test::fw_status_fake_gpio::lines["AP_GPIO"] = {};
        test::fw_status_fake_gpio::lines["AP_GPIO"].eventFd = apPipe[0];
    }

    void TearDown() override
    {
        close(erotPipe[0]);
        close(erotPipe[1]);
        close(apPipe[0]);
        close(apPipe[1]);
    }

    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    int erotPipe[2] = {-1, -1};
    int apPipe[2] = {-1, -1};
};

TEST_F(FWStatusGlacierGpioTest, GPIOResourceCoversERoTBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));

    GPIOResource erot(bus, "/xyz/openbmc_project/software/erot-gpio", event, 1,
                      0x50, 30, "EROT_GPIO", "erot.target", "Interrupt",
                      std::nullopt, "ActiveHigh", "", helper);
    static_cast<BaseResource&>(erot).updateHealth();
    EXPECT_EQ(erot.state(), OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_TRUE(erot.isFirmwareInRecovery);
    EXPECT_FALSE(erot.hasAP());

    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    erot.updateERoTHealth();
    EXPECT_EQ(test::fw_status_fake_dbus::restartedUnits,
              std::vector<std::string>{"erot.target"});
    EXPECT_FALSE(erot.isFirmwareInRecovery);
    EXPECT_EQ(erot.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(erot.state(), OperationalStatusServer::StateType::Enabled);

    erot.hideWhenHealthy = true;
    erot.updateHealthyState();
    EXPECT_EQ(erot.resourceDbusObj, nullptr);
    erot.hideWhenHealthy = false;

    erot.setConnectedToChassis(true);
    erot.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(erot).updateHealth();
    EXPECT_EQ(erot.health(), HealthServer::HealthType::Warning);
    EXPECT_EQ(erot.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST_F(FWStatusGlacierGpioTest, ERoTAndAPResourcesCoverRecoveryAndBootStatus)
{
    auto event = sdeventplus::Event::get_default();
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));

    auto helper = std::make_shared<MCTPVdmHelper>();
    ERoTResource nonRecoverable(
        bus, "/xyz/openbmc_project/software/erot0", event, 40,
        "/xyz/openbmc_project/state/chassis/erot0", false, helper);
    static_cast<BaseResource&>(nonRecoverable).updateHealth();
    EXPECT_EQ(nonRecoverable.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(nonRecoverable.state(),
              OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(nonRecoverable.getBootStatus(), (std::vector<uint8_t>{0}));
    EXPECT_TRUE(
        nvidia::fw_status::boot_status::getBit({0x00, 0x00, 0x00, 0x08}, 3));
    EXPECT_FALSE(
        nvidia::fw_status::boot_status::getBit({0x00, 0x00, 0x00, 0x00}, 3));
    EXPECT_TRUE(nonRecoverable.isApBootFinished({0x00, 0x00, 0x00, 0x20}));
    EXPECT_FALSE(nonRecoverable.isApBootFinished({0x00, 0x00, 0x00, 0x00}));
    nonRecoverable.apBootStatusTimer->start(std::chrono::microseconds(1),
                                            false);
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);

    test::fw_status_fake_dbus::reset();
    test::fw_status_fake_glacier::reset();
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));
    ERoTResource recovery(
        bus, "/xyz/openbmc_project/software/erot1", event, 1, 0x50, 50, 51,
        "/xyz/openbmc_project/state/chassis/erot1",
        "/xyz/openbmc_project/software/ap-erot1", true, helper);
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_EQ(recovery.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_EQ(recovery.state(), OperationalStatusServer::StateType::Degraded);

    recovery.setConnectedToChassis(true);
    recovery.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_EQ(recovery.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    recovery.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    recovery.apBootStatusTimer->start(std::chrono::microseconds(1), false);
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);

    recovery.mctpObjectPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/50";
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_EQ(recovery.state(), OperationalStatusServer::StateType::Enabled);
}

TEST_F(FWStatusGlacierGpioTest, APResourceCoversUpdateAndInitializeBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    ERoTResource recovery(
        bus, "/xyz/openbmc_project/software/erot-ap", event, 1, 0x50, 60, 61,
        "/xyz/openbmc_project/state/chassis/erot-ap",
        "/xyz/openbmc_project/software/ap-test", true, helper);
    ASSERT_NE(recovery.apResource, nullptr);
    auto& ap = *recovery.apResource;

    auto runCoroutine = [](auto&& coroutine) {
        if (coroutine.handle)
        {
            coroutine.detach();
        }
    };

    runCoroutine(ap.updateHealthAsync(true));
    EXPECT_EQ(ap.health(), HealthServer::HealthType::Warning);
    EXPECT_EQ(ap.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    recovery.mctpObjectPath.clear();
    ap.mctpObjectPath = "/au/com/codeconstruct/mctp1/networks/1/endpoints/61";
    runCoroutine(ap.updateHealthAsync(false));
    EXPECT_EQ(ap.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(ap.state(), OperationalStatusServer::StateType::Enabled);

    ap.mctpObjectPath.clear();
    runCoroutine(ap.updateHealthAsync(false));
    EXPECT_EQ(ap.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(ap.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    recovery.mctpObjectPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/60";
    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x08, 0x00, 0x00,
                                                   0x00};
    runCoroutine(ap.updateHealthAsync(false));
    EXPECT_EQ(ap.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(ap.state(), OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x00, 0x00, 0x00,
                                                   0x00};
    runCoroutine(ap.updateHealthAsync(false));
    EXPECT_EQ(ap.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(ap.state(), OperationalStatusServer::StateType::Degraded);

    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x08, 0x00, 0x00,
                                                   0x00};
    runCoroutine(ap.initializeHealth());
    EXPECT_NE(ap.timer, nullptr);
    ap.timer->start(std::chrono::microseconds(1), false);
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);
    recovery.apBootStatusTimer->start(std::chrono::microseconds(1), false);
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);

    test::fw_status_fake_vdm::suspendQuery = true;
    ap.updateHealth();
    auto pendingApHealthHandle = ap.co;
    ASSERT_TRUE(pendingApHealthHandle);
    EXPECT_FALSE(pendingApHealthHandle.done());
    ap.updateHealth();
    EXPECT_TRUE(pendingApHealthHandle.promise().detached);
    EXPECT_EQ(ap.co, nullptr);
    pendingApHealthHandle.destroy();
    test::fw_status_fake_vdm::suspendQuery = false;
    EXPECT_FALSE(ap.isApHealthy());
    ap.mctpObjectPath = "/au/com/codeconstruct/mctp1/networks/1/endpoints/61";
    EXPECT_TRUE(ap.isApHealthy());
}

TEST_F(FWStatusGlacierGpioTest,
       ERoTAndAPHelpersCoverCoroutineAndUnhealthyERoTBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    ERoTResource nonRecoverable(
        bus, "/xyz/openbmc_project/software/erot-coroutines", event, 70,
        "/xyz/openbmc_project/state/chassis/erot-coroutines", false, helper);

    ASSERT_EQ(nonRecoverable.apResource, nullptr);
    nonRecoverable.updateHealth();

    auto suspended = suspendedBootStatusCoroutine(1);
    auto suspendedHandle = suspended.handle;
    suspended.handle = nullptr;
    ASSERT_TRUE(suspendedHandle);
    EXPECT_FALSE(suspendedHandle.done());

    nonRecoverable.co = suspendedHandle;
    nonRecoverable.updateBootStatus();
    EXPECT_TRUE(suspendedHandle.promise().detached);
    EXPECT_EQ(nonRecoverable.co, nullptr);
    suspendedHandle.destroy();

    auto completed = completedBootStatusCoroutine(2);
    auto completedHandle = completed.handle;
    completed.handle = nullptr;
    ASSERT_TRUE(completedHandle);
    EXPECT_TRUE(completedHandle.done());

    nonRecoverable.co = completedHandle;
    nonRecoverable.updateBootStatus();
    EXPECT_EQ(nonRecoverable.co, nullptr);

    nonRecoverable.bootStatus.reset();
    nonRecoverable.updateERoTHealth();

    test::fw_status_fake_glacier::reset();
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    ERoTResource recovery(
        bus, "/xyz/openbmc_project/software/erot-ap-check", event, 1, 0x50, 80,
        81, "/xyz/openbmc_project/state/chassis/erot-ap-check",
        "/xyz/openbmc_project/software/ap-check", true, helper);
    ASSERT_NE(recovery.apResource, nullptr);
    auto& ap = *recovery.apResource;

    recovery.mctpObjectPath.clear();
    ap.mctpObjectPath = "/au/com/codeconstruct/mctp1/networks/1/endpoints/81";
    EXPECT_FALSE(ap.isERoTHealthy());
    EXPECT_FALSE(ap.isAPInRecovery());

    ap.mctpObjectPath.clear();
    EXPECT_TRUE(ap.isAPInRecovery());
}

TEST_F(FWStatusGlacierGpioTest,
       ERoTUpdateBootStatusCoversPendingCoroutineBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    ERoTResource recovery(
        bus, "/xyz/openbmc_project/software/erot-pending", event, 1, 0x50, 92,
        93, "/xyz/openbmc_project/state/chassis/erot-pending",
        "/xyz/openbmc_project/software/ap-pending-erot", true, helper);
    ASSERT_NE(recovery.apResource, nullptr);

    recovery.updateHealth();

    recovery.mctpObjectPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/92";
    test::fw_status_fake_vdm::suspendQuery = true;
    recovery.updateBootStatus();
    auto pendingErotHandle = recovery.co;
    recovery.co = nullptr;
    ASSERT_TRUE(pendingErotHandle);
    EXPECT_FALSE(pendingErotHandle.done());
    pendingErotHandle.destroy();
    test::fw_status_fake_vdm::suspendQuery = false;
}

TEST_F(FWStatusGlacierGpioTest,
       GPIOResourceCoversRegistrationAndPolarityFailures)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();
    const int erotEventFd = dup(erotPipe[0]);
    const int erotOffFd = dup(erotPipe[0]);
    ASSERT_GE(erotEventFd, 0);
    ASSERT_GE(erotOffFd, 0);

    test::fw_status_fake_gpio::lines["EROT_EVENT_GPIO"] = {};
    test::fw_status_fake_gpio::lines["EROT_EVENT_GPIO"].eventFd = erotEventFd;
    test::fw_status_fake_gpio::lines["EROT_EVENT_GPIO"].eventType =
        gpiod::line_event::RISING_EDGE;
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    GPIOResource erotEvent(bus, "/xyz/openbmc_project/software/erot-event",
                           event, 2, 0x51, 32, "EROT_EVENT_GPIO",
                           "erot-event.target", "Interrupt", std::nullopt,
                           "ActiveHigh", "", helper);
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    erotEvent.waitForGPIOEvent();

    test::fw_status_fake_gpio::lines["EROT_BADFD_GPIO"] = {};
    test::fw_status_fake_gpio::lines["EROT_BADFD_GPIO"].eventFd = -1;
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    GPIOResource erotBadFd(bus, "/xyz/openbmc_project/software/erot-badfd",
                           event, 3, 0x52, 33, "EROT_BADFD_GPIO",
                           "erot-badfd.target", "Interrupt", std::nullopt,
                           "ActiveHigh", "", helper);
    EXPECT_EQ(erotBadFd.gpioEvent, nullptr);

    test::fw_status_fake_gpio::lines["EROT_OFF_GPIO"] = {};
    test::fw_status_fake_gpio::lines["EROT_OFF_GPIO"].eventFd = erotOffFd;
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    GPIOResource erotOff(bus, "/xyz/openbmc_project/software/erot-off", event,
                         4, 0x53, 34, "EROT_OFF_GPIO", "erot-off.target",
                         "Interrupt", std::nullopt, "ActiveHigh", "", helper);
    erotOff.setConnectedToChassis(true);
    erotOff.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(erotOff).updateHealth();
    EXPECT_EQ(erotOff.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST_F(FWStatusGlacierGpioTest,
       MCUResourceCoversEnumeratedRecoveryHealthyAbsentAndPowerOffStates)
{
    auto manager = std::make_shared<mcu_recovery_manager::MCURecoveryManager>();

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/66",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(66));
    test::fw_status_fake_mcu::devices["mcu-enumerated"] = {.healthy = false,
                                                           .inRecovery = true};
    MCUResource enumerated(bus, "/xyz/openbmc_project/software/mcu-enumerated",
                           66, "mcu-enumerated", manager);
    static_cast<BaseResource&>(enumerated).updateHealth();
    EXPECT_TRUE(test::fw_status_fake_mcu::updateAllCalled);
    EXPECT_EQ(enumerated.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(enumerated.state(), OperationalStatusServer::StateType::Enabled);

    test::fw_status_fake_dbus::reset();
    test::fw_status_fake_mcu::updateAllCalled = false;
    test::fw_status_fake_mcu::devices["mcu-recovery"] = {.healthy = false,
                                                         .inRecovery = true};
    MCUResource recovery(bus, "/xyz/openbmc_project/software/mcu-recovery", 67,
                         "mcu-recovery", manager);
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_TRUE(test::fw_status_fake_mcu::updateAllCalled);
    EXPECT_EQ(recovery.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(recovery.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_mcu::devices["mcu-healthy"] = {.healthy = true,
                                                        .inRecovery = false};
    MCUResource healthy(bus, "/xyz/openbmc_project/software/mcu-healthy", 68,
                        "mcu-healthy", manager);
    static_cast<BaseResource&>(healthy).updateHealth();
    EXPECT_EQ(healthy.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(healthy.state(), OperationalStatusServer::StateType::Degraded);

    test::fw_status_fake_mcu::devices["mcu-offline"] = {.healthy = false,
                                                        .inRecovery = false};
    MCUResource offline(bus, "/xyz/openbmc_project/software/mcu-offline", 69,
                        "mcu-offline", manager);
    static_cast<BaseResource&>(offline).updateHealth();
    EXPECT_EQ(offline.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(offline.state(), OperationalStatusServer::StateType::Absent);

    offline.wasEnumeratedOnce = true;
    static_cast<BaseResource&>(offline).updateHealth();
    EXPECT_EQ(offline.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(offline.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    offline.setConnectedToChassis(true);
    offline.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(offline).updateHealth();
    EXPECT_EQ(offline.health(), HealthServer::HealthType::Warning);
    EXPECT_EQ(offline.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST(BootStatusUtils, BootStatusUtilsCoversAllHelpers)
{
    using namespace nvidia::fw_status::boot_status;

    // getAPFatalErrorCode: too short → nullopt; zero code; non-zero code
    EXPECT_EQ(getAPFatalErrorCode({0x00, 0x00, 0x00}), std::nullopt);
    EXPECT_EQ(getAPFatalErrorCode({0x00, 0x00, 0x00, 0x00}),
              std::optional<uint8_t>(0));
    // bits 28-31 = upper nibble of byte[0]: 0x10 >> 4 = 1
    EXPECT_EQ(getAPFatalErrorCode({0x10, 0x00, 0x00, 0x00}),
              std::optional<uint8_t>(1));

    // isAPFatalErrorCodeNormal: zero code → true; too short → false; non-zero →
    // false
    EXPECT_TRUE(isAPFatalErrorCodeNormal({0x00, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(isAPFatalErrorCodeNormal({0x01, 0x02}));
    EXPECT_FALSE(isAPFatalErrorCodeNormal({0x10, 0x00, 0x00, 0x00}));

    // isAPFatalErrorCodeSet: non-zero → true; zero → false; too short → false
    EXPECT_TRUE(isAPFatalErrorCodeSet({0x10, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(isAPFatalErrorCodeSet({0x00, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(isAPFatalErrorCodeSet({0x01, 0x02, 0x03}));

    // isAPBootComplete: bit 5 of last byte (0x20 = bit 5 set)
    EXPECT_TRUE(isAPBootComplete({0x00, 0x00, 0x00, 0x20}));
    EXPECT_FALSE(isAPBootComplete({0x00, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(isAPBootComplete({}));

    // isAPBootCompleteTimeout: bit 27 = byte[0] bit 3 in a 4-byte vector
    EXPECT_TRUE(isAPBootCompleteTimeout({0x08, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(isAPBootCompleteTimeout({0x00, 0x00, 0x00, 0x00}));
}

TEST_F(FWStatusGlacierGpioTest, GPIOResourceCoversAPBootStatusPaths)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    GPIOResource erot(bus, "/xyz/openbmc_project/software/erot-ap-gpio", event,
                      5, 0x54, 35, "EROT_GPIO", "erot-ap.target", "Interrupt",
                      std::nullopt, "ActiveHigh", "gpio-ap", helper);
    EXPECT_TRUE(erot.hasAP());
    ASSERT_NE(erot.apResource, nullptr);

    // FatalErrorAssert with ERoT not in recovery → startAPBootStatusCheck
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    erot.updateERoTHealth(GPIOResource::HealthUpdateReason::FatalErrorAssert);
    EXPECT_TRUE(erot.apBootStatusCheckActive);
    ASSERT_NE(erot.apBootStatusRetryTimer, nullptr);

    // Case 1: fatal error code set → AP unhealthy (Critical/StandbyOffline)
    // bootStatusPayload[0] is stripped; status = payload[1..end]
    // {0x00, 0x10, 0x00, 0x00, 0x00} → status = {0x10, 0x00, 0x00, 0x00},
    // code=1
    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x10, 0x00, 0x00,
                                                   0x00};
    erot.runAPBootStatusQuery();
    EXPECT_FALSE(erot.apBootStatusCheckActive);
    EXPECT_EQ(erot.apResource->health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(erot.apResource->state(),
              OperationalStatusServer::StateType::StandbyOffline);

    // Case 2: boot-complete timeout → AP unhealthy
    // status = {0x08, 0x00, 0x00, 0x00}: code=0, bit27=1 (timeout)
    erot.startAPBootStatusCheck();
    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x08, 0x00, 0x00,
                                                   0x00};
    erot.runAPBootStatusQuery();
    EXPECT_FALSE(erot.apBootStatusCheckActive);
    EXPECT_EQ(erot.apResource->state(),
              OperationalStatusServer::StateType::StandbyOffline);

    // Case 3: boot complete, no fatal error → stop check
    // status = {0x00, 0x00, 0x00, 0x20}: code=0, no timeout, bit5=1 (complete)
    erot.startAPBootStatusCheck();
    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x00, 0x00, 0x00,
                                                   0x20};
    erot.runAPBootStatusQuery();
    EXPECT_FALSE(erot.apBootStatusCheckActive);

    // Case 4: no response → retry
    erot.startAPBootStatusCheck();
    test::fw_status_fake_vdm::bootStatusPayload.clear();
    erot.runAPBootStatusQuery();
    EXPECT_TRUE(erot.apBootStatusCheckActive);
    EXPECT_EQ(erot.apBootStatusQueryRetryCount, 1u);

    // Case 5: status too short for fatal error code → retry
    // {0x00, 0x00} → status = {0x00} (1 byte), getAPFatalErrorCode → nullopt
    erot.startAPBootStatusCheck();
    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x00};
    erot.runAPBootStatusQuery();
    EXPECT_GT(erot.apBootStatusQueryRetryCount, 0u);

    // Case 6: still booting (code=0, no timeout, not complete) → retry
    // status = {0x00, 0x00, 0x00, 0x00}
    erot.startAPBootStatusCheck();
    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x00, 0x00, 0x00,
                                                   0x00};
    erot.runAPBootStatusQuery();
    EXPECT_GT(erot.apBootStatusQueryRetryCount, 0u);

    // Case 7: max retries exhausted → Degraded
    erot.apBootStatusQueryRetryCount = 10; // maxAPBootStatusQueryRetries
    erot.handleAPBootStatusUnavailable();
    EXPECT_FALSE(erot.apBootStatusCheckActive);
    EXPECT_EQ(erot.apResource->health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(erot.apResource->state(),
              OperationalStatusServer::StateType::Degraded);

    // Case 8: FatalErrorDeassert → stopAPBootStatusCheck + deleteAPObject
    erot.startAPBootStatusCheck();
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    erot.updateERoTHealth(GPIOResource::HealthUpdateReason::FatalErrorDeassert);
    EXPECT_FALSE(erot.apBootStatusCheckActive);
}

TEST_F(FWStatusGlacierGpioTest,
       GPIOResourceCoversPollingModeAndERoTRecoveryMonitor)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    test::fw_status_fake_gpio::lines["POLL_GPIO"] = {};
    test::fw_status_fake_gpio::lines["POLL_GPIO"].getValue = 0; // deasserted

    GPIOResource erot(bus, "/xyz/openbmc_project/software/erot-polling", event,
                      6, 0x55, 36, "POLL_GPIO",
                      "", // empty target: triggerMctpDiscovery is a no-op
                      "Polling", std::optional<uint64_t>(100), "ActiveHigh", "",
                      helper);

    // ERoT in recovery → startERoTRecoveryMonitor (only in Polling mode)
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));
    erot.updateERoTHealth(GPIOResource::HealthUpdateReason::FatalErrorAssert);
    EXPECT_TRUE(erot.isFirmwareInRecovery);
    EXPECT_TRUE(erot.erotRecoveryMonitorActive);
    ASSERT_NE(erot.erotRecoveryMonitorTimer, nullptr);

    // isMctpEndpointPresent: no endpoint in fake_dbus → false
    EXPECT_FALSE(erot.isMctpEndpointPresent());

    // runERoTRecoveryMonitor without endpoint → triggerMctpDiscovery (no-op)
    erot.runERoTRecoveryMonitor();
    EXPECT_TRUE(erot.erotRecoveryMonitorActive);

    // stopERoTRecoveryMonitor
    erot.stopERoTRecoveryMonitor();
    EXPECT_FALSE(erot.erotRecoveryMonitorActive);

    // runERoTRecoveryMonitor when inactive → early return
    erot.runERoTRecoveryMonitor();
    EXPECT_FALSE(erot.erotRecoveryMonitorActive);

    // Re-arm for the "endpoint present" path
    erot.erotRecoveryMonitorActive = true;

    // isMctpEndpointPresent: endpoint for EID=36 → true
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/36",
        "xyz.openbmc_project.MCTP.Endpoint", "EID", static_cast<uint8_t>(36));
    EXPECT_TRUE(erot.isMctpEndpointPresent());

    // runERoTRecoveryMonitor with endpoint → reevaluateERoTHealthAfterRecovery
    // GPIO deasserted (getValue=0, ActiveHigh) → FatalErrorDeassert → healthy
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    erot.runERoTRecoveryMonitor();
    EXPECT_FALSE(erot.isFirmwareInRecovery);
    EXPECT_EQ(erot.health(), HealthServer::HealthType::OK);
}

TEST_F(FWStatusGlacierGpioTest, GPIOResourceCoversRemainingBranchPaths)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    // ── AP boot-status: coroutine-in-progress early return ────────────────
    GPIOResource erotAP(bus, "/xyz/openbmc_project/software/erot-branches",
                        event, 7, 0x56, 37, "EROT_GPIO", "erot-br.target",
                        "Interrupt", std::nullopt, "ActiveHigh", "gpio-ap-br",
                        helper);

    // Inject a synthetic suspended handle to simulate an in-progress query
    auto suspended = suspendedBootStatusCoroutine(0);
    auto pendingHandle = suspended.handle;
    suspended.handle = nullptr; // prevent auto-clean in Coroutine destructor
    erotAP.apBootStatusCo = pendingHandle;
    erotAP.apBootStatusCheckActive = true;

    // runAPBootStatusQuery returns early: query already in progress
    EXPECT_TRUE(erotAP.apBootStatusCo && !erotAP.apBootStatusCo.done());
    erotAP.runAPBootStatusQuery();
    EXPECT_FALSE(erotAP.apBootStatusCo.done()); // still suspended

    // Clean up using the same pattern as existing tests
    erotAP.apBootStatusCo = nullptr;
    pendingHandle.destroy();

    // ── handleAPBootStatusUnavailable: inactive path ──────────────────────
    erotAP.apBootStatusCheckActive = false;
    erotAP.handleAPBootStatusUnavailable(); // early return: inactive

    // ── isMctpEndpointPresent: exception, mismatch, missing-intf paths ────
    test::fw_status_fake_gpio::lines["POLL_BR_GPIO"] = {};
    test::fw_status_fake_gpio::lines["POLL_BR_GPIO"].getValue = 0;
    GPIOResource erotPoll(bus, "/xyz/openbmc_project/software/erot-poll-br",
                          event, 8, 0x57, 38, "POLL_BR_GPIO", "", "Polling",
                          std::optional<uint64_t>(100), "ActiveHigh", "",
                          helper);

    // Exception → false
    test::fw_status_fake_dbus::throwOnGetManagedObjects = true;
    EXPECT_FALSE(erotPoll.isMctpEndpointPresent());
    test::fw_status_fake_dbus::throwOnGetManagedObjects = false;

    // Object missing MCTP endpoint interface → skip
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/other",
        "some.other.interface", "Prop", std::string("value"));
    EXPECT_FALSE(erotPoll.isMctpEndpointPresent());

    // Endpoint present but EID mismatch (99 ≠ 38) → false
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/99",
        "xyz.openbmc_project.MCTP.Endpoint", "EID", static_cast<uint8_t>(99));
    EXPECT_FALSE(erotPoll.isMctpEndpointPresent());

    // ── reevaluateERoTHealthAfterRecovery: asserted GPIO path ────────────
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));
    erotPoll.updateERoTHealth(
        GPIOResource::HealthUpdateReason::FatalErrorAssert);
    EXPECT_TRUE(erotPoll.isFirmwareInRecovery);

    // GPIO asserted (getValue=1) → FatalErrorAssert → stays in recovery
    test::fw_status_fake_gpio::lines["POLL_BR_GPIO"].getValue = 1;
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));
    erotPoll.reevaluateERoTHealthAfterRecovery();
    EXPECT_TRUE(erotPoll.isFirmwareInRecovery);

    // ── reevaluateERoTHealthAfterRecovery: lastGpioValue path ────────────
    // Invalidate gpioLine so the lastGpioValue branch is taken
    test::fw_status_fake_gpio::lines["POLL_BR_GPIO"].present = false;
    erotPoll.gpioLine = gpiod::line{};
    erotPoll.lastGpioValue = 0; // deasserted
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    erotPoll.reevaluateERoTHealthAfterRecovery();
    EXPECT_FALSE(erotPoll.isFirmwareInRecovery);

    // ── startERoTRecoveryMonitor: re-enter while timer already running ────
    test::fw_status_fake_gpio::lines["POLL_BR_GPIO"].present = true;
    test::fw_status_fake_gpio::lines["POLL_BR_GPIO"].getValue = 1;
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));
    erotPoll.gpioLine = gpiod::find_line("POLL_BR_GPIO");
    erotPoll.updateERoTHealth(
        GPIOResource::HealthUpdateReason::FatalErrorAssert);
    EXPECT_TRUE(erotPoll.erotRecoveryMonitorActive);
    // Second call: timer already running, match already exists → no-op
    erotPoll.startERoTRecoveryMonitor();
    EXPECT_TRUE(erotPoll.erotRecoveryMonitorActive);
}

TEST_F(FWStatusGlacierGpioTest, GPIOResourceCoversGuardAndDiscoveryBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    // ── Polling ERoT WITH a target: triggerMctpDiscovery restart branch ───
    test::fw_status_fake_gpio::lines["POLL_TGT_GPIO"] = {};
    test::fw_status_fake_gpio::lines["POLL_TGT_GPIO"].getValue = 1;
    GPIOResource erotTgt(
        bus, "/xyz/openbmc_project/software/erot-poll-tgt", event, 9, 0x58, 39,
        "POLL_TGT_GPIO", "erot-poll-tgt.target", "Polling",
        std::optional<uint64_t>(100), "ActiveHigh", "", helper);

    // Enter recovery → startERoTRecoveryMonitor → triggerMctpDiscovery restarts
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));
    erotTgt.updateERoTHealth(
        GPIOResource::HealthUpdateReason::FatalErrorAssert);
    EXPECT_TRUE(erotTgt.erotRecoveryMonitorActive);
    EXPECT_FALSE(test::fw_status_fake_dbus::restartedUnits.empty());

    // Timer tick with no endpoint present → triggerMctpDiscovery again
    test::fw_status_fake_dbus::restartedUnits.clear();
    erotTgt.runERoTRecoveryMonitor();
    EXPECT_EQ(test::fw_status_fake_dbus::restartedUnits,
              std::vector<std::string>{"erot-poll-tgt.target"});

    // ── Interrupt mode: startERoTRecoveryMonitor early return ────────────
    // Each interrupt-mode resource needs its own event fd: sd_event rejects a
    // second IO source registered on an fd it already watches.
    const int intrFd = dup(erotPipe[0]);
    ASSERT_GE(intrFd, 0);
    test::fw_status_fake_gpio::lines["EROT_INTR_MON_GPIO"] = {};
    test::fw_status_fake_gpio::lines["EROT_INTR_MON_GPIO"].eventFd = intrFd;
    GPIOResource erotIntr(bus, "/xyz/openbmc_project/software/erot-intr-mon",
                          event, 10, 0x59, 40, "EROT_INTR_MON_GPIO",
                          "erot-intr.target", "Interrupt", std::nullopt,
                          "ActiveHigh", "", helper);
    erotIntr.startERoTRecoveryMonitor(); // no-op: not Polling
    EXPECT_FALSE(erotIntr.erotRecoveryMonitorActive);
    EXPECT_EQ(erotIntr.erotRecoveryMonitorTimer, nullptr);

    // stopERoTRecoveryMonitor with no timer → early return
    erotIntr.stopERoTRecoveryMonitor();
    EXPECT_FALSE(erotIntr.erotRecoveryMonitorActive);

    // triggerMctpDiscovery is a no-op only when the target is empty
    erotIntr.systemTarget.clear();
    test::fw_status_fake_dbus::restartedUnits.clear();
    erotIntr.triggerMctpDiscovery();
    EXPECT_TRUE(test::fw_status_fake_dbus::restartedUnits.empty());

    // ── AP helpers with no AP configured: all guard branches ─────────────
    EXPECT_FALSE(erotIntr.hasAP());
    erotIntr.startAPBootStatusCheck(); // !hasAP()
    erotIntr.runAPBootStatusQuery();   // !apBootStatusCheckActive
    erotIntr.scheduleAPBootStatusQuery(std::chrono::seconds(1)); // !hasAP()
    erotIntr.stopAPBootStatusCheck();                            // no timer
    erotIntr.handleAPBootStatusUnavailable();                    // !hasAP()
    erotIntr.updateAPHealth(HealthServer::HealthType::Critical,
                            OperationalStatusServer::StateType::Degraded);
    erotIntr.deleteAPObject(); // apResource == nullptr
    EXPECT_EQ(erotIntr.apResource, nullptr);
    EXPECT_EQ(erotIntr.apBootStatusRetryTimer, nullptr);

    // ── AP configured but no MCTP VDM helper: startAPBootStatusCheck warns ─
    const int noHelperFd = dup(erotPipe[0]);
    ASSERT_GE(noHelperFd, 0);
    test::fw_status_fake_gpio::lines["EROT_NOHELPER_GPIO"] = {};
    test::fw_status_fake_gpio::lines["EROT_NOHELPER_GPIO"].eventFd = noHelperFd;
    GPIOResource erotNoHelper(
        bus, "/xyz/openbmc_project/software/erot-nohelper", event, 11, 0x5A, 41,
        "EROT_NOHELPER_GPIO", "erot-nohelper.target", "Interrupt", std::nullopt,
        "ActiveHigh", "gpio-ap-nohelper", nullptr);
    EXPECT_TRUE(erotNoHelper.hasAP());
    erotNoHelper.startAPBootStatusCheck(); // !mctpVdmHelper → early return
    EXPECT_FALSE(erotNoHelper.apBootStatusCheckActive);

    // runAPBootStatusQuery with active check but no helper → early return
    erotNoHelper.apBootStatusCheckActive = true;
    erotNoHelper.runAPBootStatusQuery();
    EXPECT_EQ(erotNoHelper.apBootStatusCo, nullptr);
    erotNoHelper.apBootStatusCheckActive = false;
}

} // namespace
