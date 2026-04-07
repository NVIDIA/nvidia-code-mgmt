#include <sdbusplus/message.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::NiceMock;

namespace
{

struct FakeDeviceData
{
    std::optional<std::string> action;
    std::optional<std::string> devpath;
};

struct FakeUdevState
{
    bool failUdevNew = false;
    bool failMonitorNew = false;
    int filterResult = 0;
    int enableResult = 0;
    int unrefUdevCount = 0;
    int unrefMonitorCount = 0;
    int unrefDeviceCount = 0;
    std::optional<FakeDeviceData> pendingDevice;
    int pipeFds[2] = {-1, -1};
};

FakeUdevState fakeUdev;

void resetFakeUdev()
{
    fakeUdev = {};
    if (pipe(fakeUdev.pipeFds) != 0)
    {
        throw std::runtime_error("failed to create fake udev pipe");
    }
}

} // namespace

extern "C" struct udev* __wrap_udev_new()
{
    if (fakeUdev.failUdevNew)
    {
        return nullptr;
    }
    return reinterpret_cast<struct udev*>(0x1);
}

extern "C" struct udev_monitor*
    __wrap_udev_monitor_new_from_netlink(struct udev*, const char*)
{
    if (fakeUdev.failMonitorNew)
    {
        return nullptr;
    }
    return reinterpret_cast<struct udev_monitor*>(0x2);
}

extern "C" int __wrap_udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor*, const char*, const char*)
{
    return fakeUdev.filterResult;
}

extern "C" int __wrap_udev_monitor_enable_receiving(struct udev_monitor*)
{
    return fakeUdev.enableResult;
}

extern "C" int __wrap_udev_monitor_get_fd(struct udev_monitor*)
{
    return fakeUdev.pipeFds[0];
}

extern "C" struct udev_device*
    __wrap_udev_monitor_receive_device(struct udev_monitor*)
{
    if (!fakeUdev.pendingDevice.has_value())
    {
        return nullptr;
    }
    return reinterpret_cast<struct udev_device*>(0x3);
}

extern "C" const char* __wrap_udev_device_get_action(struct udev_device*)
{
    if (!fakeUdev.pendingDevice || !fakeUdev.pendingDevice->action.has_value())
    {
        return nullptr;
    }
    return fakeUdev.pendingDevice->action->c_str();
}

extern "C" const char* __wrap_udev_device_get_devpath(struct udev_device*)
{
    if (!fakeUdev.pendingDevice || !fakeUdev.pendingDevice->devpath.has_value())
    {
        return nullptr;
    }
    return fakeUdev.pendingDevice->devpath->c_str();
}

extern "C" struct udev* __wrap_udev_unref(struct udev* ctx)
{
    ++fakeUdev.unrefUdevCount;
    return ctx;
}

extern "C" struct udev_monitor*
    __wrap_udev_monitor_unref(struct udev_monitor* mon)
{
    ++fakeUdev.unrefMonitorCount;
    return mon;
}

extern "C" struct udev_device* __wrap_udev_device_unref(struct udev_device* dev)
{
    ++fakeUdev.unrefDeviceCount;
    fakeUdev.pendingDevice.reset();
    return dev;
}

#define private public
#define protected public
#pragma GCC push_options
#pragma GCC optimize("no-inline")
#include "../fw-status/mcu_resource.hpp"

#include "../fw-status/force_recovery/mcu_recovery_mode_manager.cpp"
#include "../fw-status/force_recovery/usbrcm_recovery_manager.cpp"
#include "../fw-status/mctp_discovery_resource.cpp"
#include "../fw-status/udev_monitor.cpp"
#include "../fw-status/usb_rcm_resource.cpp"
#pragma GCC pop_options
#undef protected
#undef private

namespace
{

class TestDiscoveryResource : public MCTPDiscoveryResource
{
  public:
    TestDiscoveryResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                          uint8_t eid) :
        MCTPDiscoveryResource(bus, objPath, eid)
    {}

