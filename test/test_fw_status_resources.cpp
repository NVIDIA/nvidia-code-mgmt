#include "dbusutils.hpp"
#include "gpiod.hpp"
#include "i2c_utils.hpp"
#include "recovery_commands.hpp"

#include <fcntl.h>
#include <systemd/sd-bus.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include <chrono>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "gtest/gtest.h"

using ::testing::NiceMock;

namespace
{

std::vector<std::string> openedPaths;
std::vector<long long> nanosleepCallsNs;
std::vector<unsigned int> sleepCalls;
std::vector<unsigned int> usleepCalls;

bool fakeOpenFail = false;
int fakeOpenErrno = ENOENT;

class FWStatusResourceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        openedPaths.clear();
        nanosleepCallsNs.clear();
        sleepCalls.clear();
        usleepCalls.clear();
        fakeOpenFail = false;
        fakeOpenErrno = ENOENT;
        test::fw_status_fake_dbus::reset();
        test::fw_status_fake_gpio::reset();
        test::fw_status_fake_i2c::reset();
        test::fw_status_fake_ocp::reset();
    }

    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
};

} // namespace

extern "C" int __real_open(const char* path, int flags, ...);
extern "C" int __real_open64(const char* path, int flags, ...);

namespace
{

int wrapOpenImpl(const char* path, int flags, bool useOpen64, va_list args)
{
    if (std::strncmp(path, "/dev/i2c-", 9) != 0)
    {
        const int mode = ((flags & O_CREAT) != 0) ? va_arg(args, int) : 0;
        if ((flags & O_CREAT) != 0)
        {
            return useOpen64 ? __real_open64(path, flags, mode)
                             : __real_open(path, flags, mode);
        }
        return useOpen64 ? __real_open64(path, flags)
                         : __real_open(path, flags);
    }

    openedPaths.emplace_back(path);
    if (fakeOpenFail)
    {
        errno = fakeOpenErrno;
        return -1;
    }

    return useOpen64 ? __real_open64("/dev/null", O_RDONLY)
                     : __real_open("/dev/null", O_RDONLY);
}

} // namespace

extern "C" int __wrap_open(const char* path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    const int result = wrapOpenImpl(path, flags, false, args);
    va_end(args);
    return result;
}

extern "C" int __wrap_open64(const char* path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    const int result = wrapOpenImpl(path, flags, true, args);
    va_end(args);
    return result;
}

extern "C" unsigned int __wrap_sleep(unsigned int seconds)
{
    sleepCalls.push_back(seconds);
    return 0;
}

extern "C" int __wrap_usleep(useconds_t usec)
{
    usleepCalls.push_back(static_cast<unsigned int>(usec));
    return 0;
}

extern "C" int __wrap_nanosleep(const struct timespec* req, struct timespec*)
{
    if (req != nullptr)
    {
        nanosleepCallsNs.push_back(
            static_cast<long long>(req->tv_sec) * 1000000000LL + req->tv_nsec);
    }
    return 0;
}

extern "C" int __wrap_clock_nanosleep(clockid_t, int,
                                      const struct timespec* req,
                                      struct timespec*)
{
    if (req != nullptr)
    {
        nanosleepCallsNs.push_back(
            static_cast<long long>(req->tv_sec) * 1000000000LL + req->tv_nsec);
    }
    return 0;
}

#define private public
#define protected public
#pragma GCC push_options
#pragma GCC optimize("no-inline")
#include "../fw-status/connectx_resource.hpp"
#include "../fw-status/gpu_resource.hpp"
#include "../fw-status/mcu_resource.hpp"
#include "../fw-status/nvlinkmgmt_nic_resource.hpp"
#include "../fw-status/nvswitch_resource.hpp"
#pragma GCC pop_options
#undef protected
#undef private

namespace
{

class CountingMCTPResource : public MCTPDiscoveryResource
{
  public:
    CountingMCTPResource(sdbusplus::bus_t& bus, const std::string& objPath,
                         uint8_t eid) : MCTPDiscoveryResource(bus, objPath, eid)
    {}

    int updateCalls = 0;

    void updateHealth() override
    {
        ++updateCalls;
    }
};

sdbusplus::message::message makeInterfacesAddedMessage(
    sdbusplus::bus_t& bus, const std::string& path,
    const nvidia::software::updater::InterfaceMap& interfaces)
{
    auto msg =
        bus.new_signal("/au/com/codeconstruct/mctp1",
                       "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
    msg.append(sdbusplus::object_path(path), interfaces);
    return msg;
}

sdbusplus::message::message makeInterfacesAddedMessage(sdbusplus::bus_t& bus,
                                                       const std::string& path,
                                                       uint8_t eid)
{
    nvidia::software::updater::InterfaceMap interfaces{
        {mctpEndpointIntfName, {{"EID", eid}}}};
    return makeInterfacesAddedMessage(bus, path, interfaces);
}

sdbusplus::message::message
    makeInterfacesRemovedMessage(sdbusplus::bus_t& bus, const std::string& path)
{
    auto msg = bus.new_signal("/au/com/codeconstruct/mctp1",
                              "org.freedesktop.DBus.ObjectManager",
                              "InterfacesRemoved");
    msg.append(sdbusplus::object_path(path),
               std::vector<std::string>{mctpEndpointIntfName});
    return msg;
}

sdbusplus::message::message
    makeMalformedObjectManagerMessage(sdbusplus::bus_t& bus,
                                      const std::string& member)
{
    auto msg =
        bus.new_signal("/au/com/codeconstruct/mctp1",
                       "org.freedesktop.DBus.ObjectManager", member.c_str());
    msg.append(std::string("bad-payload"));
    return msg;
}

void drainBus(sdbusplus::bus_t& bus, int maxIterations = 32)
{
    for (int iteration = 0; iteration < maxIterations; ++iteration)
    {
        if (!bus.process_discard())
        {
            return;
        }
    }
}

template <typename Predicate>
void pumpBusUntil(sdbusplus::bus_t& bus, Predicate&& predicate,
                  int maxAttempts = 10)
{
    for (int attempt = 0; attempt < maxAttempts && !predicate(); ++attempt)
    {
        if (bus.process_discard())
        {
            continue;
        }
        bus.wait(std::chrono::milliseconds(10));
    }
}

void sendObjectManagerSignal(sdbusplus::message::message& msg)
{
    msg.signal_send();
}

int dispatchMatchCallback(const std::unique_ptr<sdbusplus::bus::match_t>& match,
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

template <typename Resource>
void exerciseSmaEndpointCallbacks(sdbusplus::bus_t& receiver,
                                  sdbusplus::bus_t& sender, Resource& resource,
                                  uint8_t smaEid)
{
    (void)receiver;
    const std::string path =
        std::string(mctpObjPathPrefix) + std::to_string(smaEid);

    auto add = makeInterfacesAddedMessage(sender, path, smaEid);
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, add), 0);
    EXPECT_EQ(resource.smaMctpObjectPath, path);

    auto badAdd = makeMalformedObjectManagerMessage(sender, "InterfacesAdded");
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, badAdd), 0);

    auto remove = makeInterfacesRemovedMessage(sender, path);
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointRemovedMatch, remove),
              0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto badRemove =
        makeMalformedObjectManagerMessage(sender, "InterfacesRemoved");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, badRemove), 0);
}

template <typename Resource>
void exerciseSmaFalseBranchCallbacks(Resource& resource,
                                     sdbusplus::bus_t& sender, uint8_t smaEid)
{
    const std::string path =
        std::string(mctpObjPathPrefix) + std::to_string(smaEid);

    resource.monitorSMAEndpoint();

    auto missingIntfAdd = makeInterfacesAddedMessage(
        sender, path, nvidia::software::updater::InterfaceMap{});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingIntfAdd),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto wrongTypeAdd = makeInterfacesAddedMessage(
        sender, path,
        nvidia::software::updater::InterfaceMap{
            {mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, wrongTypeAdd), 0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto wrongValueAdd = makeInterfacesAddedMessage(
        sender, path,
        nvidia::software::updater::InterfaceMap{
            {mctpEndpointIntfName,
             {{"EID", static_cast<uint8_t>(smaEid - 1)}}}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, wrongValueAdd),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto missingEidAdd = makeInterfacesAddedMessage(
        sender, path,
        nvidia::software::updater::InterfaceMap{{mctpEndpointIntfName, {}}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingEidAdd),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto wrongRemove = makeInterfacesRemovedMessage(sender, path + "-other");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, wrongRemove),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());
}

} // namespace

