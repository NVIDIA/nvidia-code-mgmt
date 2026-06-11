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

TEST_F(FWStatusGlacierGpioTest, GPIOResourceCoversERoTAndAPBranches)
{
    auto event = sdeventplus::Event::get_default();
    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));

    GPIOResource erot(bus, "/xyz/openbmc_project/software/erot-gpio", event, 1,
                      0x50, 30, "EROT_GPIO", "erot.target", "Interrupt",
                      std::nullopt, "ActiveHigh");
    static_cast<BaseResource&>(erot).updateHealth();
    EXPECT_EQ(erot.state(), OperationalStatusServer::StateType::StandbyOffline);

    erot.isFirmwareInRecovery = true;
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    erot.updateERoTHealth();
    EXPECT_EQ(test::fw_status_fake_dbus::restartedUnits,
              std::vector<std::string>{"erot.target"});
    EXPECT_FALSE(erot.isFirmwareInRecovery);

    test::fw_status_fake_vdm::bootStatusPayload = {0x00, 0x00, 0x00, 0x00,
                                                   0x08};
    test::fw_status_fake_gpio::lines["AP_GPIO"].getValue = 1;
    GPIOResource ap(bus, "/xyz/openbmc_project/software/ap-gpio", event, 31,
                    "AP_GPIO", "rise.target", "fall.target", "InvalidPolarity",
                    "/xyz/openbmc_project/state/chassis/ap",
                    std::make_shared<MCTPVdmHelper>());
    ASSERT_NE(ap.resourceDbusObj, nullptr);
    EXPECT_EQ(ap.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(ap.state(), OperationalStatusServer::StateType::Enabled);
    ap.hideWhenHealthy = true;
    ap.updateHealthyState();
    EXPECT_EQ(ap.resourceDbusObj, nullptr);
    ap.hideWhenHealthy = false;
    EXPECT_EQ(test::fw_status_fake_dbus::startedUnits,
              std::vector<std::string>{"rise.target"});

    test::fw_status_fake_gpio::lines["AP_GPIO"].eventType =
        gpiod::line_event::FALLING_EDGE;
    ap.waitForGPIOEvent();
    EXPECT_EQ(ap.state(), OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_EQ(test::fw_status_fake_dbus::startedUnits,
              (std::vector<std::string>{"rise.target", "fall.target"}));
    EXPECT_GT(test::fw_status_fake_vdm::queryCalls, 0);

    test::fw_status_fake_glacier::pushResult(static_cast<uint8_t>(
        glacier_recovery_tool::glacier_recovery_commands::RecoveryResult::Ok));
    static_cast<BaseResource&>(erot).updateHealth();
    EXPECT_TRUE(erot.isFirmwareInRecovery);

    ap.setConnectedToChassis(true);
    ap.setChassisPowerState("xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(ap).updateHealth();
    EXPECT_EQ(ap.health(), HealthServer::HealthType::Warning);
    EXPECT_EQ(ap.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    ap.setChassisPowerState("xyz.openbmc_project.State.Chassis.PowerState.On");
    test::fw_status_fake_gpio::lines["AP_GPIO"].getValue = 0;
    static_cast<BaseResource&>(ap).updateHealth();
    EXPECT_EQ(ap.state(), OperationalStatusServer::StateType::StandbyOffline);
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
    EXPECT_TRUE(getBit({0x00, 0x00, 0x00, 0x08}, 3));
    EXPECT_FALSE(getBit({0x00, 0x00, 0x00, 0x00}, 3));
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
       GPIOAndERoTUpdateBootStatusCoverPendingCoroutineBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();

    test::fw_status_fake_gpio::lines["AP_PENDING_GPIO"] = {};
    test::fw_status_fake_gpio::lines["AP_PENDING_GPIO"].eventFd =
        dup(apPipe[0]);
    test::fw_status_fake_gpio::lines["AP_PENDING_GPIO"].getValue = 1;
    ASSERT_GE(test::fw_status_fake_gpio::lines["AP_PENDING_GPIO"].eventFd, 0);

    GPIOResource ap(bus, "/xyz/openbmc_project/software/ap-pending", event, 91,
                    "AP_PENDING_GPIO", "rise-pending", "fall-pending",
                    "ActiveHigh",
                    "/xyz/openbmc_project/state/chassis/ap-pending", helper);

    auto existing = suspendedBootStatusCoroutine(1);
    auto existingHandle = existing.handle;
    existing.handle = nullptr;
    ASSERT_TRUE(existingHandle);
    ASSERT_FALSE(existingHandle.done());

    ap.co = existingHandle;
    test::fw_status_fake_vdm::suspendQuery = true;
    ap.updateBootStatus();
    auto replacementHandle = ap.co;
    ASSERT_TRUE(replacementHandle);
    EXPECT_NE(replacementHandle, existingHandle);
    EXPECT_TRUE(existingHandle.promise().detached);
    ap.co = nullptr;
    replacementHandle.destroy();
    existingHandle.destroy();

    ap.updateBootStatus();
    auto pendingApHandle = ap.co;
    ap.co = nullptr;
    ASSERT_TRUE(pendingApHandle);
    EXPECT_FALSE(pendingApHandle.done());
    pendingApHandle.destroy();
    test::fw_status_fake_vdm::suspendQuery = false;

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
    const int apThrowFd = dup(apPipe[0]);
    const int apLowFd = dup(apPipe[0]);
    const int apEdgeFd = dup(apPipe[0]);
    ASSERT_GE(erotEventFd, 0);
    ASSERT_GE(erotOffFd, 0);
    ASSERT_GE(apThrowFd, 0);
    ASSERT_GE(apLowFd, 0);
    ASSERT_GE(apEdgeFd, 0);

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
                           "ActiveHigh");
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
                           "ActiveHigh");
    EXPECT_EQ(erotBadFd.gpioEvent, nullptr);

    test::fw_status_fake_gpio::lines["EROT_OFF_GPIO"] = {};
    test::fw_status_fake_gpio::lines["EROT_OFF_GPIO"].eventFd = erotOffFd;
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    GPIOResource erotOff(bus, "/xyz/openbmc_project/software/erot-off", event,
                         4, 0x53, 34, "EROT_OFF_GPIO", "erot-off.target",
                         "Interrupt", std::nullopt, "ActiveHigh");
    erotOff.setConnectedToChassis(true);
    erotOff.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(erotOff).updateHealth();
    EXPECT_EQ(erotOff.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    GPIOResource missingAp(
        bus, "/xyz/openbmc_project/software/ap-missing", event, 41,
        "AP_MISSING_GPIO", "rise-missing", "fall-missing", "ActiveLow",
        "/xyz/openbmc_project/state/chassis/ap-missing", helper);
    EXPECT_EQ(missingAp.gpioEvent, nullptr);

    test::fw_status_fake_gpio::lines["AP_THROW_GPIO"] = {};
    test::fw_status_fake_gpio::lines["AP_THROW_GPIO"].eventFd = apThrowFd;
    test::fw_status_fake_gpio::lines["AP_THROW_GPIO"].throwOnRequest = true;
    GPIOResource throwAp(bus, "/xyz/openbmc_project/software/ap-throw", event,
                         42, "AP_THROW_GPIO", "rise-throw", "fall-throw",
                         "ActiveLow",
                         "/xyz/openbmc_project/state/chassis/ap-throw", helper);
    EXPECT_EQ(throwAp.gpioEvent, nullptr);

    test::fw_status_fake_gpio::lines["AP_LOW_GPIO"] = {};
    test::fw_status_fake_gpio::lines["AP_LOW_GPIO"].eventFd = apLowFd;
    test::fw_status_fake_gpio::lines["AP_LOW_GPIO"].getValue = 0;
    GPIOResource activeLow(bus, "/xyz/openbmc_project/software/ap-low", event,
                           43, "AP_LOW_GPIO", "rise-low", "fall-low",
                           "ActiveLow",
                           "/xyz/openbmc_project/state/chassis/ap-low", helper);
    EXPECT_EQ(test::fw_status_fake_dbus::startedUnits,
              (std::vector<std::string>{"fall-low"}));

    test::fw_status_fake_gpio::lines["AP_LOW_GPIO"].getValue = 1;
    activeLow.updateAPHealth(0);
    EXPECT_EQ(test::fw_status_fake_dbus::startedUnits,
              (std::vector<std::string>{"fall-low", "rise-low"}));

    test::fw_status_fake_gpio::lines["AP_EDGE_GPIO"] = {};
    test::fw_status_fake_gpio::lines["AP_EDGE_GPIO"].eventFd = apEdgeFd;
    test::fw_status_fake_gpio::lines["AP_EDGE_GPIO"].getValue = 1;
    test::fw_status_fake_gpio::lines["AP_EDGE_GPIO"].eventType =
        gpiod::line_event::RISING_EDGE;
    GPIOResource activeHigh(
        bus, "/xyz/openbmc_project/software/ap-edge", event, 44, "AP_EDGE_GPIO",
        "rise-edge", "fall-edge", "ActiveHigh",
        "/xyz/openbmc_project/state/chassis/ap-edge", helper);
    activeHigh.waitForGPIOEvent();
    EXPECT_EQ(test::fw_status_fake_dbus::startedUnits,
              (std::vector<std::string>{"fall-low", "rise-low", "rise-edge",
                                        "rise-edge"}));

    activeHigh.updateAPHealth(0xFF);
}

TEST_F(FWStatusGlacierGpioTest, APResourceCoversEmptyTargetEdgeAndLevelBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto helper = std::make_shared<MCTPVdmHelper>();
    std::vector<int> extraFds;

    auto makeEventFd = [&] {
        const int fd = dup(apPipe[0]);
        EXPECT_GE(fd, 0);
        if (fd >= 0)
        {
            extraFds.push_back(fd);
        }
        return fd;
    };

    test::fw_status_fake_gpio::lines["AP_EDGE_HIGH_EMPTY_RISE"] = {};
    test::fw_status_fake_gpio::lines["AP_EDGE_HIGH_EMPTY_RISE"].eventFd =
        makeEventFd();
    test::fw_status_fake_gpio::lines["AP_EDGE_HIGH_EMPTY_RISE"].getValue = 1;
    test::fw_status_fake_gpio::lines["AP_EDGE_HIGH_EMPTY_RISE"].eventType =
        gpiod::line_event::RISING_EDGE;
    GPIOResource edgeHighEmptyRise(
        bus, "/xyz/openbmc_project/software/ap-edge-high-empty-rise", event, 90,
        "AP_EDGE_HIGH_EMPTY_RISE", "", "fall-edge-only", "ActiveHigh",
        "/xyz/openbmc_project/state/chassis/ap-edge-high-empty-rise", helper);
    edgeHighEmptyRise.updateAPHealth(0);
    edgeHighEmptyRise.waitForGPIOEvent();

    test::fw_status_fake_gpio::lines["AP_EDGE_EMPTY_FALL"] = {};
    test::fw_status_fake_gpio::lines["AP_EDGE_EMPTY_FALL"].eventFd =
        makeEventFd();
    test::fw_status_fake_gpio::lines["AP_EDGE_EMPTY_FALL"].getValue = 1;
    test::fw_status_fake_gpio::lines["AP_EDGE_EMPTY_FALL"].eventType =
        gpiod::line_event::FALLING_EDGE;
    GPIOResource edgeEmptyFall(
        bus, "/xyz/openbmc_project/software/ap-edge-empty-fall", event, 91,
        "AP_EDGE_EMPTY_FALL", "", "", "ActiveHigh",
        "/xyz/openbmc_project/state/chassis/ap-edge-empty-fall", helper);
    edgeEmptyFall.waitForGPIOEvent();

    test::fw_status_fake_gpio::lines["AP_LEVEL_LOW_EMPTY_FALL"] = {};
    test::fw_status_fake_gpio::lines["AP_LEVEL_LOW_EMPTY_FALL"].eventFd =
        makeEventFd();
    test::fw_status_fake_gpio::lines["AP_LEVEL_LOW_EMPTY_FALL"].getValue = 0;
    GPIOResource lowHealthyEmptyFall(
        bus, "/xyz/openbmc_project/software/ap-low-empty-fall", event, 92,
        "AP_LEVEL_LOW_EMPTY_FALL", "rise-low-only", "", "ActiveLow",
        "/xyz/openbmc_project/state/chassis/ap-low-empty-fall", helper);
    lowHealthyEmptyFall.updateAPHealth(0);

    test::fw_status_fake_gpio::lines["AP_LEVEL_HIGH_EMPTY_FALL"] = {};
    test::fw_status_fake_gpio::lines["AP_LEVEL_HIGH_EMPTY_FALL"].eventFd =
        makeEventFd();
    test::fw_status_fake_gpio::lines["AP_LEVEL_HIGH_EMPTY_FALL"].getValue = 0;
    GPIOResource highUnhealthyEmptyFall(
        bus, "/xyz/openbmc_project/software/ap-high-empty-fall", event, 93,
        "AP_LEVEL_HIGH_EMPTY_FALL", "rise-high-only", "", "ActiveHigh",
        "/xyz/openbmc_project/state/chassis/ap-high-empty-fall", helper);
    highUnhealthyEmptyFall.updateAPHealth(0);
    EXPECT_EQ(highUnhealthyEmptyFall.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_gpio::lines["AP_LEVEL_LOW_EMPTY_RISE"] = {};
    test::fw_status_fake_gpio::lines["AP_LEVEL_LOW_EMPTY_RISE"].eventFd =
        makeEventFd();
    test::fw_status_fake_gpio::lines["AP_LEVEL_LOW_EMPTY_RISE"].getValue = 1;
    GPIOResource lowUnhealthyEmptyRise(
        bus, "/xyz/openbmc_project/software/ap-low-empty-rise", event, 94,
        "AP_LEVEL_LOW_EMPTY_RISE", "", "fall-low-only", "ActiveLow",
        "/xyz/openbmc_project/state/chassis/ap-low-empty-rise", helper);
    lowUnhealthyEmptyRise.updateAPHealth(0);
    EXPECT_EQ(lowUnhealthyEmptyRise.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    for (int fd : extraFds)
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }
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

} // namespace