    int updateCalls = 0;

    void updateHealth() override
    {
        ++updateCalls;
    }

    const std::string& currentMctpPath() const
    {
        return mctpObjectPath;
    }

    bool wasEnumerated() const
    {
        return wasEnumeratedOnce;
    }
};

class DummyRecoveryModeManager :
    public nvidia::recovery::RecoveryModeManagerBase
{
  public:
    enum class Behavior
    {
        success,
        sdbusFailure,
        runtimeFailure,
    };

    DummyRecoveryModeManager(sdbusplus::bus_t& bus, std::string chassisName,
                             std::string objPath) :
        RecoveryModeManagerBase(bus, std::move(chassisName), std::move(objPath))
    {}

    Behavior behavior = Behavior::success;

  protected:
    void performForceRecovery() override
    {
        switch (behavior)
        {
            case Behavior::success:
                return;
            case Behavior::sdbusFailure:
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    InternalFailure();
            case Behavior::runtimeFailure:
                throw std::runtime_error("dummy runtime failure");
        }
    }
};

int dispatchDiscoveryMatch(
    const std::unique_ptr<sdbusplus::bus::match_t>& match,
    sdbusplus::message::message& msg)
{
    if (!match || !match->_callback)
    {
        return 0;
    }
    (void)sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);
    (*match->_callback)(msg);
    return 1;
}

sdbusplus::message::message makeInterfacesAddedSignal(
    sdbusplus::bus_t& sender, const std::string& objectPath,
    const nvidia::software::updater::InterfaceMap& interfaces)
{
    auto msg = sender.new_signal("/au/com/codeconstruct/mctp1",
                                 "org.freedesktop.DBus.ObjectManager",
                                 "InterfacesAdded");
    msg.append(sdbusplus::message::object_path(objectPath), interfaces);
    return msg;
}

sdbusplus::message::message
    makeInterfacesRemovedSignal(sdbusplus::bus_t& sender,
                                const std::string& objectPath)
{
    auto msg = sender.new_signal("/au/com/codeconstruct/mctp1",
                                 "org.freedesktop.DBus.ObjectManager",
                                 "InterfacesRemoved");
    msg.append(sdbusplus::message::object_path(objectPath),
               std::vector<std::string>{mctpEndpointIntfName});
    return msg;
}

class FWStatusMiscTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        resetFakeUdev();
        test::fw_status_fake_dbus::reset();
        test::fw_status_fake_usb_recovery::reset();
        test::fw_status_fake_force_recovery::reset();
        test::fw_status_fake_mcu::reset();
    }

    void TearDown() override
    {
        if (fakeUdev.pipeFds[0] >= 0)
        {
            close(fakeUdev.pipeFds[0]);
        }
        if (fakeUdev.pipeFds[1] >= 0)
        {
            close(fakeUdev.pipeFds[1]);
        }
    }

    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
};

TEST_F(FWStatusMiscTest, UdevMonitorConstructorFailurePathsThrow)
{
    auto event = sdeventplus::Event::get_default();

    fakeUdev.failUdevNew = true;
    EXPECT_THROW((void)UdevMonitor(event), std::runtime_error);
    fakeUdev.failUdevNew = false;

    fakeUdev.failMonitorNew = true;
    EXPECT_THROW((void)UdevMonitor(event), std::runtime_error);
    EXPECT_EQ(fakeUdev.unrefUdevCount, 1);
    fakeUdev.failMonitorNew = false;

    fakeUdev.filterResult = -1;
    EXPECT_THROW((void)UdevMonitor(event), std::runtime_error);
    EXPECT_EQ(fakeUdev.unrefMonitorCount, 1);
    EXPECT_EQ(fakeUdev.unrefUdevCount, 2);
    fakeUdev.filterResult = 0;

    fakeUdev.enableResult = -1;
    EXPECT_THROW((void)UdevMonitor(event), std::runtime_error);
    EXPECT_EQ(fakeUdev.unrefMonitorCount, 2);
    EXPECT_EQ(fakeUdev.unrefUdevCount, 3);
}