TEST_F(FWStatusResourceTest, ConnectXCoversSuccessRecoveryAndOfflineBranches)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    test::fw_status_fake_gpio::lines["reset"] = {};
    test::fw_status_fake_gpio::lines["fnp"] = {};

    ConnectXResource resource(
        bus, "/xyz/openbmc_project/software/connectx0",
        "/xyz/openbmc_project/state/chassis/connectx0",
        "/xyz/openbmc_project/inventory/system/chassis/ch0", 4, 0x50, 10, 11,
        "reset", "fnp");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);
    EXPECT_EQ(resource.getSMAMCTPObjectPath(), "");
    EXPECT_EQ(openedPaths, std::vector<std::string>{"/dev/i2c-4"});

    const auto result = resource.setForceRecoveryMode();
    EXPECT_TRUE(result.first);
    EXPECT_EQ(test::fw_status_fake_gpio::lines["reset"].requestCount, 1);
    EXPECT_EQ(test::fw_status_fake_gpio::lines["fnp"].requestCount, 1);
    EXPECT_EQ(test::fw_status_fake_gpio::lines["fnp"].setValues,
              std::vector<int>{0});
    EXPECT_EQ(test::fw_status_fake_gpio::lines["reset"].setValues,
              std::vector<int>({0, 1}));
    EXPECT_EQ(usleepCalls, std::vector<unsigned int>({500000, 500000}));
    EXPECT_EQ(sleepCalls, std::vector<unsigned int>({3}));

    fakeOpenFail = true;
    resource.mctpObjectPath.clear();
    resource.wasEnumeratedOnce = true;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    ASSERT_NE(resource.recoveryModeInterface, nullptr);
    EXPECT_NO_THROW(resource.recoveryModeInterface->setRecoveryMode());
}

TEST_F(FWStatusResourceTest, ConnectXEnumeratedAndGpioFailureBranches)
{
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/12",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(12));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/90",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(90));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x00}});
    test::fw_status_fake_gpio::lines["badreset"] =
        test::fw_status_fake_gpio::LineState{};
    test::fw_status_fake_gpio::lines["badreset"].present = false;

    ConnectXResource enumerated(bus, "/xyz/openbmc_project/software/connectx1",
                                "/xyz/openbmc_project/state/chassis/connectx1",
                                "", 5, 0x51, 12, 90, "badreset", "badfnp");
    static_cast<BaseResource&>(enumerated).updateHealth();
    EXPECT_EQ(enumerated.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(enumerated.state(), OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(enumerated.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/90");
    EXPECT_FALSE(enumerated.initGpioLines());
    EXPECT_FALSE(enumerated.setForceRecoveryMode().first);
}

TEST_F(FWStatusResourceTest, ConnectXRecoveryInterfaceThrowsOnFailure)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    test::fw_status_fake_gpio::lines["reset_missing"].present = false;

    ConnectXResource resource(
        bus, "/xyz/openbmc_project/software/connectx-fail",
        "/xyz/openbmc_project/state/chassis/connectx-fail",
        "/xyz/openbmc_project/inventory/system/chassis/ch-fail", 4, 0x50, 30,
        31, "reset_missing", "fnp_missing");

    ASSERT_NE(resource.recoveryModeInterface, nullptr);
    EXPECT_THROW(resource.recoveryModeInterface->setRecoveryMode(),
                 sdbusplus::exception_t);
}

TEST_F(FWStatusResourceTest, ConnectXDirectHelpersCoverI2CAndGpioBranches)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    ConnectXResource resource(
        bus, "/xyz/openbmc_project/software/connectx-direct",
        "/xyz/openbmc_project/state/chassis/connectx-direct", "", 12, 0x58, 41,
        91, "", "");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_FALSE(resource.setForceRecoveryMode().first);
    EXPECT_FALSE(resource.initGpioLines());

    fakeOpenFail = true;
    auto [openOk, openData, openErr] = resource.getDeviceStatus();
    EXPECT_FALSE(openOk);
    EXPECT_TRUE(openData.empty());
    EXPECT_EQ(openErr, "Failed to open device.");
    fakeOpenFail = false;

    test::fw_status_fake_i2c::pushReply({.success = false});
    auto [readOk, readData, readErr] = resource.getDeviceStatus();
    EXPECT_FALSE(readOk);
    EXPECT_TRUE(readData.empty());
    EXPECT_EQ(readErr, "Failed to get device status");

    test::fw_status_fake_i2c::pushReply(
        {.throwException = true, .error = "connectx i2c boom"});
    auto [throwOk, throwData, throwErr] = resource.getDeviceStatus();
    EXPECT_FALSE(throwOk);
    EXPECT_TRUE(throwData.empty());
    EXPECT_NE(throwErr.find("connectx i2c boom"), std::string::npos);

    resource.resetGpioName = "cx-missing-reset";
    resource.flashNotPresentGpioName = "cx-fnp";
    test::fw_status_fake_gpio::lines["cx-missing-reset"].present = false;
    test::fw_status_fake_gpio::lines["cx-fnp"] = {};
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "cx-reset";
    resource.flashNotPresentGpioName = "cx-missing-fnp";
    test::fw_status_fake_gpio::lines["cx-reset"] = {};
    test::fw_status_fake_gpio::lines["cx-missing-fnp"].present = false;
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "cx-throw-reset";
    resource.flashNotPresentGpioName = "cx-throw-fnp";
    test::fw_status_fake_gpio::lines["cx-throw-reset"] = {};
    test::fw_status_fake_gpio::lines["cx-throw-reset"].throwOnRequest = true;
    test::fw_status_fake_gpio::lines["cx-throw-fnp"] = {};
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "cx-release-reset";
    resource.flashNotPresentGpioName = "cx-release-fnp";
    test::fw_status_fake_gpio::lines["cx-release-reset"] = {};
    test::fw_status_fake_gpio::lines["cx-release-fnp"] = {};
    ASSERT_TRUE(resource.initGpioLines());
    test::fw_status_fake_gpio::lines["cx-release-fnp"].throwOnRelease = true;
    EXPECT_NO_THROW(resource.releaseGpioLines());

    resource.resetLine = {};
    resource.fnpLine = {};
    EXPECT_NO_THROW(resource.enterRecoveryMode());

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/91",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(91));
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/91");

    auto signalBus = sdbusplus::bus::new_default();
    auto ignoredAdd = makeInterfacesAddedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/5", 5);
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, ignoredAdd),
              0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto matchingAdd = makeInterfacesAddedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/91", 91);
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x10, 0x00, 0x00, 0x00}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, matchingAdd), 0);
    EXPECT_EQ(resource.smaMctpObjectPath,
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/91");

    auto badAdd =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesAdded");
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, badAdd), 0);

    auto badRemove =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesRemoved");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, badRemove), 0);

    auto wrongRemove = makeInterfacesRemovedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/1");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, wrongRemove),
        0);
    EXPECT_FALSE(resource.smaMctpObjectPath.empty());

    auto matchingRemove = makeInterfacesRemovedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/91");
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, matchingRemove),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());
}

TEST_F(FWStatusResourceTest,
       ConnectXAdditionalBranchesCoverStateAndRecoveryFailures)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    ConnectXResource resource(
        bus, "/xyz/openbmc_project/software/connectx-extra",
        "/xyz/openbmc_project/state/chassis/connectx-extra",
        "/xyz/openbmc_project/inventory/system/chassis/cx-extra", 15, 0x5B, 51,
        94, "cx-extra-reset", "cx-extra-fnp");

    resource.resetGpioName = "cx-extra-request-reset";
    resource.flashNotPresentGpioName = "cx-extra-request-fnp";
    test::fw_status_fake_gpio::lines["cx-extra-request-reset"] = {};
    test::fw_status_fake_gpio::lines["cx-extra-request-fnp"] = {};
    test::fw_status_fake_gpio::lines["cx-extra-request-fnp"].throwOnRequest =
        true;
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "cx-extra-set-reset";
    resource.flashNotPresentGpioName = "cx-extra-set-fnp";
    test::fw_status_fake_gpio::lines["cx-extra-set-reset"] = {};
    test::fw_status_fake_gpio::lines["cx-extra-set-fnp"] = {};
    test::fw_status_fake_gpio::lines["cx-extra-set-fnp"].throwOnSet = true;
    const auto recoveryResult = resource.setForceRecoveryMode();
    EXPECT_FALSE(recoveryResult.first);
    EXPECT_NE(recoveryResult.second.find("Failed to set GPIO"),
              std::string::npos);
    ASSERT_NE(resource.recoveryModeInterface, nullptr);
    EXPECT_THROW(resource.recoveryModeInterface->setRecoveryMode(),
                 sdbusplus::exception_t);

    fakeOpenFail = true;
    resource.wasEnumeratedOnce = false;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
    fakeOpenFail = false;

    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST_F(FWStatusResourceTest,
       ConnectXExplicitlyCoversStateTransitionsAndSmaLookup)
{
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/"
         "networks/1/endpoints/00-ignored"] = {
            {"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/55",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(55));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/98",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(98));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});

    ConnectXResource resource(
        bus, "/xyz/openbmc_project/software/connectx-states",
        "/xyz/openbmc_project/state/chassis/connectx-states", "", 19, 0x5F, 55,
        98, "", "");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/98");

    auto signalBus = sdbusplus::bus::new_default();
    auto missingIntfAdd = signalBus.new_signal(
        mctpObjMgrPath.data(), "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded");
    missingIntfAdd.append(
        sdbusplus::object_path(
            "/au/com/codeconstruct/mctp1/networks/1/endpoints/99"),
        nvidia::software::updater::InterfaceMap{});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingIntfAdd),
        0);

    fakeOpenFail = true;
    resource.mctpObjectPath.clear();
    resource.wasEnumeratedOnce = true;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    resource.wasEnumeratedOnce = false;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
    fakeOpenFail = false;

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x10, 0x00, 0x00, 0x00}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);
}