TEST_F(FWStatusMiscTest, UdevMonitorHandlesAddAndNonAddEvents)
{
    auto event = sdeventplus::Event::get_default();
    UdevMonitor monitor(event);
    int callbackCount = 0;

    monitor.registerCallback("1-2.3", [&]() { ++callbackCount; });
    fakeUdev.pendingDevice = FakeDeviceData{
        .action = std::string("add"),
        .devpath = std::string("/sys/devices/platform/usb1/1-2/1-2.3"),
    };
    monitor.handleUdevEvent();
    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(fakeUdev.unrefDeviceCount, 1);

    fakeUdev.pendingDevice = FakeDeviceData{
        .action = std::string("remove"),
        .devpath = std::string("/sys/devices/platform/usb1/1-2/1-2.3"),
    };
    monitor.handleUdevEvent();
    EXPECT_EQ(callbackCount, 1);

    fakeUdev.pendingDevice = FakeDeviceData{
        .action = std::nullopt,
        .devpath = std::string("/sys/devices/platform/usb1/1-2/1-2.3"),
    };
    monitor.handleUdevEvent();
    EXPECT_EQ(fakeUdev.unrefDeviceCount, 3);

    EXPECT_EQ(monitor.extractPortPath(reinterpret_cast<::udev_device*>(0x3)),
              "");
}

TEST_F(FWStatusMiscTest,
       UdevMonitorCoversUnregisterMissingCallbacksAndEmptyPath)
{
    auto event = sdeventplus::Event::get_default();
    UdevMonitor monitor(event);
    int callbackCount = 0;

    monitor.registerCallback("2-3.4", [&]() { ++callbackCount; });
    monitor.unregisterCallback("2-3.4");

    fakeUdev.pendingDevice = FakeDeviceData{
        .action = std::string("add"),
        .devpath = std::string("/sys/devices/platform/usb1/2-3/2-3.4"),
    };
    monitor.handleUdevEvent();
    EXPECT_EQ(callbackCount, 0);

    fakeUdev.pendingDevice = FakeDeviceData{
        .action = std::string("add"),
        .devpath = std::nullopt,
    };
    monitor.handleUdevEvent();

    fakeUdev.pendingDevice = FakeDeviceData{
        .action = std::string("add"),
        .devpath = std::string("/"),
    };
    EXPECT_TRUE(
        monitor.extractPortPath(reinterpret_cast<::udev_device*>(0x3)).empty());
    monitor.handleUdevEvent();

    EXPECT_EQ(fakeUdev.unrefDeviceCount, 3);
}

TEST_F(FWStatusMiscTest, UdevMonitorCoversNoDeviceAndNullDestructorBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto monitor = std::make_unique<UdevMonitor>(event);

    monitor->handleUdevEvent();
    EXPECT_EQ(fakeUdev.unrefDeviceCount, 0);

    monitor->monitor = nullptr;
    monitor->udev = nullptr;
    monitor.reset();

    EXPECT_EQ(fakeUdev.unrefMonitorCount, 0);
    EXPECT_EQ(fakeUdev.unrefUdevCount, 0);
}

TEST_F(FWStatusMiscTest, UdevMonitorExhaustivelyCoversActionCallbackTruthTable)
{
    auto event = sdeventplus::Event::get_default();
    UdevMonitor monitor(event);
    int callbackCount = 0;

    monitor.registerCallback("3-4.5", [&]() { ++callbackCount; });

    auto dispatch = [&](std::string action, std::string portPath) {
        fakeUdev.pendingDevice = FakeDeviceData{
            .action = std::move(action),
            .devpath = std::string("/sys/devices/platform/usb1/") + portPath,
        };
        monitor.handleUdevEvent();
    };

    dispatch("add", "3-4/3-4.5");
    EXPECT_EQ(callbackCount, 1);

    dispatch("add", "3-4/3-4.6");
    EXPECT_EQ(callbackCount, 1);

    dispatch("change", "3-4/3-4.5");
    EXPECT_EQ(callbackCount, 1);

    dispatch("change", "3-4/3-4.6");
    EXPECT_EQ(callbackCount, 1);

    EXPECT_EQ(fakeUdev.unrefDeviceCount, 4);
}