TEST_F(FWStatusResourceTest, ConnectXCoversSmaFalseBranchesAndMonitorReuse)
{
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/ignored"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/badtype"] =
        {{mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/96"] = {
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(96)}}}};
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});

    ConnectXResource resource(
        bus, "/xyz/openbmc_project/software/connectx-sma-false",
        "/xyz/openbmc_project/state/chassis/connectx-sma-false", "", 22, 0x62,
        57, 97, "", "");
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto signalBus = sdbusplus::bus::new_default();
    exerciseSmaFalseBranchCallbacks(resource, signalBus, 97);
}

TEST_F(FWStatusResourceTest, NVLinkMgmtNicCoversRecoveryAndGpioBranches)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0xAA, 0x00, 0x00, 0x01}});
    test::fw_status_fake_gpio::lines["nicreset"] = {};
    test::fw_status_fake_gpio::lines["nicfnp"] = {};

    NVLinkMgmtNicResource resource(
        bus, "/xyz/openbmc_project/software/nic0",
        "/xyz/openbmc_project/state/chassis/nic0",
        "/xyz/openbmc_project/inventory/system/chassis/nic0", 6, 0x61, 13, 14,
        "nicreset", "nicfnp");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_TRUE(resource.setForceRecoveryMode().first);

    fakeOpenFail = true;
    resource.wasEnumeratedOnce = true;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    ASSERT_NE(resource.recoveryModeInterface, nullptr);
    EXPECT_NO_THROW(resource.recoveryModeInterface->setRecoveryMode());
}

TEST_F(FWStatusResourceTest, NVLinkMgmtNicDirectHelpersCoverI2CAndGpioBranches)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    NVLinkMgmtNicResource resource(
        bus, "/xyz/openbmc_project/software/nic-direct",
        "/xyz/openbmc_project/state/chassis/nic-direct", "", 13, 0x59, 42, 92,
        "", "");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_FALSE(resource.setForceRecoveryMode().first);
    EXPECT_FALSE(resource.initGpioLines());

    fakeOpenFail = true;
    auto [openOk, openData, openErr] = resource.getDeviceStatus();
    EXPECT_FALSE(openOk);
    EXPECT_TRUE(openData.empty());
    EXPECT_EQ(openErr, "Failed to open device.");
    fakeOpenFail = false;

    test::fw_status_fake_i2c::pushReply({.success = false});
    auto [readOk, readData, readErr] = resource.getDeviceStatus();
    EXPECT_FALSE(readOk);
    EXPECT_TRUE(readData.empty());
    EXPECT_EQ(readErr, "Failed to get device status");

    test::fw_status_fake_i2c::pushReply(
        {.throwException = true, .error = "nic i2c boom"});
    auto [throwOk, throwData, throwErr] = resource.getDeviceStatus();
    EXPECT_FALSE(throwOk);
    EXPECT_TRUE(throwData.empty());
    EXPECT_NE(throwErr.find("nic i2c boom"), std::string::npos);

    resource.resetGpioName = "nic-missing-reset";
    resource.flashNotPresentGpioName = "nic-fnp";
    test::fw_status_fake_gpio::lines["nic-missing-reset"].present = false;
    test::fw_status_fake_gpio::lines["nic-fnp"] = {};
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "nic-reset";
    resource.flashNotPresentGpioName = "nic-missing-fnp";
    test::fw_status_fake_gpio::lines["nic-reset"] = {};
    test::fw_status_fake_gpio::lines["nic-missing-fnp"].present = false;
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "nic-throw-reset";
    resource.flashNotPresentGpioName = "nic-throw-fnp";
    test::fw_status_fake_gpio::lines["nic-throw-reset"] = {};
    test::fw_status_fake_gpio::lines["nic-throw-reset"].throwOnRequest = true;
    test::fw_status_fake_gpio::lines["nic-throw-fnp"] = {};
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "nic-release-reset";
    resource.flashNotPresentGpioName = "nic-release-fnp";
    test::fw_status_fake_gpio::lines["nic-release-reset"] = {};
    test::fw_status_fake_gpio::lines["nic-release-fnp"] = {};
    ASSERT_TRUE(resource.initGpioLines());
    test::fw_status_fake_gpio::lines["nic-release-fnp"].throwOnRelease = true;
    EXPECT_NO_THROW(resource.releaseGpioLines());

    resource.resetLine = {};
    resource.fnpLine = {};
    EXPECT_THROW(resource.enterRecoveryMode(), std::runtime_error);

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/92",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(92));
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/92");

    auto signalBus = sdbusplus::bus::new_default();
    auto matchingAdd = makeInterfacesAddedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/92", 92);
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, matchingAdd), 0);
    EXPECT_FALSE(resource.smaMctpObjectPath.empty());

    auto badAdd =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesAdded");
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, badAdd), 0);

    auto matchingRemove = makeInterfacesRemovedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/92");
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x00}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, matchingRemove),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());
}

TEST_F(FWStatusResourceTest,
       NVLinkMgmtNicAdditionalBranchesCoverStateAndRecoveryFailures)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    NVLinkMgmtNicResource resource(
        bus, "/xyz/openbmc_project/software/nic-extra",
        "/xyz/openbmc_project/state/chassis/nic-extra",
        "/xyz/openbmc_project/inventory/system/chassis/nic-extra", 16, 0x5C, 52,
        95, "nic-extra-reset", "nic-extra-fnp");

    resource.resetGpioName = "nic-extra-request-reset";
    resource.flashNotPresentGpioName = "nic-extra-request-fnp";
    test::fw_status_fake_gpio::lines["nic-extra-request-reset"] = {};
    test::fw_status_fake_gpio::lines["nic-extra-request-fnp"] = {};
    test::fw_status_fake_gpio::lines["nic-extra-request-fnp"].throwOnRequest =
        true;
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "nic-extra-set-reset";
    resource.flashNotPresentGpioName = "nic-extra-set-fnp";
    test::fw_status_fake_gpio::lines["nic-extra-set-reset"] = {};
    test::fw_status_fake_gpio::lines["nic-extra-set-fnp"] = {};
    test::fw_status_fake_gpio::lines["nic-extra-set-fnp"].throwOnSet = true;
    const auto recoveryResult = resource.setForceRecoveryMode();
    EXPECT_FALSE(recoveryResult.first);
    EXPECT_NE(recoveryResult.second.find("Failed to set GPIO"),
              std::string::npos);
    ASSERT_NE(resource.recoveryModeInterface, nullptr);
    EXPECT_THROW(resource.recoveryModeInterface->setRecoveryMode(),
                 sdbusplus::exception_t);

    fakeOpenFail = true;
    resource.wasEnumeratedOnce = false;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
    fakeOpenFail = false;

    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST_F(FWStatusResourceTest,
       NVLinkMgmtNicExplicitlyCoversStateTransitionsAndSmaLookup)
{
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/"
         "networks/1/endpoints/00-ignored"] = {
            {"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/56",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(56));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/99",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(99));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});

    NVLinkMgmtNicResource resource(
        bus, "/xyz/openbmc_project/software/nic-states",
        "/xyz/openbmc_project/state/chassis/nic-states", "", 20, 0x60, 56, 99,
        "", "");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/99");

    auto signalBus = sdbusplus::bus::new_default();
    auto missingIntfAdd = signalBus.new_signal(
        mctpObjMgrPath.data(), "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded");
    missingIntfAdd.append(
        sdbusplus::object_path(
            "/au/com/codeconstruct/mctp1/networks/1/endpoints/100"),
        nvidia::software::updater::InterfaceMap{});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingIntfAdd),
        0);

    fakeOpenFail = true;
    resource.mctpObjectPath.clear();
    resource.wasEnumeratedOnce = true;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    resource.wasEnumeratedOnce = false;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
    fakeOpenFail = false;

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x10, 0x00, 0x00, 0x00}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);
}

TEST_F(FWStatusResourceTest, NVLinkMgmtNicCoversSmaFalseBranchesAndMonitorReuse)
{
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/ignored"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/badtype"] =
        {{mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/95"] = {
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(95)}}}};
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});

    NVLinkMgmtNicResource resource(
        bus, "/xyz/openbmc_project/software/nic-sma-false",
        "/xyz/openbmc_project/state/chassis/nic-sma-false", "", 23, 0x63, 58,
        96, "", "");
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto signalBus = sdbusplus::bus::new_default();
    exerciseSmaFalseBranchCallbacks(resource, signalBus, 96);
}

TEST_F(FWStatusResourceTest, NVSwitchCoversDegradedRecoveryAndGpioFailures)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    test::fw_status_fake_gpio::lines["swreset"] = {};
    test::fw_status_fake_gpio::lines["swfnp"] =
        test::fw_status_fake_gpio::LineState{};
    test::fw_status_fake_gpio::lines["swfnp"].throwOnSet = true;

    NVSwitchResource resource(
        bus, "/xyz/openbmc_project/software/nvswitch0",
        "/xyz/openbmc_project/state/chassis/nvswitch0",
        "/xyz/openbmc_project/inventory/system/chassis/switch0", 7, 0x62, 15,
        16, "swreset", "swfnp");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);
    const auto result = resource.setForceRecoveryMode();
    EXPECT_FALSE(result.first);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x10, 0x10, 0x10, 0x10}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    test::fw_status_fake_gpio::lines["swreset_ok"] = {};
    test::fw_status_fake_gpio::lines["swfnp_ok"] = {};
    NVSwitchResource callback(
        bus, "/xyz/openbmc_project/software/nvswitch-callback",
        "/xyz/openbmc_project/state/chassis/nvswitch-callback",
        "/xyz/openbmc_project/inventory/system/chassis/switch-callback", 8,
        0x63, 25, 26, "swreset_ok", "swfnp_ok");
    ASSERT_NE(callback.recoveryModeInterface, nullptr);
    EXPECT_NO_THROW(callback.recoveryModeInterface->setRecoveryMode());
}

TEST_F(FWStatusResourceTest, NVSwitchDirectHelpersCoverI2CAndGpioBranches)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    NVSwitchResource resource(
        bus, "/xyz/openbmc_project/software/nvswitch-direct",
        "/xyz/openbmc_project/state/chassis/nvswitch-direct", "", 14, 0x5A, 43,
        93);
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_FALSE(resource.setForceRecoveryMode().first);
    EXPECT_FALSE(resource.initGpioLines());

    fakeOpenFail = true;
    auto [openOk, openData, openErr] = resource.getDeviceStatus();
    EXPECT_FALSE(openOk);
    EXPECT_TRUE(openData.empty());
    EXPECT_EQ(openErr, "Failed to open device.");
    fakeOpenFail = false;

    test::fw_status_fake_i2c::pushReply({.success = false});
    auto [readOk, readData, readErr] = resource.getDeviceStatus();
    EXPECT_FALSE(readOk);
    EXPECT_TRUE(readData.empty());
    EXPECT_EQ(readErr, "Failed to get device status");

    test::fw_status_fake_i2c::pushReply(
        {.throwException = true, .error = "switch i2c boom"});
    auto [throwOk, throwData, throwErr] = resource.getDeviceStatus();
    EXPECT_FALSE(throwOk);
    EXPECT_TRUE(throwData.empty());
    EXPECT_NE(throwErr.find("switch i2c boom"), std::string::npos);

    resource.resetGpioName = "sw-missing-reset";
    resource.flashNotPresentGpioName = "sw-fnp";
    test::fw_status_fake_gpio::lines["sw-missing-reset"].present = false;
    test::fw_status_fake_gpio::lines["sw-fnp"] = {};
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "sw-reset";
    resource.flashNotPresentGpioName = "sw-missing-fnp";
    test::fw_status_fake_gpio::lines["sw-reset"] = {};
    test::fw_status_fake_gpio::lines["sw-missing-fnp"].present = false;
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "sw-throw-reset";
    resource.flashNotPresentGpioName = "sw-throw-fnp";
    test::fw_status_fake_gpio::lines["sw-throw-reset"] = {};
    test::fw_status_fake_gpio::lines["sw-throw-reset"].throwOnRequest = true;
    test::fw_status_fake_gpio::lines["sw-throw-fnp"] = {};
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "sw-release-reset";
    resource.flashNotPresentGpioName = "sw-release-fnp";
    test::fw_status_fake_gpio::lines["sw-release-reset"] = {};
    test::fw_status_fake_gpio::lines["sw-release-fnp"] = {};
    ASSERT_TRUE(resource.initGpioLines());
    test::fw_status_fake_gpio::lines["sw-release-fnp"].throwOnRelease = true;
    EXPECT_NO_THROW(resource.releaseGpioLines());

    resource.resetLine = {};
    resource.fnpLine = {};
    EXPECT_THROW(resource.enterRecoveryMode(), std::runtime_error);

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/93",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(93));
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/93");

    auto signalBus = sdbusplus::bus::new_default();
    auto missingIntfAdd = signalBus.new_signal(
        mctpObjMgrPath.data(), "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded");
    missingIntfAdd.append(
        sdbusplus::object_path(
            "/au/com/codeconstruct/mctp1/networks/1/endpoints/2"),
        nvidia::software::updater::InterfaceMap{});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingIntfAdd),
        0);

    auto matchingAdd = makeInterfacesAddedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/93", 93);
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x10, 0x10, 0x10, 0x10}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, matchingAdd), 0);
    EXPECT_FALSE(resource.smaMctpObjectPath.empty());

    auto badRemove =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesRemoved");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, badRemove), 0);

    auto matchingRemove = makeInterfacesRemovedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/93");
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, matchingRemove),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());
}

TEST_F(FWStatusResourceTest,
       ResourceGpioReleaseAndSingleEmptyNameBranchesAreCovered)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    ConnectXResource connectx(
        bus, "/xyz/openbmc_project/software/connectx-release",
        "/xyz/openbmc_project/state/chassis/connectx-release", "", 24, 0x64, 61,
        101, "", "");
    connectx.resetGpioName = "cx-release-reset-ok";
    connectx.flashNotPresentGpioName = "cx-release-fnp-ok";
    test::fw_status_fake_gpio::lines["cx-release-reset-ok"] = {};
    test::fw_status_fake_gpio::lines["cx-release-fnp-ok"] = {};
    ASSERT_TRUE(connectx.initGpioLines());
    connectx.releaseGpioLines();
    EXPECT_FALSE(static_cast<bool>(connectx.resetLine));
    EXPECT_FALSE(static_cast<bool>(connectx.fnpLine));
    EXPECT_NO_THROW(connectx.releaseGpioLines());
    connectx.resetGpioName.clear();
    connectx.flashNotPresentGpioName = "cx-only-fnp";
    EXPECT_FALSE(connectx.initGpioLines());
    connectx.resetGpioName = "cx-only-reset";
    connectx.flashNotPresentGpioName.clear();
    EXPECT_FALSE(connectx.initGpioLines());

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    NVLinkMgmtNicResource nic(bus, "/xyz/openbmc_project/software/nic-release",
                              "/xyz/openbmc_project/state/chassis/nic-release",
                              "", 25, 0x65, 62, 102, "", "");
    nic.resetGpioName = "nic-release-reset-ok";
    nic.flashNotPresentGpioName = "nic-release-fnp-ok";
    test::fw_status_fake_gpio::lines["nic-release-reset-ok"] = {};
    test::fw_status_fake_gpio::lines["nic-release-fnp-ok"] = {};
    ASSERT_TRUE(nic.initGpioLines());
    nic.releaseGpioLines();
    EXPECT_FALSE(static_cast<bool>(nic.resetLine));
    EXPECT_FALSE(static_cast<bool>(nic.fnpLine));
    EXPECT_NO_THROW(nic.releaseGpioLines());
    nic.resetGpioName.clear();
    nic.flashNotPresentGpioName = "nic-only-fnp";
    EXPECT_FALSE(nic.initGpioLines());
    nic.resetGpioName = "nic-only-reset";
    nic.flashNotPresentGpioName.clear();
    EXPECT_FALSE(nic.initGpioLines());

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    NVSwitchResource nvswitch(
        bus, "/xyz/openbmc_project/software/nvswitch-release",
        "/xyz/openbmc_project/state/chassis/nvswitch-release", "", 26, 0x66, 63,
        103);
    nvswitch.resetGpioName = "sw-release-reset-ok";
    nvswitch.flashNotPresentGpioName = "sw-release-fnp-ok";
    test::fw_status_fake_gpio::lines["sw-release-reset-ok"] = {};
    test::fw_status_fake_gpio::lines["sw-release-fnp-ok"] = {};
    ASSERT_TRUE(nvswitch.initGpioLines());
    nvswitch.releaseGpioLines();
    EXPECT_FALSE(static_cast<bool>(nvswitch.resetLine));
    EXPECT_FALSE(static_cast<bool>(nvswitch.fnpLine));
    EXPECT_NO_THROW(nvswitch.releaseGpioLines());
    nvswitch.resetGpioName.clear();
    nvswitch.flashNotPresentGpioName = "sw-only-fnp";
    EXPECT_FALSE(nvswitch.initGpioLines());
    nvswitch.resetGpioName = "sw-only-reset";
    nvswitch.flashNotPresentGpioName.clear();
    EXPECT_FALSE(nvswitch.initGpioLines());
}