TEST_F(FWStatusMiscTest, MctpDiscoveryResourceCoversSignalBranches)
{
    TestDiscoveryResource resource(bus, "/xyz/openbmc_project/software/mctp0",
                                   9);
    ASSERT_NE(resource.endpointAddedMatch, nullptr);
    ASSERT_NE(resource.endpointRemovedMatch, nullptr);
    EXPECT_TRUE(resource.currentMctpPath().empty());
    EXPECT_FALSE(resource.wasEnumerated());

    auto signalBus = sdbusplus::bus::new_default();

    nvidia::software::updater::InterfaceMap unrelatedInterfaces{
        {"xyz.openbmc_project.Unrelated", {}}};
    auto unrelatedAdded = makeInterfacesAddedSignal(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
        unrelatedInterfaces);
    EXPECT_GT(
        dispatchDiscoveryMatch(resource.endpointAddedMatch, unrelatedAdded), 0);
    EXPECT_EQ(resource.updateCalls, 0);

    nvidia::software::updater::InterfaceMap wrongEidInterfaces{
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(8)}}}};
    auto wrongEidAdded = makeInterfacesAddedSignal(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/8",
        wrongEidInterfaces);
    EXPECT_GT(
        dispatchDiscoveryMatch(resource.endpointAddedMatch, wrongEidAdded), 0);
    EXPECT_EQ(resource.updateCalls, 0);

    nvidia::software::updater::InterfaceMap badTypeInterfaces{
        {mctpEndpointIntfName, {{"EID", std::string("bad")}}}};
    auto badTypeAdded = makeInterfacesAddedSignal(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
        badTypeInterfaces);
    EXPECT_GT(dispatchDiscoveryMatch(resource.endpointAddedMatch, badTypeAdded),
              0);
    EXPECT_EQ(resource.updateCalls, 0);

    nvidia::software::updater::InterfaceMap matchingInterfaces{
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(9)}}}};
    const std::string endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/9";
    auto matchingAdded =
        makeInterfacesAddedSignal(signalBus, endpointPath, matchingInterfaces);
    EXPECT_GT(
        dispatchDiscoveryMatch(resource.endpointAddedMatch, matchingAdded), 0);
    EXPECT_EQ(resource.updateCalls, 1);
    EXPECT_EQ(resource.currentMctpPath(), endpointPath);
    EXPECT_TRUE(resource.wasEnumerated());

    auto malformedAdded = signalBus.new_signal(
        "/au/com/codeconstruct/mctp1", "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded");
    malformedAdded.append(std::string("bad-payload"));
    EXPECT_GT(
        dispatchDiscoveryMatch(resource.endpointAddedMatch, malformedAdded), 0);
    EXPECT_EQ(resource.updateCalls, 1);

    auto unrelatedRemoved = makeInterfacesRemovedSignal(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/99");
    EXPECT_GT(
        dispatchDiscoveryMatch(resource.endpointRemovedMatch, unrelatedRemoved),
        0);
    EXPECT_EQ(resource.updateCalls, 1);
    EXPECT_EQ(resource.currentMctpPath(), endpointPath);

    auto matchingRemoved = makeInterfacesRemovedSignal(signalBus, endpointPath);
    EXPECT_GT(
        dispatchDiscoveryMatch(resource.endpointRemovedMatch, matchingRemoved),
        0);
    EXPECT_EQ(resource.updateCalls, 2);
    EXPECT_TRUE(resource.currentMctpPath().empty());

    auto malformedRemoved = signalBus.new_signal(
        "/au/com/codeconstruct/mctp1", "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved");
    malformedRemoved.append(std::string("bad-payload"));
    EXPECT_GT(
        dispatchDiscoveryMatch(resource.endpointRemovedMatch, malformedRemoved),
        0);
    EXPECT_EQ(resource.updateCalls, 2);
}