TEST_F(FWStatusResourceTest,
       ResourceRecoveryGuardBranchesCoverSingleEmptyNamesAndPartialLines)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    ConnectXResource connectx(
        bus, "/xyz/openbmc_project/software/connectx-guards",
        "/xyz/openbmc_project/state/chassis/connectx-guards", "", 33, 0x6D, 70,
        110, "", "");

    connectx.resetGpioName = "cx-guard-reset-name";
    connectx.flashNotPresentGpioName.clear();
    EXPECT_FALSE(connectx.setForceRecoveryMode().first);
    connectx.resetGpioName.clear();
    connectx.flashNotPresentGpioName = "cx-guard-fnp-name";
    EXPECT_FALSE(connectx.setForceRecoveryMode().first);

    test::fw_status_fake_gpio::lines["cx-partial-reset"] = {};
    connectx.resetLine = gpiod::line("cx-partial-reset", true);
    connectx.fnpLine = {};
    EXPECT_NO_THROW(connectx.enterRecoveryMode());
    EXPECT_NO_THROW(connectx.releaseGpioLines());
    EXPECT_EQ(test::fw_status_fake_gpio::lines["cx-partial-reset"].releaseCount,
              1);

    test::fw_status_fake_gpio::lines["cx-partial-fnp"] = {};
    connectx.resetLine = {};
    connectx.fnpLine = gpiod::line("cx-partial-fnp", true);
    EXPECT_NO_THROW(connectx.enterRecoveryMode());
    EXPECT_NO_THROW(connectx.releaseGpioLines());
    EXPECT_EQ(test::fw_status_fake_gpio::lines["cx-partial-fnp"].releaseCount,
              1);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    NVLinkMgmtNicResource nic(bus, "/xyz/openbmc_project/software/nic-guards",
                              "/xyz/openbmc_project/state/chassis/nic-guards",
                              "", 34, 0x6E, 71, 111, "", "");

    nic.resetGpioName = "nic-guard-reset-name";
    nic.flashNotPresentGpioName.clear();
    EXPECT_FALSE(nic.setForceRecoveryMode().first);
    nic.resetGpioName.clear();
    nic.flashNotPresentGpioName = "nic-guard-fnp-name";
    EXPECT_FALSE(nic.setForceRecoveryMode().first);

    test::fw_status_fake_gpio::lines["nic-partial-reset"] = {};
    nic.resetLine = gpiod::line("nic-partial-reset", true);
    nic.fnpLine = {};
    EXPECT_THROW(nic.enterRecoveryMode(), std::runtime_error);
    EXPECT_NO_THROW(nic.releaseGpioLines());
    EXPECT_EQ(
        test::fw_status_fake_gpio::lines["nic-partial-reset"].releaseCount, 1);

    test::fw_status_fake_gpio::lines["nic-partial-fnp"] = {};
    nic.resetLine = {};
    nic.fnpLine = gpiod::line("nic-partial-fnp", true);
    EXPECT_THROW(nic.enterRecoveryMode(), std::runtime_error);
    EXPECT_NO_THROW(nic.releaseGpioLines());
    EXPECT_EQ(test::fw_status_fake_gpio::lines["nic-partial-fnp"].releaseCount,
              1);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    NVSwitchResource nvswitch(
        bus, "/xyz/openbmc_project/software/nvswitch-guards",
        "/xyz/openbmc_project/state/chassis/nvswitch-guards", "", 35, 0x6F, 72,
        112);

    nvswitch.resetGpioName = "sw-guard-reset-name";
    nvswitch.flashNotPresentGpioName.clear();
    EXPECT_FALSE(nvswitch.setForceRecoveryMode().first);
    nvswitch.resetGpioName.clear();
    nvswitch.flashNotPresentGpioName = "sw-guard-fnp-name";
    EXPECT_FALSE(nvswitch.setForceRecoveryMode().first);

    test::fw_status_fake_gpio::lines["sw-partial-reset"] = {};
    nvswitch.resetLine = gpiod::line("sw-partial-reset", true);
    nvswitch.fnpLine = {};
    EXPECT_THROW(nvswitch.enterRecoveryMode(), std::runtime_error);
    EXPECT_NO_THROW(nvswitch.releaseGpioLines());
    EXPECT_EQ(test::fw_status_fake_gpio::lines["sw-partial-reset"].releaseCount,
              1);

    test::fw_status_fake_gpio::lines["sw-partial-fnp"] = {};
    nvswitch.resetLine = {};
    nvswitch.fnpLine = gpiod::line("sw-partial-fnp", true);
    EXPECT_THROW(nvswitch.enterRecoveryMode(), std::runtime_error);
    EXPECT_NO_THROW(nvswitch.releaseGpioLines());
    EXPECT_EQ(test::fw_status_fake_gpio::lines["sw-partial-fnp"].releaseCount,
              1);
}

TEST_F(FWStatusResourceTest,
       ResourceRecoveryExceptionsCoverFirstByteMismatchAndResetFailures)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x21, 0x00, 0x00, 0x19}});
    ConnectXResource connectx(
        bus, "/xyz/openbmc_project/software/connectx-reset-fail",
        "/xyz/openbmc_project/state/chassis/connectx-reset-fail", "", 42, 0x76,
        79, 124, "cx-reset-throw", "cx-fnp-ok");
    static_cast<BaseResource&>(connectx).updateHealth();
    EXPECT_EQ(connectx.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_gpio::lines["cx-reset-throw"] = {};
    test::fw_status_fake_gpio::lines["cx-reset-throw"].throwOnSet = true;
    test::fw_status_fake_gpio::lines["cx-fnp-ok"] = {};
    const auto connectxSetFail = connectx.setForceRecoveryMode();
    EXPECT_FALSE(connectxSetFail.first);
    EXPECT_NE(connectxSetFail.second.find("Failed to set GPIO"),
              std::string::npos);

    connectx.resetGpioName = "cx-release-throw-reset";
    connectx.flashNotPresentGpioName = "cx-release-throw-fnp";
    test::fw_status_fake_gpio::lines["cx-release-throw-reset"] = {};
    test::fw_status_fake_gpio::lines["cx-release-throw-reset"].throwOnRelease =
        true;
    test::fw_status_fake_gpio::lines["cx-release-throw-fnp"] = {};
    ASSERT_TRUE(connectx.initGpioLines());
    EXPECT_NO_THROW(connectx.releaseGpioLines());

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x21, 0x00, 0x00, 0x19}});
    NVLinkMgmtNicResource nic(
        bus, "/xyz/openbmc_project/software/nic-reset-fail",
        "/xyz/openbmc_project/state/chassis/nic-reset-fail", "", 43, 0x77, 80,
        125, "nic-reset-throw", "nic-fnp-ok");
    static_cast<BaseResource&>(nic).updateHealth();
    EXPECT_EQ(nic.state(), OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_gpio::lines["nic-reset-throw"] = {};
    test::fw_status_fake_gpio::lines["nic-reset-throw"].throwOnSet = true;
    test::fw_status_fake_gpio::lines["nic-fnp-ok"] = {};
    const auto nicSetFail = nic.setForceRecoveryMode();
    EXPECT_FALSE(nicSetFail.first);
    EXPECT_NE(nicSetFail.second.find("Failed to set GPIO"), std::string::npos);

    nic.resetGpioName = "nic-release-throw-reset";
    nic.flashNotPresentGpioName = "nic-release-throw-fnp";
    test::fw_status_fake_gpio::lines["nic-release-throw-reset"] = {};
    test::fw_status_fake_gpio::lines["nic-release-throw-reset"].throwOnRelease =
        true;
    test::fw_status_fake_gpio::lines["nic-release-throw-fnp"] = {};
    ASSERT_TRUE(nic.initGpioLines());
    EXPECT_NO_THROW(nic.releaseGpioLines());

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x01, 0x00, 0x00, 0x5e}});
    NVSwitchResource nvswitch(
        bus, "/xyz/openbmc_project/software/nvswitch-reset-fail",
        "/xyz/openbmc_project/state/chassis/nvswitch-reset-fail", "", 44, 0x78,
        81, 126, "sw-reset-throw", "sw-fnp-ok");
    static_cast<BaseResource&>(nvswitch).updateHealth();
    EXPECT_EQ(nvswitch.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_gpio::lines["sw-reset-throw"] = {};
    test::fw_status_fake_gpio::lines["sw-reset-throw"].throwOnSet = true;
    test::fw_status_fake_gpio::lines["sw-fnp-ok"] = {};
    const auto switchSetFail = nvswitch.setForceRecoveryMode();
    EXPECT_FALSE(switchSetFail.first);
    EXPECT_NE(switchSetFail.second.find("Failed to set GPIO"),
              std::string::npos);

    nvswitch.resetGpioName = "sw-release-throw-reset";
    nvswitch.flashNotPresentGpioName = "sw-release-throw-fnp";
    test::fw_status_fake_gpio::lines["sw-release-throw-reset"] = {};
    test::fw_status_fake_gpio::lines["sw-release-throw-reset"].throwOnRelease =
        true;
    test::fw_status_fake_gpio::lines["sw-release-throw-fnp"] = {};
    ASSERT_TRUE(nvswitch.initGpioLines());
    EXPECT_NO_THROW(nvswitch.releaseGpioLines());
}

TEST_F(FWStatusResourceTest,
       ResourceDirectSmaLookupsAndResetReleaseCatchesCoverRemainingBranches)
{
    test::fw_status_fake_dbus::managedObjects.clear();
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/no-intf"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/bad-type"] =
        {{mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/wrong"] = {
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(119)}}}};

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    ConnectXResource connectx(
        bus, "/xyz/openbmc_project/software/connectx-reset-catch",
        "/xyz/openbmc_project/state/chassis/connectx-reset-catch", "", 36, 0x70,
        73, 120, "", "");
    EXPECT_TRUE(connectx.getSMAMCTPObjectPath().empty());
    connectx.resetGpioName = "cx-reset-release-throw";
    connectx.flashNotPresentGpioName = "cx-fnp-release-ok";
    test::fw_status_fake_gpio::lines["cx-reset-release-throw"] = {};
    test::fw_status_fake_gpio::lines["cx-fnp-release-ok"] = {};
    ASSERT_TRUE(connectx.initGpioLines());
    test::fw_status_fake_gpio::lines["cx-reset-release-throw"].throwOnRelease =
        true;
    EXPECT_NO_THROW(connectx.releaseGpioLines());

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    NVLinkMgmtNicResource nic(
        bus, "/xyz/openbmc_project/software/nic-reset-catch",
        "/xyz/openbmc_project/state/chassis/nic-reset-catch", "", 37, 0x71, 74,
        120, "", "");
    EXPECT_TRUE(nic.getSMAMCTPObjectPath().empty());
    nic.resetGpioName = "nic-reset-release-throw";
    nic.flashNotPresentGpioName = "nic-fnp-release-ok";
    test::fw_status_fake_gpio::lines["nic-reset-release-throw"] = {};
    test::fw_status_fake_gpio::lines["nic-fnp-release-ok"] = {};
    ASSERT_TRUE(nic.initGpioLines());
    test::fw_status_fake_gpio::lines["nic-reset-release-throw"].throwOnRelease =
        true;
    EXPECT_NO_THROW(nic.releaseGpioLines());

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    NVSwitchResource nvswitch(
        bus, "/xyz/openbmc_project/software/nvswitch-reset-catch",
        "/xyz/openbmc_project/state/chassis/nvswitch-reset-catch", "", 38, 0x72,
        75, 120);
    EXPECT_TRUE(nvswitch.getSMAMCTPObjectPath().empty());
    nvswitch.resetGpioName = "sw-reset-release-throw";
    nvswitch.flashNotPresentGpioName = "sw-fnp-release-ok";
    test::fw_status_fake_gpio::lines["sw-reset-release-throw"] = {};
    test::fw_status_fake_gpio::lines["sw-fnp-release-ok"] = {};
    ASSERT_TRUE(nvswitch.initGpioLines());
    test::fw_status_fake_gpio::lines["sw-reset-release-throw"].throwOnRelease =
        true;
    EXPECT_NO_THROW(nvswitch.releaseGpioLines());
}

TEST_F(FWStatusResourceTest,
       ResourceReleaseAlsoCoversFlashNotPresentReleaseCatchBranches)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    ConnectXResource connectx(
        bus, "/xyz/openbmc_project/software/connectx-fnp-catch",
        "/xyz/openbmc_project/state/chassis/connectx-fnp-catch", "", 39, 0x73,
        76, 121, "", "");
    connectx.resetGpioName = "cx-fnp-catch-reset";
    connectx.flashNotPresentGpioName = "cx-fnp-catch-fnp";
    test::fw_status_fake_gpio::lines["cx-fnp-catch-reset"] = {};
    test::fw_status_fake_gpio::lines["cx-fnp-catch-fnp"] = {};
    ASSERT_TRUE(connectx.initGpioLines());
    test::fw_status_fake_gpio::lines["cx-fnp-catch-fnp"].throwOnRelease = true;
    EXPECT_NO_THROW(connectx.releaseGpioLines());
    EXPECT_EQ(
        test::fw_status_fake_gpio::lines["cx-fnp-catch-reset"].releaseCount, 1);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    NVLinkMgmtNicResource nic(
        bus, "/xyz/openbmc_project/software/nic-fnp-catch",
        "/xyz/openbmc_project/state/chassis/nic-fnp-catch", "", 40, 0x74, 77,
        122, "", "");
    nic.resetGpioName = "nic-fnp-catch-reset";
    nic.flashNotPresentGpioName = "nic-fnp-catch-fnp";
    test::fw_status_fake_gpio::lines["nic-fnp-catch-reset"] = {};
    test::fw_status_fake_gpio::lines["nic-fnp-catch-fnp"] = {};
    ASSERT_TRUE(nic.initGpioLines());
    test::fw_status_fake_gpio::lines["nic-fnp-catch-fnp"].throwOnRelease = true;
    EXPECT_NO_THROW(nic.releaseGpioLines());
    EXPECT_EQ(
        test::fw_status_fake_gpio::lines["nic-fnp-catch-reset"].releaseCount,
        1);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    NVSwitchResource nvswitch(
        bus, "/xyz/openbmc_project/software/nvswitch-fnp-catch",
        "/xyz/openbmc_project/state/chassis/nvswitch-fnp-catch", "", 41, 0x75,
        78, 123);
    nvswitch.resetGpioName = "sw-fnp-catch-reset";
    nvswitch.flashNotPresentGpioName = "sw-fnp-catch-fnp";
    test::fw_status_fake_gpio::lines["sw-fnp-catch-reset"] = {};
    test::fw_status_fake_gpio::lines["sw-fnp-catch-fnp"] = {};
    ASSERT_TRUE(nvswitch.initGpioLines());
    test::fw_status_fake_gpio::lines["sw-fnp-catch-fnp"].throwOnRelease = true;
    EXPECT_NO_THROW(nvswitch.releaseGpioLines());
    EXPECT_EQ(
        test::fw_status_fake_gpio::lines["sw-fnp-catch-reset"].releaseCount, 1);
}

TEST_F(FWStatusResourceTest,
       NVSwitchAdditionalBranchesCoverStateAndRecoveryFailures)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    NVSwitchResource resource(
        bus, "/xyz/openbmc_project/software/nvswitch-extra",
        "/xyz/openbmc_project/state/chassis/nvswitch-extra",
        "/xyz/openbmc_project/inventory/system/chassis/nvswitch-extra", 17,
        0x5D, 53, 96, "sw-extra-reset", "sw-extra-fnp");

    resource.resetGpioName = "sw-extra-request-reset";
    resource.flashNotPresentGpioName = "sw-extra-request-fnp";
    test::fw_status_fake_gpio::lines["sw-extra-request-reset"] = {};
    test::fw_status_fake_gpio::lines["sw-extra-request-fnp"] = {};
    test::fw_status_fake_gpio::lines["sw-extra-request-fnp"].throwOnRequest =
        true;
    EXPECT_FALSE(resource.initGpioLines());

    resource.resetGpioName = "sw-extra-set-reset";
    resource.flashNotPresentGpioName = "sw-extra-set-fnp";
    test::fw_status_fake_gpio::lines["sw-extra-set-reset"] = {};
    test::fw_status_fake_gpio::lines["sw-extra-set-fnp"] = {};
    test::fw_status_fake_gpio::lines["sw-extra-set-fnp"].throwOnSet = true;
    const auto recoveryResult = resource.setForceRecoveryMode();
    EXPECT_FALSE(recoveryResult.first);
    EXPECT_NE(recoveryResult.second.find("Failed to set GPIO"),
              std::string::npos);
    ASSERT_NE(resource.recoveryModeInterface, nullptr);
    EXPECT_THROW(resource.recoveryModeInterface->setRecoveryMode(),
                 sdbusplus::exception_t);

    fakeOpenFail = true;
    resource.wasEnumeratedOnce = false;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
    fakeOpenFail = false;

    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST_F(FWStatusResourceTest,
       NVSwitchCoversEnumeratedStateAndRecoveryInitFailure)
{
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/ignored"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/54",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(54));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/97",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(97));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});

    NVSwitchResource resource(
        bus, "/xyz/openbmc_project/software/nvswitch-enumerated",
        "/xyz/openbmc_project/state/chassis/nvswitch-enumerated",
        "/xyz/openbmc_project/inventory/system/chassis/nvswitch-enumerated", 18,
        0x5E, 54, 97, "sw-enum-reset", "sw-enum-fnp");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/97");

    resource.resetGpioName = "sw-enum-missing-reset";
    resource.flashNotPresentGpioName = "sw-enum-missing-fnp";
    test::fw_status_fake_gpio::lines["sw-enum-missing-reset"].present = false;
    test::fw_status_fake_gpio::lines["sw-enum-missing-fnp"] = {};
    const auto initFailure = resource.setForceRecoveryMode();
    EXPECT_FALSE(initFailure.first);
    EXPECT_EQ(initFailure.second,
              "Failed to initialize GPIO lines for force recovery");

    auto signalBus = sdbusplus::bus::new_default();
    auto badAdd =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesAdded");
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, badAdd), 0);

    auto badRemove =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesRemoved");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, badRemove), 0);
}

TEST_F(FWStatusResourceTest,
       NVSwitchExplicitlyCoversStateTransitionsAndSmaLookup)
{
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/"
         "networks/1/endpoints/00-ignored"] = {
            {"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/57",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(57));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/100",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(100));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});

    NVSwitchResource resource(
        bus, "/xyz/openbmc_project/software/nvswitch-states",
        "/xyz/openbmc_project/state/chassis/nvswitch-states", "", 21, 0x61, 57,
        100, "", "");
    static_cast<BaseResource&>(resource).updateHealth();

    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/100");

    fakeOpenFail = true;
    resource.mctpObjectPath.clear();
    resource.wasEnumeratedOnce = true;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    resource.wasEnumeratedOnce = false;
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
    fakeOpenFail = false;

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x10, 0x00, 0x00, 0x00}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);
}