TEST_F(FWStatusMiscTest,
       MctpDiscoveryResourceReusesExistingMatchesWithoutRecreatingThem)
{
    TestDiscoveryResource resource(bus, "/xyz/openbmc_project/software/mctp1",
                                   10);
    auto* added = resource.endpointAddedMatch.get();
    auto* removed = resource.endpointRemovedMatch.get();

    resource.monitorMCTPEndpoint();

    EXPECT_EQ(resource.endpointAddedMatch.get(), added);
    EXPECT_EQ(resource.endpointRemovedMatch.get(), removed);
}

TEST_F(FWStatusMiscTest, USBRcmResourceTracksRecoveryStatusAndCompanionState)
{
    auto event = sdeventplus::Event::get_default();
    auto monitor = std::make_shared<UdevMonitor>(event);

    test::fw_status_fake_usb_recovery::fallbackPayload = nlohmann::json::array(
        {{{"USB Port Path", "1-1.1"}, {"Recovery Status", "In Recovery"}}});

    USBRcmResource resource(bus, "/xyz/openbmc_project/software/fmc", 9,
                            "1-1.1", "/xyz/openbmc_project/software/fws",
                            monitor);
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_EQ(resource.companionResource->health(),
              HealthServer::HealthType::Critical);
    EXPECT_EQ(resource.companionResource->state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_usb_recovery::fallbackPayload =
        nlohmann::json::array({{{"USB Port Path", "1-1.1"},
                                {"Recovery Status", "Recovery Complete"}}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);

    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.health(), HealthServer::HealthType::Warning);
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST_F(FWStatusMiscTest, USBRcmResourceHandlesEnumeratedAndMissingPortCases)
{
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(9));
    test::fw_status_fake_usb_recovery::fallbackPayload = nlohmann::json::array(
        {{{"USB Port Path", "9-9"}, {"Recovery Status", "Unknown"}}});

    USBRcmResource enumerated(bus, "/xyz/openbmc_project/software/fmc2", 9,
                              "9-9", "/xyz/openbmc_project/software/fws2",
                              nullptr);
    static_cast<BaseResource&>(enumerated).updateHealth();
    EXPECT_EQ(enumerated.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(enumerated.state(), OperationalStatusServer::StateType::Enabled);

    test::fw_status_fake_dbus::reset();
    test::fw_status_fake_usb_recovery::fallbackPayload = nlohmann::json::array(
        {{{"USB Port Path", "1-1"}, {"Recovery Status", "Unknown"}}});
    USBRcmResource absent(bus, "/xyz/openbmc_project/software/fmc3", 10, "2-2",
                          "/xyz/openbmc_project/software/fws3", nullptr);
    static_cast<BaseResource&>(absent).updateHealth();
    EXPECT_EQ(absent.state(), OperationalStatusServer::StateType::Absent);
}

TEST_F(FWStatusMiscTest, USBRcmResourceQueryHelperCoversUnexpectedPayloads)
{
    test::fw_status_fake_usb_recovery::fallbackPayload = nlohmann::json::array(
        {{{"USB Port Path", "4-4"}, {"Recovery Status", "Not in Recovery"}}});

    USBRcmResource resource(bus, "/xyz/openbmc_project/software/fmc-query", 11,
                            "4-4", "/xyz/openbmc_project/software/fws-query",
                            nullptr);

    test::fw_status_fake_usb_recovery::pushReply(false, {{"Error", "boom"}});
    EXPECT_EQ(resource.queryUSBRecoveryStatus(), "Unknown");

    test::fw_status_fake_usb_recovery::pushReply(true, {{"Error", "bad"}});
    EXPECT_EQ(resource.queryUSBRecoveryStatus(), "USB Port Not Found");

    test::fw_status_fake_usb_recovery::pushReply(
        true, nlohmann::json{{"Status", "unexpected"}});
    EXPECT_EQ(resource.queryUSBRecoveryStatus(), "Unknown");

    test::fw_status_fake_usb_recovery::pushReply(
        true, nlohmann::json::array(
                  {{{"Recovery Status", "In Recovery"}},
                   {{"USB Port Path", "4-4"}, {"Error", "device error"}},
                   {{"USB Port Path", "4-4"}}}));
    EXPECT_EQ(resource.queryUSBRecoveryStatus(), "Unknown");
}

TEST_F(FWStatusMiscTest, MCUResourceCoversHealthyRecoveryAndAbsentPaths)
{
    auto manager = std::make_shared<mcu_recovery_manager::MCURecoveryManager>();

    test::fw_status_fake_mcu::devices["dev0"] = {.healthy = true,
                                                 .inRecovery = false};
    MCUResource healthy(bus, "/xyz/openbmc_project/software/mcu0", 20, "dev0",
                        manager);
    static_cast<BaseResource&>(healthy).updateHealth();
    EXPECT_TRUE(test::fw_status_fake_mcu::updateAllCalled);
    EXPECT_EQ(healthy.state(), OperationalStatusServer::StateType::Degraded);

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/21",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(21));
    test::fw_status_fake_mcu::devices["dev1"] = {.healthy = false,
                                                 .inRecovery = true};
    MCUResource enumerated(bus, "/xyz/openbmc_project/software/mcu1", 21,
                           "dev1", manager);
    static_cast<BaseResource&>(enumerated).updateHealth();
    EXPECT_EQ(enumerated.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(enumerated.state(), OperationalStatusServer::StateType::Enabled);

    test::fw_status_fake_dbus::reset();
    test::fw_status_fake_mcu::devices["dev2"] = {.healthy = false,
                                                 .inRecovery = true};
    MCUResource recovery(bus, "/xyz/openbmc_project/software/mcu2", 22, "dev2",
                         manager);
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_EQ(recovery.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_mcu::devices["dev3"] = {.healthy = false,
                                                 .inRecovery = false};
    MCUResource absent(bus, "/xyz/openbmc_project/software/mcu3", 23, "dev3",
                       manager);
    static_cast<BaseResource&>(absent).updateHealth();
    EXPECT_EQ(absent.state(), OperationalStatusServer::StateType::Absent);

    absent.wasEnumeratedOnce = true;
    static_cast<BaseResource&>(absent).updateHealth();
    EXPECT_EQ(absent.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    absent.setConnectedToChassis(true);
    absent.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(absent).updateHealth();
    EXPECT_EQ(absent.health(), HealthServer::HealthType::Warning);
}

TEST_F(FWStatusMiscTest, MCURecoveryModeManagerCoversSuccessAndFailures)
{
    auto manager = std::make_shared<mcu_recovery_manager::MCURecoveryManager>();
    nvidia::recovery::MCURecoveryModeManager recoveryManager(
        bus, "chassis0", "/xyz/openbmc_project/inventory/system/chassis/c0",
        manager, "dev0");

    recoveryManager.setRecoveryMode();
    EXPECT_EQ(test::fw_status_fake_mcu::enteredDevices,
              std::vector<std::string>{"dev0"});
    EXPECT_TRUE(test::fw_status_fake_mcu::releaseCalled);

    test::fw_status_fake_mcu::reset();
    test::fw_status_fake_mcu::initResult = false;
    EXPECT_THROW(recoveryManager.setRecoveryMode(), sdbusplus::exception_t);
    EXPECT_TRUE(test::fw_status_fake_mcu::releaseCalled);

    test::fw_status_fake_mcu::reset();
    test::fw_status_fake_mcu::throwOnEnter = true;
    EXPECT_THROW(recoveryManager.setRecoveryMode(), sdbusplus::exception_t);
    EXPECT_TRUE(test::fw_status_fake_mcu::releaseCalled);

    nvidia::recovery::MCURecoveryModeManager nullManager(
        bus, "chassis1", "/xyz/openbmc_project/inventory/system/chassis/c1",
        nullptr, "dev1");
    EXPECT_THROW(nullManager.setRecoveryMode(), sdbusplus::exception_t);
}

TEST_F(FWStatusMiscTest,
       RecoveryModeManagerBaseCoversPassThroughAndExceptionTranslation)
{
    DummyRecoveryModeManager manager(
        bus, "dummy0", "/xyz/openbmc_project/inventory/system/chassis/dummy0");
    EXPECT_EQ(manager.getObjectPath(),
              "/xyz/openbmc_project/inventory/system/chassis/dummy0");

    manager.behavior = DummyRecoveryModeManager::Behavior::success;
    EXPECT_NO_THROW(manager.setRecoveryMode());

    manager.behavior = DummyRecoveryModeManager::Behavior::sdbusFailure;
    EXPECT_THROW(manager.setRecoveryMode(), sdbusplus::exception_t);

    manager.behavior = DummyRecoveryModeManager::Behavior::runtimeFailure;
    EXPECT_THROW(manager.setRecoveryMode(), sdbusplus::exception_t);
}

TEST_F(FWStatusMiscTest, USBRCMRecoveryManagerCoversStatusBranches)
{
    nvidia::recovery::USBRCMRecoveryManager manager(
        bus, "vera0", "/xyz/openbmc_project/inventory/system/chassis/vera0",
        "c2");

    test::fw_status_fake_usb_recovery::pushReply(false, {{"Error", "boom"}});
    EXPECT_FALSE(manager.areDevicesInRecovery());

    test::fw_status_fake_usb_recovery::pushReply(true, {{"Error", "bad"}});
    EXPECT_FALSE(manager.areDevicesInRecovery());

    test::fw_status_fake_usb_recovery::pushReply(true, nlohmann::json::array());
    EXPECT_FALSE(manager.areDevicesInRecovery());

    test::fw_status_fake_usb_recovery::pushReply(
        true,
        nlohmann::json::array(
            {{{"USB Port Path", "1-1"}, {"Recovery Status", "In Recovery"}},
             {{"USB Port Path", "1-2"},
              {"Recovery Status", "Not in Recovery"}}}));
    EXPECT_FALSE(manager.areDevicesInRecovery());

    test::fw_status_fake_usb_recovery::pushReply(
        true, nlohmann::json::array({{{"USB Port Path", "1-1"},
                                      {"Recovery Status", "In Recovery"}}}));
    EXPECT_TRUE(manager.areDevicesInRecovery());
}

TEST_F(FWStatusMiscTest,
       USBRCMRecoveryManagerCoversMissingStatusAndStatuslessWarningBranches)
{
    nvidia::recovery::USBRCMRecoveryManager manager(
        bus, "vera-missing",
        "/xyz/openbmc_project/inventory/system/chassis/vera-missing", "c2");

    test::fw_status_fake_usb_recovery::pushReply(
        true, nlohmann::json::array({{{"USB Port Path", "1-1"}},
                                     {{"USB Port Path", "1-2"},
                                      {"Recovery Status", "In Recovery"}}}));
    EXPECT_FALSE(manager.areDevicesInRecovery());

    test::fw_status_fake_force_recovery::forceResult = nlohmann::json::object();
    test::fw_status_fake_force_recovery::defaultResult =
        nlohmann::json::object();
    test::fw_status_fake_usb_recovery::pushReply(
        true, nlohmann::json::array({{{"USB Port Path", "1-3"},
                                      {"Recovery Status", "In Recovery"}}}));
    EXPECT_NO_THROW(manager.setRecoveryMode());
    EXPECT_EQ(test::fw_status_fake_force_recovery::forceCalls,
              std::vector<std::string>{"c2"});
    EXPECT_EQ(test::fw_status_fake_force_recovery::defaultCalls,
              std::vector<std::string>{"c2"});
}

TEST_F(FWStatusMiscTest, USBRCMRecoveryManagerSetRecoveryModeSuccessAndFailure)
{
    nvidia::recovery::USBRCMRecoveryManager success(
        bus, "vera1", "/xyz/openbmc_project/inventory/system/chassis/vera1",
        "c2");

    test::fw_status_fake_usb_recovery::pushReply(
        true, nlohmann::json::array({{{"USB Port Path", "1-1"},
                                      {"Recovery Status", "In Recovery"}}}));
    success.setRecoveryMode();
    EXPECT_EQ(test::fw_status_fake_force_recovery::forceCalls,
              std::vector<std::string>{"c2"});
    EXPECT_EQ(test::fw_status_fake_force_recovery::defaultCalls,
              std::vector<std::string>{"c2"});

    test::fw_status_fake_force_recovery::reset();
    nvidia::recovery::USBRCMRecoveryManager failure(
        bus, "vera2", "/xyz/openbmc_project/inventory/system/chassis/vera2",
        "c1g2");
    test::fw_status_fake_force_recovery::forceResult = {{"Status", "Failed"},
                                                        {"Error", "gpio"}};
    test::fw_status_fake_force_recovery::defaultResult = {
        {"Status", "Failed"},
        {"Error", "default gpio"},
    };
    test::fw_status_fake_usb_recovery::pushReply(
        true,
        nlohmann::json::array({{{"USB Port Path", "2-1"},
                                {"Recovery Status", "Not in Recovery"}}}));
    test::fw_status_fake_usb_recovery::pushReply(
        true,
        nlohmann::json::array({{{"USB Port Path", "2-1"},
                                {"Recovery Status", "Not in Recovery"}}}));
    test::fw_status_fake_usb_recovery::pushReply(
        true,
        nlohmann::json::array({{{"USB Port Path", "2-1"},
                                {"Recovery Status", "Not in Recovery"}}}));
    EXPECT_THROW(failure.setRecoveryMode(), sdbusplus::exception_t);
    EXPECT_EQ(test::fw_status_fake_force_recovery::forceCalls,
              std::vector<std::string>{"c1g2"});
    EXPECT_EQ(test::fw_status_fake_force_recovery::defaultCalls,
              std::vector<std::string>{"c1g2"});

    nvidia::recovery::USBRCMRecoveryManager emptyCfg(
        bus, "vera3", "/xyz/openbmc_project/inventory/system/chassis/vera3",
        "");
    EXPECT_THROW(emptyCfg.setRecoveryMode(), sdbusplus::exception_t);
}

TEST_F(FWStatusMiscTest,
       USBRcmResourceCoversEmptyErrorMissingStatusAndEnumeratedUnknownBranches)
{
    test::fw_status_fake_usb_recovery::fallbackPayload = nlohmann::json::array(
        {{{"USB Port Path", "5-5"}, {"Recovery Status", "Unknown"}}});

    USBRcmResource resource(bus, "/xyz/openbmc_project/software/fmc-extra", 31,
                            "5-5", "/xyz/openbmc_project/software/fws-extra",
                            nullptr);

    test::fw_status_fake_usb_recovery::pushReply(
        true,
        nlohmann::json::array({{{"USB Port Path", "5-5"}, {"Error", ""}}}));
    EXPECT_EQ(resource.queryUSBRecoveryStatus(), "Unknown");

    resource.wasEnumeratedOnce = true;
    test::fw_status_fake_usb_recovery::pushReply(
        true,
        nlohmann::json::array({{{"USB Port Path", "5-5"}, {"Error", ""}}}));
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    resource.wasEnumeratedOnce = false;
    test::fw_status_fake_usb_recovery::pushReply(
        true,
        nlohmann::json::array({{{"USB Port Path", "5-5"}, {"Error", ""}}}));
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
}

} // namespace