TEST_F(FWStatusResourceTest, NVSwitchCoversSmaFalseBranchesAndMonitorReuse)
{
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/ignored"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/badtype"] =
        {{mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/94"] = {
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(94)}}}};
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5e}});

    NVSwitchResource resource(
        bus, "/xyz/openbmc_project/software/nvswitch-sma-false",
        "/xyz/openbmc_project/state/chassis/nvswitch-sma-false", "", 24, 0x64,
        59, 95, "", "");
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto signalBus = sdbusplus::bus::new_default();
    exerciseSmaFalseBranchCallbacks(resource, signalBus, 95);
}

TEST_F(FWStatusResourceTest,
       ConnectXAndNicCoverLastByteRecoveryAndWrongRemoveBranches)
{
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x18}});
    ConnectXResource connectx(
        bus, "/xyz/openbmc_project/software/connectx-last-byte",
        "/xyz/openbmc_project/state/chassis/connectx-last-byte", "", 27, 0x67,
        64, 104, "", "");
    static_cast<BaseResource&>(connectx).updateHealth();
    EXPECT_EQ(connectx.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/105",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(105));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x18}});
    NVLinkMgmtNicResource nic(
        bus, "/xyz/openbmc_project/software/nic-last-byte",
        "/xyz/openbmc_project/state/chassis/nic-last-byte", "", 28, 0x68, 65,
        105, "", "");
    static_cast<BaseResource&>(nic).updateHealth();
    EXPECT_EQ(nic.state(), OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_EQ(nic.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/105");

    auto signalBus = sdbusplus::bus::new_default();
    auto wrongRemove = makeInterfacesRemovedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/other");
    EXPECT_GT(dispatchMatchCallback(nic.smaEndpointRemovedMatch, wrongRemove),
              0);
    EXPECT_EQ(nic.smaMctpObjectPath,
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/105");

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/106",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(106));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x00, 0x00, 0x00, 0x5f}});
    NVSwitchResource nvswitch(
        bus, "/xyz/openbmc_project/software/nvswitch-last-byte",
        "/xyz/openbmc_project/state/chassis/nvswitch-last-byte", "", 29, 0x69,
        66, 106, "", "");
    static_cast<BaseResource&>(nvswitch).updateHealth();
    EXPECT_EQ(nvswitch.state(),
              OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_EQ(nvswitch.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/106");

    auto wrongSwitchRemove = makeInterfacesRemovedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/other-sw");
    EXPECT_GT(dispatchMatchCallback(nvswitch.smaEndpointRemovedMatch,
                                    wrongSwitchRemove),
              0);
    EXPECT_EQ(nvswitch.smaMctpObjectPath,
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/106");
}

TEST_F(FWStatusResourceTest,
       ConnectXAndNicCoverMiddleByteRecoveryAndSmaLookupBranches)
{
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/no-intf"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/117",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(117));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/118",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(118));

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x01, 0x00, 0x19}});
    ConnectXResource connectxSecond(
        bus, "/xyz/openbmc_project/software/connectx-second-byte",
        "/xyz/openbmc_project/state/chassis/connectx-second-byte", "", 30, 0x6A,
        67, 117, "", "");
    static_cast<BaseResource&>(connectxSecond).updateHealth();
    EXPECT_EQ(connectxSecond.state(),
              OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_EQ(connectxSecond.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/117");

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x01, 0x19}});
    ConnectXResource connectxThird(
        bus, "/xyz/openbmc_project/software/connectx-third-byte",
        "/xyz/openbmc_project/state/chassis/connectx-third-byte", "", 31, 0x6B,
        68, 117, "", "");
    static_cast<BaseResource&>(connectxThird).updateHealth();
    EXPECT_EQ(connectxThird.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x01, 0x00, 0x19}});
    NVLinkMgmtNicResource nicSecond(
        bus, "/xyz/openbmc_project/software/nic-second-byte",
        "/xyz/openbmc_project/state/chassis/nic-second-byte", "", 32, 0x6C, 69,
        118, "", "");
    static_cast<BaseResource&>(nicSecond).updateHealth();
    EXPECT_EQ(nicSecond.state(),
              OperationalStatusServer::StateType::StandbyOffline);
    EXPECT_EQ(nicSecond.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/118");

    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x01, 0x19}});
    NVLinkMgmtNicResource nicThird(
        bus, "/xyz/openbmc_project/software/nic-third-byte",
        "/xyz/openbmc_project/state/chassis/nic-third-byte", "", 33, 0x6D, 70,
        118, "", "");
    static_cast<BaseResource&>(nicThird).updateHealth();
    EXPECT_EQ(nicThird.state(),
              OperationalStatusServer::StateType::StandbyOffline);
}

TEST_F(FWStatusResourceTest, GPUCoversAllUpdateHealthStatesAndRecoveryCallback)
{
    test::fw_status_fake_ocp::pushStatusReply(
        {.success = false, .output = {}, .error = "not accessible"});
    GpuResource absent(bus, "/xyz/openbmc_project/software/gpu0",
                       "/xyz/openbmc_project/state/chassis/gpu0",
                       "/xyz/openbmc_project/inventory/system/chassis/gpu0", 8,
                       0x63, 17, "/xyz/openbmc_project/software/info0");
    static_cast<BaseResource&>(absent).updateHealth();
    EXPECT_EQ(absent.state(), OperationalStatusServer::StateType::Absent);
    EXPECT_EQ(absent.inforomResource->state(),
              OperationalStatusServer::StateType::Absent);

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/18",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(18));
    GpuResource enumerated(bus, "/xyz/openbmc_project/software/gpu1",
                           "/xyz/openbmc_project/state/chassis/gpu1", "", 9,
                           0x64, 18, "/xyz/openbmc_project/software/info1");
    static_cast<BaseResource&>(enumerated).updateHealth();
    EXPECT_EQ(enumerated.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(enumerated.state(), OperationalStatusServer::StateType::Enabled);

    test::fw_status_fake_dbus::reset();
    test::fw_status_fake_ocp::pushStatusReply(
        {.success = true,
         .output = {0x00, static_cast<uint8_t>(
                              recovery_tool::DeviceStatus::RecoveryMode)},
         .error = ""});
    GpuResource recovery(bus, "/xyz/openbmc_project/software/gpu2",
                         "/xyz/openbmc_project/state/chassis/gpu2", "", 10,
                         0x65, 19, "/xyz/openbmc_project/software/info2");
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_EQ(recovery.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    test::fw_status_fake_ocp::pushStatusReply(
        {.success = true,
         .output = {0x00, static_cast<uint8_t>(
                              recovery_tool::DeviceStatus::DeviceHealthy)},
         .error = ""});
    static_cast<BaseResource&>(recovery).updateHealth();
    EXPECT_EQ(recovery.state(), OperationalStatusServer::StateType::Degraded);

    recovery.recoveryModeInterface.reset();
    test::fw_status_fake_ocp::forceRecoveryResult = {true, "ok"};
    GpuResource callback(bus, "/xyz/openbmc_project/software/gpu3",
                         "/xyz/openbmc_project/state/chassis/gpu3",
                         "/xyz/openbmc_project/inventory/system/chassis/gpu3",
                         11, 0x66, 20, "/xyz/openbmc_project/software/info3");
    ASSERT_NE(callback.recoveryModeInterface, nullptr);
    EXPECT_NO_THROW(callback.recoveryModeInterface->setRecoveryMode());

    test::fw_status_fake_ocp::forceRecoveryResult = {false, "force failed"};
    EXPECT_THROW(callback.recoveryModeInterface->setRecoveryMode(),
                 sdbusplus::exception_t);
}

TEST_F(FWStatusResourceTest, GPUCoversPowerOffRecoveryImageAndSmaFalseBranches)
{
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/ignored"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/badtype"] =
        {{mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/98"] = {
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(98)}}}};

    test::fw_status_fake_ocp::pushStatusReply(
        {.success = true,
         .output = {0x00, static_cast<uint8_t>(
                              recovery_tool::DeviceStatus::RecoveryImgRunning)},
         .error = ""});
    GpuResource resource(
        bus, "/xyz/openbmc_project/software/gpu-false-branches",
        "/xyz/openbmc_project/state/chassis/gpu-false-branches", "", 12, 0x67,
        77, 99, "/xyz/openbmc_project/software/info-false-branches");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    resource.monitorSMAEndpoint();
    ASSERT_NE(resource.smaEndpointAddedMatch, nullptr);
    ASSERT_NE(resource.smaEndpointRemovedMatch, nullptr);

    auto signalBus = sdbusplus::bus::new_default();
    auto wrongTypeAdd = makeInterfacesAddedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/99",
        nvidia::software::updater::InterfaceMap{
            {mctpEndpointIntfName, {{"EID", std::string("still-bad")}}}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, wrongTypeAdd), 0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto wrongValueAdd = makeInterfacesAddedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/98",
        nvidia::software::updater::InterfaceMap{
            {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(98)}}}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, wrongValueAdd),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto missingEidAdd = makeInterfacesAddedMessage(
        signalBus, "/au/com/codeconstruct/mctp1/networks/1/endpoints/99",
        nvidia::software::updater::InterfaceMap{{mctpEndpointIntfName, {}}});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingEidAdd),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
    ASSERT_NE(resource.inforomResource, nullptr);
    EXPECT_EQ(resource.inforomResource->state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    resource.inforomResource.reset();
    resource.wasEnumeratedOnce = true;
    test::fw_status_fake_ocp::pushStatusReply(
        {.success = false, .output = {}, .error = "gpu unreachable"});
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
}

TEST_F(FWStatusResourceTest,
       GPUWithoutInforomCoversEmptyOptionalAndSmaIgnoreBranches)
{
    test::fw_status_fake_ocp::pushStatusReply(
        {.success = true,
         .output = {0x00, static_cast<uint8_t>(
                              recovery_tool::DeviceStatus::DeviceHealthy)},
         .error = ""});
    GpuResource resource(bus, "/xyz/openbmc_project/software/gpu-no-inforom",
                         "/xyz/openbmc_project/state/chassis/gpu-no-inforom",
                         "", 13, 0x68, 78, 107, "");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.inforomResource, nullptr);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Degraded);

    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    static_cast<BaseResource&>(resource).updateHealth();
    EXPECT_EQ(resource.health(), HealthServer::HealthType::Warning);
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);
    EXPECT_EQ(resource.inforomResource, nullptr);

    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.On");

    auto signalBus = sdbusplus::bus::new_default();
    const std::string smaPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/107";

    auto missingInterface = makeInterfacesAddedMessage(
        signalBus, smaPath, nvidia::software::updater::InterfaceMap{});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingInterface),
        0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    auto malformedAdd =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesAdded");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, malformedAdd), 0);
    EXPECT_TRUE(resource.smaMctpObjectPath.empty());

    test::fw_status_fake_ocp::pushStatusReply(
        {.success = true,
         .output = {0x00, static_cast<uint8_t>(
                              recovery_tool::DeviceStatus::DeviceHealthy)},
         .error = ""});
    auto matchingAdd = makeInterfacesAddedMessage(signalBus, smaPath, 107);
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, matchingAdd), 0);
    EXPECT_EQ(resource.smaMctpObjectPath, smaPath);

    auto wrongRemove =
        makeInterfacesRemovedMessage(signalBus, smaPath + "-other");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, wrongRemove),
        0);
    EXPECT_EQ(resource.smaMctpObjectPath, smaPath);
}

TEST_F(FWStatusResourceTest, ResourceUpdateHealthCoversErrorStateEdges)
{
    using test::fw_status_fake_dbus::setProperty;

    auto resetFakes = [&] {
        openedPaths.clear();
        sleepCalls.clear();
        usleepCalls.clear();
        fakeOpenFail = false;
        fakeOpenErrno = ENOENT;
        test::fw_status_fake_dbus::reset();
        test::fw_status_fake_gpio::reset();
        test::fw_status_fake_i2c::reset();
        test::fw_status_fake_ocp::reset();
        test::fw_status_fake_mcu::reset();
    };

    resetFakes();
    setProperty("/au/com/codeconstruct/mctp1/networks/1/endpoints/130",
                mctpEndpointIntfName, "EID", static_cast<uint8_t>(130));
    test::fw_status_fake_i2c::pushReply(
        {.throwException = true, .error = "connectx sweep failure"});
    ConnectXResource connectx(
        bus, "/xyz/openbmc_project/software/connectx-sweep",
        "/xyz/openbmc_project/state/chassis/connectx-sweep", "", 50, 0x50, 131,
        130, "cx-sweep-reset", "cx-sweep-fnp");
    static_cast<BaseResource&>(connectx).updateHealth();
    EXPECT_EQ(connectx.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(connectx.state(), OperationalStatusServer::StateType::Absent);

    resetFakes();
    test::fw_status_fake_ocp::pushStatusReply(
        {.success = false, .output = {}, .error = "gpu sweep failure"});
    GpuResource gpu(bus, "/xyz/openbmc_project/software/gpu-sweep",
                    "/xyz/openbmc_project/state/chassis/gpu-sweep", "", 53,
                    0x53, 136, "/xyz/openbmc_project/software/gpu-sweep-info");
    static_cast<BaseResource&>(gpu).updateHealth();
    EXPECT_EQ(gpu.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(gpu.state(), OperationalStatusServer::StateType::Absent);

    auto manager = std::make_shared<mcu_recovery_manager::MCURecoveryManager>();
    resetFakes();
    test::fw_status_fake_mcu::devices["mcu-sweep"] = {.healthy = false,
                                                      .inRecovery = true};
    MCUResource mcu(bus, "/xyz/openbmc_project/software/mcu-sweep", 138,
                    "mcu-sweep", manager);
    static_cast<BaseResource&>(mcu).updateHealth();
    EXPECT_EQ(mcu.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(mcu.state(), OperationalStatusServer::StateType::StandbyOffline);
}

TEST_F(FWStatusResourceTest, MCTPDiscoveryCallbacksCoverAddRemoveAndErrorPaths)
{
    auto receiver = sdbusplus::bus::new_default();
    auto sender = sdbusplus::bus::new_default();
    sender.request_name(mctpService);

    CountingMCTPResource resource(receiver,
                                  "/xyz/openbmc_project/software/mctp0", 44);
    const std::string path = std::string(mctpObjPathPrefix) + "44";
    drainBus(receiver);

    auto add = makeInterfacesAddedMessage(sender, path, 44);
    sendObjectManagerSignal(add);
    pumpBusUntil(receiver, [&resource, &path] {
        return resource.mctpObjectPath == path && resource.wasEnumeratedOnce &&
               resource.updateCalls == 1;
    });
    EXPECT_EQ(resource.mctpObjectPath, path);
    EXPECT_TRUE(resource.wasEnumeratedOnce);
    EXPECT_EQ(resource.updateCalls, 1);

    auto badAdd = makeMalformedObjectManagerMessage(sender, "InterfacesAdded");
    sendObjectManagerSignal(badAdd);
    drainBus(receiver);

    auto remove = makeInterfacesRemovedMessage(sender, path);
    sendObjectManagerSignal(remove);
    pumpBusUntil(receiver, [&resource] {
        return resource.mctpObjectPath.empty() && resource.updateCalls == 2;
    });
    EXPECT_TRUE(resource.mctpObjectPath.empty());
    EXPECT_EQ(resource.updateCalls, 2);

    auto badRemove =
        makeMalformedObjectManagerMessage(sender, "InterfacesRemoved");
    sendObjectManagerSignal(badRemove);
    drainBus(receiver);
}

TEST_F(FWStatusResourceTest, SMAEndpointCallbacksCoverAddRemoveAndErrorPaths)
{
    auto sender = sdbusplus::bus::new_default();

    {
        auto receiver = sdbusplus::bus::new_default();
        test::fw_status_fake_ocp::pushStatusReply(
            {.success = true,
             .output = {0x00, static_cast<uint8_t>(
                                  recovery_tool::DeviceStatus::DeviceHealthy)},
             .error = ""});
        GpuResource gpu(receiver, "/xyz/openbmc_project/software/gpu_callbacks",
                        "/xyz/openbmc_project/state/chassis/gpu_callbacks", "",
                        9, 0x64, 18, 81,
                        "/xyz/openbmc_project/software/info_cb");
        drainBus(receiver);
        exerciseSmaEndpointCallbacks(receiver, sender, gpu, 81);
    }

    {
        auto receiver = sdbusplus::bus::new_default();
        test::fw_status_fake_i2c::pushReply(
            {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
        ConnectXResource connectx(
            receiver, "/xyz/openbmc_project/software/connectx_callbacks",
            "/xyz/openbmc_project/state/chassis/connectx_callbacks", "", 5,
            0x51, 12, 82, "reset_cb", "fnp_cb");
        drainBus(receiver);
        exerciseSmaEndpointCallbacks(receiver, sender, connectx, 82);
    }

    {
        auto receiver = sdbusplus::bus::new_default();
        test::fw_status_fake_i2c::pushReply(
            {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
        NVLinkMgmtNicResource nic(
            receiver, "/xyz/openbmc_project/software/nic_callbacks",
            "/xyz/openbmc_project/state/chassis/nic_callbacks", "", 6, 0x61, 13,
            83, "nic_reset_cb", "nic_fnp_cb");
        drainBus(receiver);
        exerciseSmaEndpointCallbacks(receiver, sender, nic, 83);
    }

    {
        auto receiver = sdbusplus::bus::new_default();
        test::fw_status_fake_i2c::pushReply(
            {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
        NVSwitchResource nvswitch(
            receiver, "/xyz/openbmc_project/software/nvswitch_callbacks",
            "/xyz/openbmc_project/state/chassis/nvswitch_callbacks", "", 7,
            0x62, 15, 84, "sw_reset_cb", "sw_fnp_cb");
        drainBus(receiver);
        exerciseSmaEndpointCallbacks(receiver, sender, nvswitch, 84);
    }
}
