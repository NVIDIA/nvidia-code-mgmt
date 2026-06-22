#include "dbusutils.hpp"
#include "force_recovery.hpp"
#include "get_recovery_status.hpp"
#include "glacier_recovery_commands.hpp"
#include "gpiod.hpp"
#include "i2c_utils.hpp"
#include "mctp_vdm_helper.hpp"
#include "mcu_recovery_manager.hpp"
#include "message_registry.hpp"
#include "recovery_commands.hpp"
#include "usb_i2c_mapper.hpp"

#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <sdbusplus/message.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace
{

inline ::testing::NiceMock<sdbusplus::SdBusMock> sharedSdbusMock;
inline sdbusplus::bus_t sharedMockBus =
    sdbusplus::get_mocked_new(&sharedSdbusMock);

} // namespace

namespace sdbusplus::bus
{

inline sdbusplus::bus_t new_default_for_test()
{
    return sdbusplus::get_mocked_new(&::sharedSdbusMock);
}

} // namespace sdbusplus::bus

#pragma GCC push_options
#pragma GCC optimize("O0")
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define private public
#define protected public
#define new_default new_default_for_test
#define main fw_status_main_for_test
#include "../fw-status/fw-status.cpp"
#undef main
#undef new_default
#undef protected
#undef private
#pragma GCC diagnostic pop
#pragma GCC pop_options

namespace
{

using ResponseFn =
    std::function<int(sd_bus_message* request, sd_bus_message** reply)>;

int fakeUdevFd = -1;
bool failUdevNew = false;
bool failUdevMonitorNew = false;
int udevFilterResult = 0;
int udevEnableResult = 0;

struct CapturedMatch
{
    std::string rule;
    sd_bus_message_handler_t handler = nullptr;
    void* userdata = nullptr;
};

std::vector<CapturedMatch> capturedMatches{};

namespace wrap_state
{

std::deque<ResponseFn> responses;
int eventLoopCalls = 0;

void reset()
{
    responses.clear();
    eventLoopCalls = 0;
}

template <typename AppendFn>
int makeMethodReturn(sd_bus_message* request, sd_bus_message** reply,
                     AppendFn&& appendFn)
{
    if (sd_bus_message_new_method_return(request, reply) < 0 || !*reply)
    {
        return -EIO;
    }

    try
    {
        sdbusplus::message::message response(*reply, std::false_type{});
        appendFn(response);
        if (sd_bus_message_seal(response.get(), 0, 0) < 0)
        {
            return -EIO;
        }
        *reply = response.release();
        return 0;
    }
    catch (const std::exception&)
    {
        sd_bus_message_unref(*reply);
        *reply = nullptr;
        return -EIO;
    }
}

[[maybe_unused]] void pushStringVectorReply(std::vector<std::string> values)
{
    responses.push_back([values = std::move(values)](sd_bus_message* request,
                                                     sd_bus_message** reply) {
        return makeMethodReturn(
            request, reply, [&](auto& response) { response.append(values); });
    });
}

[[maybe_unused]] void pushErrorReply(int errorCode)
{
    responses.push_back(
        [errorCode](sd_bus_message*, sd_bus_message**) { return -errorCode; });
}

} // namespace wrap_state

extern "C" int __real_sd_bus_call(sd_bus*, sd_bus_message*, uint64_t,
                                  sd_bus_error*, sd_bus_message**);
extern "C" int __real_sd_bus_add_match(sd_bus*, sd_bus_slot**, const char*,
                                       sd_bus_message_handler_t, void*);

extern "C" int __wrap_sd_bus_call(sd_bus*, sd_bus_message* request, uint64_t,
                                  sd_bus_error* ret_error,
                                  sd_bus_message** reply)
{
    if (sd_bus_message_is_method_call(
            request, "xyz.openbmc_project.ObjectMapper", "GetSubTree") > 0)
    {
        return wrap_state::makeMethodReturn(
            request, reply, [&](auto& response) {
                response.append(dbus::GetSubTreeResponse{});
            });
    }

    if (sd_bus_message_is_method_call(request,
                                      "org.freedesktop.DBus.ObjectManager",
                                      "GetManagedObjects") > 0)
    {
        return wrap_state::makeMethodReturn(
            request, reply,
            [&](auto& response) { response.append(dbus::ObjectValueTree{}); });
    }

    if (sd_bus_message_is_method_call(request,
                                      "xyz.openbmc_project.ObjectMapper",
                                      "GetSubTreePaths") <= 0)
    {
        return __real_sd_bus_call(sd_bus_message_get_bus(request), request, 0,
                                  ret_error, reply);
    }

    if (!wrap_state::responses.empty())
    {
        auto response = std::move(wrap_state::responses.front());
        wrap_state::responses.pop_front();
        return response(request, reply);
    }

    if (ret_error)
    {
        ret_error->name = "org.freedesktop.DBus.Error.NoReply";
        ret_error->message = "no queued wrapped response";
    }
    return -ENOENT;
}

extern "C" int __wrap_sd_bus_add_match(sd_bus* bus, sd_bus_slot** slot,
                                       const char* match,
                                       sd_bus_message_handler_t callback,
                                       void* userdata)
{
    capturedMatches.push_back({.rule = match ? match : "",
                               .handler = callback,
                               .userdata = userdata});
    return __real_sd_bus_add_match(bus, slot, match, callback, userdata);
}

extern "C" int __wrap_sd_event_loop(sd_event*)
{
    ++wrap_state::eventLoopCalls;
    return 0;
}

extern "C" struct udev* __wrap_udev_new()
{
    if (failUdevNew)
    {
        return nullptr;
    }
    return reinterpret_cast<struct udev*>(0x1);
}

extern "C" struct udev_monitor*
    __wrap_udev_monitor_new_from_netlink(struct udev*, const char*)
{
    if (failUdevMonitorNew)
    {
        return nullptr;
    }
    return reinterpret_cast<struct udev_monitor*>(0x2);
}

extern "C" int __wrap_udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor*, const char*, const char*)
{
    return udevFilterResult;
}

extern "C" int __wrap_udev_monitor_enable_receiving(struct udev_monitor*)
{
    return udevEnableResult;
}

extern "C" int __wrap_udev_monitor_get_fd(struct udev_monitor*)
{
    return fakeUdevFd;
}

extern "C" struct udev_device*
    __wrap_udev_monitor_receive_device(struct udev_monitor*)
{
    return nullptr;
}

extern "C" const char* __wrap_udev_device_get_action(struct udev_device*)
{
    return nullptr;
}

extern "C" const char* __wrap_udev_device_get_devpath(struct udev_device*)
{
    return nullptr;
}

extern "C" struct udev* __wrap_udev_unref(struct udev* ctx)
{
    return ctx;
}

extern "C" struct udev_monitor*
    __wrap_udev_monitor_unref(struct udev_monitor* mon)
{
    return mon;
}

extern "C" struct udev_device* __wrap_udev_device_unref(struct udev_device* dev)
{
    return dev;
}

class ThrowingResource : public BaseResource
{
  public:
    ThrowingResource(sdbusplus::bus_t& bus, const std::string& path) :
        BaseResource(bus, path)
    {}

    int updateCalls = 0;
    bool throwOnUpdate = false;

    void updateHealth() override
    {
        ++updateCalls;
        if (throwOnUpdate)
        {
            throw std::runtime_error("forced update failure");
        }
        health(HealthServer::HealthType::OK);
        state(OperationalStatusServer::StateType::Enabled);
    }
};

class FWStatusMainTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        if (fakeUdevFd >= 0)
        {
            close(fakeUdevFd);
            fakeUdevFd = -1;
        }
        for (int fd : ownedFds)
        {
            if (fd >= 0)
            {
                close(fd);
            }
        }
        ownedFds.clear();
    }

    int makeEventFd()
    {
        const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fd < 0)
        {
            throw std::runtime_error("failed to create eventfd");
        }
        ownedFds.push_back(fd);
        return fd;
    }

    void SetUp() override
    {
        wrap_state::reset();
        capturedMatches.clear();
        testing::Mock::VerifyAndClearExpectations(&sharedSdbusMock);
        testing::Mock::VerifyAndClear(&sharedSdbusMock);
        test::fw_status_fake_dbus::reset();
        test::fw_status_fake_force_recovery::reset();
        test::fw_status_fake_glacier::reset();
        test::fw_status_fake_gpio::reset();
        test::fw_status_fake_i2c::reset();
        test::fw_status_fake_mcu::reset();
        test::fw_status_fake_mcu_mode::reset();
        test::fw_status_fake_ocp::reset();
        test::fw_status_fake_usb_i2c::mappedBus = 12;
        test::fw_status_fake_usb_recovery::reset();
        test::fw_status_fake_usbrcm_mode::reset();
        test::fw_status_fake_vdm::reset();

        ON_CALL(sharedSdbusMock,
                sd_bus_add_match(testing::_, testing::_, testing::_, testing::_,
                                 testing::_))
            .WillByDefault([](sd_bus*, sd_bus_slot** slot, const char*,
                              sd_bus_message_handler_t, void*) {
                if (slot != nullptr)
                {
                    *slot = nullptr;
                }
                return 0;
            });
        ON_CALL(sharedSdbusMock,
                sd_bus_add_object_vtable(testing::_, testing::_, testing::_,
                                         testing::_, testing::_, testing::_))
            .WillByDefault([](sd_bus*, sd_bus_slot** slot, const char*,
                              const char*, const sd_bus_vtable*, void*) {
                if (slot != nullptr)
                {
                    *slot = nullptr;
                }
                return 0;
            });
        ON_CALL(sharedSdbusMock, sd_bus_slot_unref(testing::_))
            .WillByDefault(testing::Return(nullptr));

        resources.clear();
        recoveryModeManagers.clear();
        entityManagerServiceMatch.reset();
        chassisPowerStateMatch.reset();
        chassisDiscoveryRetryMatch.reset();
        powerStateRefreshTimer.reset();
        mctpVdmHelper.reset();
        mcuRecoveryManager.reset();
        udevMonitor.reset();

        test::fw_status_fake_dbus::hostPowerStatus =
            "xyz.openbmc_project.State.Chassis.PowerState.On";
        fakeUdevFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        failUdevNew = false;
        failUdevMonitorNew = false;
        udevFilterResult = 0;
        udevEnableResult = 0;
    }

    nvidia::software::updater::InterfaceMap makeProperties()
    {
        return {};
    }

    std::vector<int> ownedFds{};
};

template <typename Predicate>
void pumpMainBusUntil(Predicate&& predicate, int maxAttempts = 10)
{
    for (int attempt = 0; attempt < maxAttempts && !predicate(); ++attempt)
    {
        if (getBus().process_discard())
        {
            continue;
        }
        getBus().wait(std::chrono::milliseconds(10));
    }
}

void expectSubTreePathsReply(const std::vector<std::string>& paths)
{
    EXPECT_CALL(sharedSdbusMock, sd_bus_call(testing::_, testing::_, testing::_,
                                             testing::_, testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });

    auto remaining = std::make_shared<std::vector<std::string>>(paths);
    auto index = std::make_shared<size_t>(0);
    ON_CALL(sharedSdbusMock,
            sd_bus_message_enter_container(testing::_, 'a', testing::_))
        .WillByDefault(testing::Return(0));
    ON_CALL(sharedSdbusMock, sd_bus_message_at_end(testing::_, testing::_))
        .WillByDefault([remaining, index](sd_bus_message*, int) {
            return *index >= remaining->size() ? 1 : 0;
        });
    ON_CALL(sharedSdbusMock,
            sd_bus_message_read_basic(testing::_, 's', testing::_))
        .WillByDefault([remaining, index](sd_bus_message*, char, void* output) {
            if (*index >= remaining->size())
            {
                return -EINVAL;
            }
            *static_cast<const char**>(output) =
                (*remaining)[(*index)++].c_str();
            return 1;
        });
    ON_CALL(sharedSdbusMock, sd_bus_message_exit_container(testing::_))
        .WillByDefault(testing::Return(0));
}

void expectSubTreePathsError(int errorCode)
{
    EXPECT_CALL(sharedSdbusMock, sd_bus_call(testing::_, testing::_, testing::_,
                                             testing::_, testing::_))
        .WillOnce(testing::Return(-errorCode));
}

[[maybe_unused]] void expectManagedObjectsReplyEmpty()
{
    EXPECT_CALL(sharedSdbusMock, sd_bus_call(testing::_, testing::_, testing::_,
                                             testing::_, testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
    ON_CALL(sharedSdbusMock,
            sd_bus_message_enter_container(testing::_, 'a', testing::_))
        .WillByDefault(testing::Return(0));
    ON_CALL(sharedSdbusMock, sd_bus_message_at_end(testing::_, testing::_))
        .WillByDefault(testing::Return(1));
    ON_CALL(sharedSdbusMock, sd_bus_message_exit_container(testing::_))
        .WillByDefault(testing::Return(0));
}

[[maybe_unused]] int
    dispatchMatchCallback(const std::unique_ptr<sdbusplus::bus::match_t>& match,
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

sdbusplus::message::message makeObjectManagerSignal(
    sdbusplus::bus_t& sender, const std::string& signalPath,
    const std::string& member, const std::string& objectPath,
    const InterfaceMap& interfaces)
{
    auto msg =
        sender.new_signal(signalPath.c_str(),
                          "org.freedesktop.DBus.ObjectManager", member.c_str());
    msg.append(sdbusplus::object_path(objectPath), interfaces);
    return msg;
}

sdbusplus::message::message
    makeMalformedObjectManagerSignal(sdbusplus::bus_t& sender,
                                     const std::string& signalPath,
                                     const std::string& member)
{
    auto msg =
        sender.new_signal(signalPath.c_str(),
                          "org.freedesktop.DBus.ObjectManager", member.c_str());
    msg.append(std::string("bad-payload"));
    return msg;
}

sdbusplus::message::message makePropertiesChangedSignal(
    sdbusplus::bus_t& sender, const std::string& objectPath,
    const std::map<std::string, std::variant<std::string>>& properties)
{
    auto msg =
        sender.new_signal(objectPath.c_str(), "org.freedesktop.DBus.Properties",
                          "PropertiesChanged");
    msg.append(chassisInterface, properties, std::vector<std::string>{});
    return msg;
}

struct ObjectMapperServiceState
{
    std::vector<std::string> subtreePaths{};
};

int handleObjectMapperGetSubTreePaths(sd_bus_message* raw, void* userdata,
                                      sd_bus_error*)
{
    auto* state = static_cast<ObjectMapperServiceState*>(userdata);
    auto msg = sdbusplus::message::message(raw);
    auto reply = msg.new_method_return();
    reply.append(state->subtreePaths);
    reply.method_return();
    return 1;
}

const sd_bus_vtable objectMapperVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetSubTreePaths", "sias", "as",
                  handleObjectMapperGetSubTreePaths,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

class ObjectMapperService
{
  public:
    explicit ObjectMapperService(std::vector<std::string> subtreePaths) :
        bus(sdbusplus::bus::new_default())
    {
        state.subtreePaths = std::move(subtreePaths);
        bus.request_name(MAPPER_BUSNAME);
        const int rc = sd_bus_add_object_vtable(
            sdbusplus::details::bus_friend::get_busp(bus), &slot, MAPPER_PATH,
            MAPPER_INTERFACE, objectMapperVTable, &state);
        if (rc < 0)
        {
            throw std::runtime_error(
                "failed to register ObjectMapper test service");
        }
        worker = std::thread([this] {
            while (!stopRequested.load())
            {
                if (bus.process_discard())
                {
                    continue;
                }
                bus.wait(std::chrono::milliseconds(10));
            }
        });
    }

    ~ObjectMapperService()
    {
        stopRequested = true;
        if (worker.joinable())
        {
            worker.join();
        }
        if (slot != nullptr)
        {
            sd_bus_slot_unref(slot);
            slot = nullptr;
        }
    }

  private:
    sdbusplus::bus_t bus;
    ObjectMapperServiceState state;
    std::atomic<bool> stopRequested = false;
    std::thread worker;
    sd_bus_slot* slot = nullptr;
};

TEST_F(FWStatusMainTest, HelperFunctionsCoverParsingAndRefresh)
{
    EXPECT_EQ(getSoftwareDBusObjectPath("/tmp/component0"),
              "/xyz/openbmc_project/software/component0");
    EXPECT_EQ(getChassisObjPath("chassis0"),
              "/xyz/openbmc_project/inventory/system/chassis/chassis0");

    InterfaceMap interfaces{{"iface",
                             {{"Name", std::string("gpu0")},
                              {"Count", static_cast<uint64_t>(42)},
                              {"Enabled", true},
                              {"Small", static_cast<uint64_t>(7)},
                              {"Large", static_cast<uint64_t>(0x1FF)},
                              {"hasChassisPowerSource", true}}}};

    EXPECT_EQ(getString(interfaces, "iface", "Name"), "gpu0");
    EXPECT_EQ(getUint64(interfaces, "iface", "Count"), 42u);
    EXPECT_TRUE(getBool(interfaces, "iface", "Enabled"));
    ASSERT_TRUE(getUint8(interfaces, "iface", "Small").has_value());
    EXPECT_EQ(*getUint8(interfaces, "iface", "Small"), 7);
    EXPECT_FALSE(getUint8(interfaces, "iface", "Large").has_value());
    EXPECT_TRUE(hasProperty(interfaces, "iface", "Name"));
    EXPECT_FALSE(hasProperty(interfaces, "iface", "Missing"));

    EXPECT_EQ(getString(interfaces, "iface", "Missing"), "");
    EXPECT_EQ(getUint64(interfaces, "iface", "Missing"), 0u);
    EXPECT_FALSE(getBool(interfaces, "iface", "Missing"));
    EXPECT_FALSE(getUint8(interfaces, "iface", "Missing").has_value());
    EXPECT_FALSE(hasProperty(interfaces, "missing", "Name"));

    ThrowingResource healthy(getBus(), "/xyz/openbmc_project/software/test0");
    applyChassisConnectionAndRefresh(
        interfaces, "iface", healthy,
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    EXPECT_TRUE(healthy.hasChassisPowerSource());
    EXPECT_EQ(healthy.getChassisPowerState(),
              "xyz.openbmc_project.State.Chassis.PowerState.On");
    EXPECT_EQ(healthy.updateCalls, 1);

    ThrowingResource disconnected(getBus(),
                                  "/xyz/openbmc_project/software/test-false");
    InterfaceMap disconnectedInterfaces{
        {"iface", {{"hasChassisPowerSource", false}}}};
    applyChassisConnectionAndRefresh(
        disconnectedInterfaces, "iface", disconnected,
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    EXPECT_FALSE(disconnected.hasChassisPowerSource());
    EXPECT_EQ(disconnected.getChassisPowerState(), "");
    EXPECT_EQ(disconnected.updateCalls, 1);

    ThrowingResource noChassis(getBus(), "/xyz/openbmc_project/software/test1");
    InterfaceMap noChassisInterfaces{{"iface", {{"Name", std::string("x")}}}};
    applyChassisConnectionAndRefresh(
        noChassisInterfaces, "iface", noChassis,
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    EXPECT_EQ(noChassis.updateCalls, 1);

    ThrowingResource throwing(getBus(), "/xyz/openbmc_project/software/test2");
    throwing.throwOnUpdate = true;
    EXPECT_NO_THROW(applyChassisConnectionAndRefresh(
        interfaces, "iface", throwing,
        "xyz.openbmc_project.State.Chassis.PowerState.Off"));
    EXPECT_EQ(throwing.updateCalls, 1);
}

TEST_F(FWStatusMainTest, HelperFunctionsCoverTypeMismatchCatchPaths)
{
    InterfaceMap mismatched{{"iface",
                             {{"Name", static_cast<uint64_t>(1)},
                              {"Count", std::string("bad-count")},
                              {"Enabled", std::string("bad-enabled")},
                              {"Small", std::string("bad-small")}}}};

    EXPECT_TRUE(getString(mismatched, "iface", "Missing").empty());
    EXPECT_TRUE(getString(mismatched, "iface", "Name").empty());
    EXPECT_EQ(getUint64(mismatched, "iface", "Count"), 0u);
    EXPECT_FALSE(getBool(mismatched, "iface", "Enabled"));
    EXPECT_FALSE(getUint8(mismatched, "iface", "Small").has_value());
    EXPECT_FALSE(hasProperty(mismatched, "missing-iface", "Anything"));
}

TEST_F(FWStatusMainTest, GetMCUConfigCoversUsbI2cAndMissingProperties)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/mcu_usb", mcuObjInterface,
                "Name", std::string("mcu-usb"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb", mcuObjInterface,
                "ResetGpioName", std::string("RST_USB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb", mcuObjInterface,
                "RecoveryGpioName", std::string("REC_USB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb", mcuObjInterface,
                "USBPort", std::string("1-2"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb", mcuObjInterface,
                "ProductId", static_cast<uint64_t>(0x1234));

    setProperty("/xyz/openbmc_project/inventory/mcu_i2c", mcuObjInterface,
                "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c", mcuObjInterface,
                "Name", std::string("mcu-i2c"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c", mcuObjInterface,
                "ResetGpioName", std::string("RST_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c", mcuObjInterface,
                "RecoveryGpioName", std::string("REC_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c", mcuObjInterface,
                "I2CBus", static_cast<uint64_t>(8));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c", mcuObjInterface,
                "NormalI2CAddress", static_cast<uint64_t>(0x52));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c", mcuObjInterface,
                "RecoveryI2CAddress", static_cast<uint64_t>(0x53));

    setProperty("/xyz/openbmc_project/inventory/mcu_missing_name",
                mcuObjInterface, "ResetGpioName", std::string("RST_X"));
    setProperty("/xyz/openbmc_project/inventory/mcu_missing_name",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_X"));

    setProperty("/xyz/openbmc_project/inventory/mcu_bad_usb", mcuObjInterface,
                "Name", std::string("bad-usb"));
    setProperty("/xyz/openbmc_project/inventory/mcu_bad_usb", mcuObjInterface,
                "ResetGpioName", std::string("RST_BAD"));
    setProperty("/xyz/openbmc_project/inventory/mcu_bad_usb", mcuObjInterface,
                "RecoveryGpioName", std::string("REC_BAD"));

    setProperty("/xyz/openbmc_project/inventory/mcu_bad_i2c", mcuObjInterface,
                "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_bad_i2c", mcuObjInterface,
                "Name", std::string("bad-i2c"));
    setProperty("/xyz/openbmc_project/inventory/mcu_bad_i2c", mcuObjInterface,
                "ResetGpioName", std::string("RST_BAD_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_bad_i2c", mcuObjInterface,
                "RecoveryGpioName", std::string("REC_BAD_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_bad_i2c", mcuObjInterface,
                "I2CBus", static_cast<uint64_t>(7));

    const auto config = getMCUConfig();
    ASSERT_EQ(config.size(), 2u);
    EXPECT_EQ(config.at("mcu-usb").usbPort, "1-2");
    EXPECT_EQ(config.at("mcu-usb").functionalPid, 0x1234);
    EXPECT_EQ(config.at("mcu-usb").interfaceType,
              mcu_recovery_manager::MCUInfo::InterfaceType::USB);
    EXPECT_EQ(config.at("mcu-i2c").i2cBus, 8);
    EXPECT_EQ(config.at("mcu-i2c").normalI2cAddress, 0x52);
    EXPECT_EQ(config.at("mcu-i2c").recoveryI2cAddress, 0x53);
    EXPECT_EQ(config.at("mcu-i2c").interfaceType,
              mcu_recovery_manager::MCUInfo::InterfaceType::I2C);
}

TEST_F(FWStatusMainTest, GetMCUConfigCoversSingleMissingPropertyBranches)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/mcu_valid_usb", mcuObjInterface,
                "Name", std::string("mcu-valid-usb"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_usb", mcuObjInterface,
                "ResetGpioName", std::string("RST_USB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_usb", mcuObjInterface,
                "RecoveryGpioName", std::string("REC_USB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_usb", mcuObjInterface,
                "USBPort", std::string("1-7"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_usb", mcuObjInterface,
                "ProductId", static_cast<uint64_t>(0x1234));

    setProperty("/xyz/openbmc_project/inventory/mcu_valid_i2c", mcuObjInterface,
                "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_i2c", mcuObjInterface,
                "Name", std::string("mcu-valid-i2c"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_i2c", mcuObjInterface,
                "ResetGpioName", std::string("RST_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_i2c", mcuObjInterface,
                "RecoveryGpioName", std::string("REC_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_i2c", mcuObjInterface,
                "I2CBus", static_cast<uint64_t>(8));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_i2c", mcuObjInterface,
                "NormalI2CAddress", static_cast<uint64_t>(0x52));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_i2c", mcuObjInterface,
                "RecoveryI2CAddress", static_cast<uint64_t>(0x53));

    setProperty("/xyz/openbmc_project/inventory/mcu_missing_reset",
                mcuObjInterface, "Name", std::string("missing-reset"));
    setProperty("/xyz/openbmc_project/inventory/mcu_missing_reset",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_ONLY"));
    setProperty("/xyz/openbmc_project/inventory/mcu_missing_reset",
                mcuObjInterface, "USBPort", std::string("1-8"));
    setProperty("/xyz/openbmc_project/inventory/mcu_missing_reset",
                mcuObjInterface, "ProductId", static_cast<uint64_t>(0x2000));

    setProperty("/xyz/openbmc_project/inventory/mcu_missing_recovery",
                mcuObjInterface, "Name", std::string("missing-recovery"));
    setProperty("/xyz/openbmc_project/inventory/mcu_missing_recovery",
                mcuObjInterface, "ResetGpioName", std::string("RST_ONLY"));
    setProperty("/xyz/openbmc_project/inventory/mcu_missing_recovery",
                mcuObjInterface, "USBPort", std::string("1-9"));
    setProperty("/xyz/openbmc_project/inventory/mcu_missing_recovery",
                mcuObjInterface, "ProductId", static_cast<uint64_t>(0x2001));

    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "Name", std::string("missing-bus"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "ResetGpioName", std::string("RST_MB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_MB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "NormalI2CAddress",
                static_cast<uint64_t>(0x61));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "RecoveryI2CAddress",
                static_cast<uint64_t>(0x62));

    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "Name", std::string("missing-normal"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "ResetGpioName", std::string("RST_MN"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_MN"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "I2CBus", static_cast<uint64_t>(9));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "RecoveryI2CAddress",
                static_cast<uint64_t>(0x63));

    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "Name", std::string("missing-recovery-i2c"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "ResetGpioName", std::string("RST_MR"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_MR"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "I2CBus", static_cast<uint64_t>(10));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "NormalI2CAddress",
                static_cast<uint64_t>(0x64));

    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_port",
                mcuObjInterface, "Name", std::string("missing-port"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_port",
                mcuObjInterface, "ResetGpioName", std::string("RST_MP"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_port",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_MP"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_port",
                mcuObjInterface, "ProductId", static_cast<uint64_t>(0x3000));

    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_pid",
                mcuObjInterface, "Name", std::string("missing-pid"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_pid",
                mcuObjInterface, "ResetGpioName", std::string("RST_PID"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_pid",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_PID"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_pid",
                mcuObjInterface, "USBPort", std::string("1-10"));

    const auto config = getMCUConfig();
    ASSERT_EQ(config.size(), 2u);
    EXPECT_TRUE(config.contains("mcu-valid-usb"));
    EXPECT_TRUE(config.contains("mcu-valid-i2c"));
}

TEST_F(FWStatusMainTest, GetMCUConfigCoversIsolatedShortCircuitMatrices)
{
    using test::fw_status_fake_dbus::setProperty;

    auto resetMcuState = [&] { test::fw_status_fake_dbus::reset(); };

    auto setBaseMcu = [&](const std::string& path, const std::string& name,
                          bool isI2c) {
        if (isI2c)
        {
            setProperty(path, mcuObjInterface, "Interface", std::string("I2C"));
        }
        setProperty(path, mcuObjInterface, "Name", name);
        setProperty(path, mcuObjInterface, "ResetGpioName",
                    std::string("RST_") + name);
        setProperty(path, mcuObjInterface, "RecoveryGpioName",
                    std::string("REC_") + name);
    };

    resetMcuState();
    setBaseMcu("/xyz/openbmc_project/inventory/mcu_i2c_valid", "i2c-valid",
               true);
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_valid", mcuObjInterface,
                "I2CBus", static_cast<uint64_t>(1));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_valid", mcuObjInterface,
                "NormalI2CAddress", static_cast<uint64_t>(0x50));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_valid", mcuObjInterface,
                "RecoveryI2CAddress", static_cast<uint64_t>(0x51));
    EXPECT_TRUE(getMCUConfig().contains("i2c-valid"));

    resetMcuState();
    setBaseMcu("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
               "i2c-missing-bus", true);
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "NormalI2CAddress",
                static_cast<uint64_t>(0x52));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_bus",
                mcuObjInterface, "RecoveryI2CAddress",
                static_cast<uint64_t>(0x53));
    EXPECT_TRUE(getMCUConfig().empty());

    resetMcuState();
    setBaseMcu("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
               "i2c-missing-normal", true);
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "I2CBus", static_cast<uint64_t>(2));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_normal",
                mcuObjInterface, "RecoveryI2CAddress",
                static_cast<uint64_t>(0x54));
    EXPECT_TRUE(getMCUConfig().empty());

    resetMcuState();
    setBaseMcu("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
               "i2c-missing-recovery", true);
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "I2CBus", static_cast<uint64_t>(3));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_missing_recovery",
                mcuObjInterface, "NormalI2CAddress",
                static_cast<uint64_t>(0x55));
    EXPECT_TRUE(getMCUConfig().empty());

    resetMcuState();
    setBaseMcu("/xyz/openbmc_project/inventory/mcu_usb_valid", "usb-valid",
               false);
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_valid", mcuObjInterface,
                "USBPort", std::string("9-1"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_valid", mcuObjInterface,
                "ProductId", static_cast<uint64_t>(0x1111));
    EXPECT_TRUE(getMCUConfig().contains("usb-valid"));

    resetMcuState();
    setBaseMcu("/xyz/openbmc_project/inventory/mcu_usb_missing_port",
               "usb-missing-port", false);
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_port",
                mcuObjInterface, "ProductId", static_cast<uint64_t>(0x2222));
    EXPECT_TRUE(getMCUConfig().empty());

    resetMcuState();
    setBaseMcu("/xyz/openbmc_project/inventory/mcu_usb_missing_pid",
               "usb-missing-pid", false);
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_missing_pid",
                mcuObjInterface, "USBPort", std::string("9-2"));
    EXPECT_TRUE(getMCUConfig().empty());
}

TEST_F(FWStatusMainTest, GetMCUConfigExhaustivelyCoversRequiredPropertyMasks)
{
    using test::fw_status_fake_dbus::setProperty;

    auto setCommonProperties = [&](const std::string& path,
                                   const std::string& name, bool isI2c) {
        test::fw_status_fake_dbus::reset();
        if (isI2c)
        {
            setProperty(path, mcuObjInterface, "Interface", std::string("I2C"));
        }
        setProperty(path, mcuObjInterface, "Name", name);
        setProperty(path, mcuObjInterface, "ResetGpioName",
                    std::string("RST_") + name);
        setProperty(path, mcuObjInterface, "RecoveryGpioName",
                    std::string("REC_") + name);
    };

    for (int mask = 0; mask < 8; ++mask)
    {
        SCOPED_TRACE(::testing::Message() << "i2c-mask=" << mask);
        const std::string path =
            std::format("/xyz/openbmc_project/inventory/mcu_i2c_mask_{}", mask);
        const std::string name = std::format("i2c-mask-{}", mask);
        setCommonProperties(path, name, true);

        if (mask & 0x1)
        {
            setProperty(path, mcuObjInterface, "I2CBus",
                        static_cast<uint64_t>(8 + mask));
        }
        if (mask & 0x2)
        {
            setProperty(path, mcuObjInterface, "NormalI2CAddress",
                        static_cast<uint64_t>(0x50 + mask));
        }
        if (mask & 0x4)
        {
            setProperty(path, mcuObjInterface, "RecoveryI2CAddress",
                        static_cast<uint64_t>(0x60 + mask));
        }

        const auto config = getMCUConfig();
        EXPECT_EQ(config.contains(name), mask == 0x7);
    }

    for (int mask = 0; mask < 4; ++mask)
    {
        SCOPED_TRACE(::testing::Message() << "usb-mask=" << mask);
        const std::string path =
            std::format("/xyz/openbmc_project/inventory/mcu_usb_mask_{}", mask);
        const std::string name = std::format("usb-mask-{}", mask);
        setCommonProperties(path, name, false);

        if (mask & 0x1)
        {
            setProperty(path, mcuObjInterface, "USBPort",
                        std::format("9-{}", mask + 1));
        }
        if (mask & 0x2)
        {
            setProperty(path, mcuObjInterface, "ProductId",
                        static_cast<uint64_t>(0x4000 + mask));
        }

        const auto config = getMCUConfig();
        EXPECT_EQ(config.contains(name), mask == 0x3);
    }
}

TEST_F(FWStatusMainTest, GetMCUConfigHandlesManagedObjectsFailure)
{
    using test::fw_status_fake_dbus::setProperty;

    auto seedConfig = [&] {
        test::fw_status_fake_dbus::reset();

        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc",
                    mcuObjInterface, "Interface", std::string("I2C"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc",
                    mcuObjInterface, "Name", std::string("mcu-i2c-alloc"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc",
                    mcuObjInterface, "ResetGpioName", std::string("RST_I2C"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc",
                    mcuObjInterface, "RecoveryGpioName",
                    std::string("REC_I2C"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc",
                    mcuObjInterface, "I2CBus", static_cast<uint64_t>(8));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc",
                    mcuObjInterface, "NormalI2CAddress",
                    static_cast<uint64_t>(0x52));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc",
                    mcuObjInterface, "RecoveryI2CAddress",
                    static_cast<uint64_t>(0x53));

        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc",
                    mcuObjInterface, "Name", std::string("mcu-usb-alloc"));
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc",
                    mcuObjInterface, "ResetGpioName", std::string("RST_USB"));
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc",
                    mcuObjInterface, "RecoveryGpioName",
                    std::string("REC_USB"));
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc",
                    mcuObjInterface, "USBPort", std::string("1-9"));
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc",
                    mcuObjInterface, "ProductId",
                    static_cast<uint64_t>(0x1234));
    };

    seedConfig();
    EXPECT_EQ(getMCUConfig().size(), 2u);

    test::fw_status_fake_dbus::throwOnGetManagedObjects = true;
    EXPECT_TRUE(getMCUConfig().empty());

    seedConfig();
    const auto config = getMCUConfig();
    EXPECT_EQ(config.size(), 2u);
    EXPECT_TRUE(config.contains("mcu-i2c-alloc"));
    EXPECT_TRUE(config.contains("mcu-usb-alloc"));
}

TEST_F(FWStatusMainTest, GetMCUConfigCoversTypeMismatchFallbackBranches)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_bad_types",
                mcuObjInterface, "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_bad_types",
                mcuObjInterface, "Name", std::string("mcu-i2c-bad-types"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_bad_types",
                mcuObjInterface, "ResetGpioName", std::string("RST_BAD_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_bad_types",
                mcuObjInterface, "RecoveryGpioName",
                std::string("REC_BAD_I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_bad_types",
                mcuObjInterface, "I2CBus", std::string("bad-bus"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_bad_types",
                mcuObjInterface, "NormalI2CAddress", std::string("bad-normal"));
    setProperty("/xyz/openbmc_project/inventory/mcu_i2c_bad_types",
                mcuObjInterface, "RecoveryI2CAddress",
                std::string("bad-recovery"));

    setProperty("/xyz/openbmc_project/inventory/mcu_usb_bad_types",
                mcuObjInterface, "Name", std::string("mcu-usb-bad-types"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_bad_types",
                mcuObjInterface, "ResetGpioName", std::string("RST_BAD_USB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_bad_types",
                mcuObjInterface, "RecoveryGpioName",
                std::string("REC_BAD_USB"));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_bad_types",
                mcuObjInterface, "USBPort", static_cast<uint64_t>(1));
    setProperty("/xyz/openbmc_project/inventory/mcu_usb_bad_types",
                mcuObjInterface, "ProductId", std::string("bad-pid"));

    const auto config = getMCUConfig();
    ASSERT_EQ(config.size(), 2u);
    EXPECT_EQ(config.at("mcu-i2c-bad-types").i2cBus, 0);
    EXPECT_EQ(config.at("mcu-i2c-bad-types").normalI2cAddress, 0);
    EXPECT_EQ(config.at("mcu-i2c-bad-types").recoveryI2cAddress, 0);
    EXPECT_EQ(config.at("mcu-i2c-bad-types").interfaceType,
              mcu_recovery_manager::MCUInfo::InterfaceType::I2C);
    EXPECT_EQ(config.at("mcu-usb-bad-types").usbPort, "");
    EXPECT_EQ(config.at("mcu-usb-bad-types").functionalPid, 0u);
    EXPECT_EQ(config.at("mcu-usb-bad-types").interfaceType,
              mcu_recovery_manager::MCUInfo::InterfaceType::USB);
}

TEST_F(FWStatusMainTest,
       GetMCUConfigI2cErrorPathRepeatedCallsCoverShortCircuitEdges)
{
    using test::fw_status_fake_dbus::setProperty;

    auto seedConfig = [&] {
        test::fw_status_fake_dbus::reset();
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc_missing",
                    mcuObjInterface, "Interface", std::string("I2C"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc_missing",
                    mcuObjInterface, "Name",
                    std::string("mcu-i2c-alloc-missing"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc_missing",
                    mcuObjInterface, "ResetGpioName", std::string("RST_MISS"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc_missing",
                    mcuObjInterface, "RecoveryGpioName",
                    std::string("REC_MISS"));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc_missing",
                    mcuObjInterface, "I2CBus", static_cast<uint64_t>(4));
        setProperty("/xyz/openbmc_project/inventory/mcu_i2c_alloc_missing",
                    mcuObjInterface, "NormalI2CAddress",
                    static_cast<uint64_t>(0x52));
    };

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        SCOPED_TRACE(::testing::Message() << "attempt=" << attempt);
        seedConfig();
        EXPECT_TRUE(getMCUConfig().empty());
    }
}

TEST_F(FWStatusMainTest,
       GetMCUConfigUsbErrorPathRepeatedCallsCoverShortCircuitEdges)
{
    using test::fw_status_fake_dbus::setProperty;

    auto seedConfig = [&] {
        test::fw_status_fake_dbus::reset();
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc_missing",
                    mcuObjInterface, "Name",
                    std::string("mcu-usb-alloc-missing"));
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc_missing",
                    mcuObjInterface, "ResetGpioName", std::string("RST_USB"));
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc_missing",
                    mcuObjInterface, "RecoveryGpioName",
                    std::string("REC_USB"));
        setProperty("/xyz/openbmc_project/inventory/mcu_usb_alloc_missing",
                    mcuObjInterface, "USBPort", std::string("3-7"));
    };

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        SCOPED_TRACE(::testing::Message() << "attempt=" << attempt);
        seedConfig();
        EXPECT_TRUE(getMCUConfig().empty());
    }
}

TEST_F(FWStatusMainTest, RecoveryConfigDiscoveryAndPublishingCoverMainFlow)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/recovery-marker",
                recoveryConfigIntfName, "Present", true);
    EXPECT_TRUE(checkForRecoveryConfigEMObjects());

    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface, "Name",
                std::string("mcu0"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "ResetGpioName", std::string("MCU_RST"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "RecoveryGpioName", std::string("MCU_REC"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "USBPort", std::string("1-5"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "ProductId", static_cast<uint64_t>(0x4321));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "MctpEID", static_cast<uint64_t>(31));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "ForceRecoveryChassisObject", std::string("cpu0"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "hasChassisPowerSource", true);

    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "I2CBus", static_cast<uint64_t>(4));
    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "I2CAddress", static_cast<uint64_t>(0x60));
    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "MctpEID", static_cast<uint64_t>(20));
    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "SMAEID", static_cast<uint64_t>(21));
    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "ChassisName", std::string("gpuchassis0"));
    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "ForceRecoveryChassisObject", std::string("gpufr0"));
    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "InfoRomName", std::string("gpu0_inforom"));
    setProperty("/xyz/openbmc_project/inventory/gpu0", ocpObjInterface,
                "hasChassisPowerSource", true);

    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "EID", static_cast<uint64_t>(22));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "I2CBus", static_cast<uint64_t>(5));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x61));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "SMAEID", static_cast<uint64_t>(23));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "ChassisName",
                std::string("connectxchassis0"));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "ForceRecoveryChassisObject",
                std::string("connectxforce0"));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "ResetGPIO", std::string("CX_RST"));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "FlashNotPresentGPIO",
                std::string("CX_FNP"));
    setProperty("/xyz/openbmc_project/inventory/connectx0",
                connectxObjInterface, "hasChassisPowerSource", true);

    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "EID", static_cast<uint64_t>(24));
    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "I2CBus", static_cast<uint64_t>(6));
    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x62));
    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "SMAEID", static_cast<uint64_t>(25));
    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "ChassisName",
                std::string("nicchassis0"));
    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "ForceRecoveryChassisObject",
                std::string("nicforce0"));
    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "ResetGPIO", std::string("NIC_RST"));
    setProperty("/xyz/openbmc_project/inventory/nic0",
                nvlinkMgmtNicObjInterface, "FlashNotPresentGPIO",
                std::string("NIC_FNP"));

    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "EID", static_cast<uint64_t>(26));
    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "I2CBus", static_cast<uint64_t>(7));
    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x63));
    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "SMAEID", static_cast<uint64_t>(27));
    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "ChassisName",
                std::string("switchchassis0"));
    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "ForceRecoveryChassisObject",
                std::string("switchforce0"));
    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "ResetGPIO", std::string("SW_RST"));
    setProperty("/xyz/openbmc_project/inventory/nvswitch0",
                nvswitchObjInterface, "FlashNotPresentGPIO",
                std::string("SW_FNP"));

    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "isRecoverable", true);
    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "I2CBus", static_cast<uint64_t>(8));
    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x64));
    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "MctpEID",
                static_cast<uint64_t>(28));
    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "APEID", static_cast<uint64_t>(29));
    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "APName", std::string("ap0"));
    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "APBootStatusType",
                std::string("ERoTBootStatus"));
    setProperty("/xyz/openbmc_project/inventory/erot0",
                glacierCrisisObjInterface, "ChassisName",
                std::string("erotchassis0"));

    setProperty("/xyz/openbmc_project/inventory/erot1",
                glacierCrisisObjInterface, "isRecoverable", false);
    setProperty("/xyz/openbmc_project/inventory/erot1",
                glacierCrisisObjInterface, "MctpEID",
                static_cast<uint64_t>(35));
    setProperty("/xyz/openbmc_project/inventory/erot1",
                glacierCrisisObjInterface, "APBootStatusType",
                std::string("ERoTBootStatus"));
    setProperty("/xyz/openbmc_project/inventory/erot1",
                glacierCrisisObjInterface, "ChassisName",
                std::string("erotchassis1"));

    setProperty("/xyz/openbmc_project/inventory/mcu1", mcuObjInterface,
                "Interface", std::string("I2C"));
    setProperty("/xyz/openbmc_project/inventory/mcu1", mcuObjInterface, "Name",
                std::string("mcu1"));
    setProperty("/xyz/openbmc_project/inventory/mcu1", mcuObjInterface,
                "MctpEID", static_cast<uint64_t>(36));

    setProperty("/xyz/openbmc_project/inventory/gpio_erot0", gpioObjInterface,
                "MctpEID", static_cast<uint64_t>(30));
    setProperty("/xyz/openbmc_project/inventory/gpio_erot0", gpioObjInterface,
                "GPIO", std::string("GPIO_EROT"));
    setProperty("/xyz/openbmc_project/inventory/gpio_erot0", gpioObjInterface,
                "IsERoT", true);
    setProperty("/xyz/openbmc_project/inventory/gpio_erot0", gpioObjInterface,
                "I2CBus", static_cast<uint64_t>(9));
    setProperty("/xyz/openbmc_project/inventory/gpio_erot0", gpioObjInterface,
                "I2CAddress", static_cast<uint64_t>(0x65));
    setProperty("/xyz/openbmc_project/inventory/gpio_erot0", gpioObjInterface,
                "Target", std::string("gpio-erot.target"));

    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "MctpEID", static_cast<uint64_t>(32));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "GPIO", std::string("GPIO_AP"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "IsERoT", false);
    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "RisingTarget", std::string("rise.target"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "FallingTarget", std::string("fall.target"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "Polarity", std::string("ActiveHigh"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "APBootStatusType", std::string("ERoTBootStatus"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap0", gpioObjInterface,
                "ChassisName", std::string("apchassis0"));

    setProperty("/xyz/openbmc_project/inventory/cpld0", cpldMonitorObjInterface,
                "SMAEID", static_cast<uint64_t>(33));
    setProperty("/xyz/openbmc_project/inventory/cpld0", cpldMonitorObjInterface,
                "DeviceId", std::string("CPLD_0"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm-force0",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usbfr0"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force0",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm0", usbRcmObjInterface,
                "USBPort", std::string("1-9"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm0", usbRcmObjInterface,
                "MctpEID", static_cast<uint64_t>(34));
    setProperty("/xyz/openbmc_project/inventory/usbrcm0", usbRcmObjInterface,
                "FMCComponentName", std::string("fmc0"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm0", usbRcmObjInterface,
                "FWSComponentName", std::string("fws0"));

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/21",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(21));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/23",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(23));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/25",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(25));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/27",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(27));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/33",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(33));

    for (int i = 0; i < 8; ++i)
    {
        test::fw_status_fake_i2c::pushReply(
            {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    }
    for (int i = 0; i < 4; ++i)
    {
        test::fw_status_fake_ocp::pushStatusReply(
            {.success = true,
             .output = {0x00, static_cast<uint8_t>(
                                  recovery_tool::DeviceStatus::DeviceHealthy)},
             .error = ""});
    }
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    test::fw_status_fake_glacier::pushResult(
        static_cast<uint8_t>(glacier_recovery_tool::glacier_recovery_commands::
                                 RecoveryResult::FirmwareNotInRecovery));
    test::fw_status_fake_gpio::lines["CX_RST"] = {};
    test::fw_status_fake_gpio::lines["CX_FNP"] = {};
    test::fw_status_fake_gpio::lines["NIC_RST"] = {};
    test::fw_status_fake_gpio::lines["NIC_FNP"] = {};
    test::fw_status_fake_gpio::lines["SW_RST"] = {};
    test::fw_status_fake_gpio::lines["SW_FNP"] = {};
    test::fw_status_fake_gpio::lines["GPIO_EROT"] = {};
    test::fw_status_fake_gpio::lines["GPIO_AP"] = {};
    test::fw_status_fake_gpio::lines["CX_RST"].eventFd = makeEventFd();
    test::fw_status_fake_gpio::lines["CX_FNP"].eventFd = makeEventFd();
    test::fw_status_fake_gpio::lines["NIC_RST"].eventFd = makeEventFd();
    test::fw_status_fake_gpio::lines["NIC_FNP"].eventFd = makeEventFd();
    test::fw_status_fake_gpio::lines["SW_RST"].eventFd = makeEventFd();
    test::fw_status_fake_gpio::lines["SW_FNP"].eventFd = makeEventFd();
    test::fw_status_fake_gpio::lines["GPIO_EROT"].eventFd = makeEventFd();
    test::fw_status_fake_gpio::lines["GPIO_AP"].eventFd = makeEventFd();
    test::fw_status_fake_usb_recovery::fallbackPayload = nlohmann::json::array(
        {{{"USB Port Path", "1-9"}, {"Recovery Status", "Not in Recovery"}}});
    mctpVdmHelper = std::make_shared<MCTPVdmHelper>();

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_GE(resources.size(), 8u);
    EXPECT_FALSE(test::fw_status_fake_mcu::initializedCount.empty());
    EXPECT_FALSE(test::fw_status_fake_mcu_mode::createdPaths.empty());
    EXPECT_FALSE(test::fw_status_fake_usbrcm_mode::createdPaths.empty());
    EXPECT_NE(chassisDiscoveryRetryMatch, nullptr);

    resources.clear();
    recoveryModeManagers.clear();
    test::fw_status_fake_dbus::reset();
    EXPECT_FALSE(checkForRecoveryConfigEMObjects());
    checkEntityManagerAvailability();
    EXPECT_NE(entityManagerServiceMatch, nullptr);
}

TEST_F(FWStatusMainTest, PublishSkipsDuplicateCpldResourceObjects)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/cpld0", cpldMonitorObjInterface,
                "SMAEID", static_cast<uint64_t>(33));
    setProperty("/xyz/openbmc_project/inventory/cpld0", cpldMonitorObjInterface,
                "DeviceId", std::string("CPLD_0"));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/33",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(33));

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    const auto publishedCount = resources.size();
    EXPECT_EQ(publishedCount, 1u);

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_EQ(resources.size(), publishedCount);
}

TEST_F(FWStatusMainTest, PublishRecoveryObjectCreatesNvlinkMgmtNicResource)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "EID", static_cast<uint64_t>(61));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "I2CBus", static_cast<uint64_t>(6));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x62));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "SMAEID", static_cast<uint64_t>(62));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "ChassisName",
                std::string("nic-only"));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "ForceRecoveryChassisObject",
                std::string("nic-force"));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "ResetGPIO",
                std::string("NIC_ONLY_RST"));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "FlashNotPresentGPIO",
                std::string("NIC_ONLY_FNP"));
    setProperty("/xyz/openbmc_project/inventory/nic-only",
                nvlinkMgmtNicObjInterface, "hasChassisPowerSource", true);

    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/61",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(61));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/62",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(62));
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});
    test::fw_status_fake_i2c::pushReply(
        {.success = true, .readData = {0x20, 0x00, 0x00, 0x19}});

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_NE(dynamic_cast<NVLinkMgmtNicResource*>(resources.front().get()),
              nullptr);
    EXPECT_EQ(resources.front()->getObjectPath(),
              "/xyz/openbmc_project/software/nic-only");
}

TEST_F(FWStatusMainTest, PublishRecoveryObjectCoversOptionalPropertyFalsePaths)
{
    using test::fw_status_fake_dbus::setProperty;

    expectSubTreePathsReply({"/xyz/openbmc_project/state/chassis/chassis0"});
    mctpVdmHelper = std::make_shared<MCTPVdmHelper>();
    test::fw_status_fake_usb_i2c::mappedBus = 15;
    test::fw_status_fake_gpio::lines["GPIO_AP_EMPTY"] = {};
    test::fw_status_fake_gpio::lines["GPIO_AP_EMPTY"].eventFd = makeEventFd();

    setProperty("/xyz/openbmc_project/inventory/ocp_usb_min", ocpObjInterface,
                "USBPort", std::string("1-4"));
    setProperty("/xyz/openbmc_project/inventory/ocp_usb_min", ocpObjInterface,
                "I2CAddress", static_cast<uint64_t>(0x50));
    setProperty("/xyz/openbmc_project/inventory/ocp_usb_min", ocpObjInterface,
                "MctpEID", static_cast<uint64_t>(40));
    setProperty("/xyz/openbmc_project/inventory/ocp_usb_min", ocpObjInterface,
                "ChassisName", std::string("ocp-min"));

    setProperty("/xyz/openbmc_project/inventory/connectx_min",
                connectxObjInterface, "EID", static_cast<uint64_t>(41));
    setProperty("/xyz/openbmc_project/inventory/connectx_min",
                connectxObjInterface, "I2CBus", static_cast<uint64_t>(4));
    setProperty("/xyz/openbmc_project/inventory/connectx_min",
                connectxObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x51));
    setProperty("/xyz/openbmc_project/inventory/connectx_min",
                connectxObjInterface, "SMAEID", static_cast<uint64_t>(42));
    setProperty("/xyz/openbmc_project/inventory/connectx_min",
                connectxObjInterface, "ChassisName",
                std::string("connectx-min"));

    setProperty("/xyz/openbmc_project/inventory/nic_min",
                nvlinkMgmtNicObjInterface, "EID", static_cast<uint64_t>(43));
    setProperty("/xyz/openbmc_project/inventory/nic_min",
                nvlinkMgmtNicObjInterface, "I2CBus", static_cast<uint64_t>(5));
    setProperty("/xyz/openbmc_project/inventory/nic_min",
                nvlinkMgmtNicObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x52));
    setProperty("/xyz/openbmc_project/inventory/nic_min",
                nvlinkMgmtNicObjInterface, "SMAEID", static_cast<uint64_t>(44));
    setProperty("/xyz/openbmc_project/inventory/nic_min",
                nvlinkMgmtNicObjInterface, "ChassisName",
                std::string("nic-min"));

    setProperty("/xyz/openbmc_project/inventory/switch_min",
                nvswitchObjInterface, "EID", static_cast<uint64_t>(45));
    setProperty("/xyz/openbmc_project/inventory/switch_min",
                nvswitchObjInterface, "I2CBus", static_cast<uint64_t>(6));
    setProperty("/xyz/openbmc_project/inventory/switch_min",
                nvswitchObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x53));
    setProperty("/xyz/openbmc_project/inventory/switch_min",
                nvswitchObjInterface, "SMAEID", static_cast<uint64_t>(46));
    setProperty("/xyz/openbmc_project/inventory/switch_min",
                nvswitchObjInterface, "ChassisName", std::string("switch-min"));

    setProperty("/xyz/openbmc_project/inventory/glacier_empty_ap",
                glacierCrisisObjInterface, "isRecoverable", true);
    setProperty("/xyz/openbmc_project/inventory/glacier_empty_ap",
                glacierCrisisObjInterface, "I2CBus", static_cast<uint64_t>(7));
    setProperty("/xyz/openbmc_project/inventory/glacier_empty_ap",
                glacierCrisisObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x54));
    setProperty("/xyz/openbmc_project/inventory/glacier_empty_ap",
                glacierCrisisObjInterface, "MctpEID",
                static_cast<uint64_t>(47));
    setProperty("/xyz/openbmc_project/inventory/glacier_empty_ap",
                glacierCrisisObjInterface, "ChassisName",
                std::string("glacier-empty"));

    setProperty("/xyz/openbmc_project/inventory/glacier_not_recoverable",
                glacierCrisisObjInterface, "isRecoverable", false);
    setProperty("/xyz/openbmc_project/inventory/glacier_not_recoverable",
                glacierCrisisObjInterface, "MctpEID",
                static_cast<uint64_t>(48));
    setProperty("/xyz/openbmc_project/inventory/glacier_not_recoverable",
                glacierCrisisObjInterface, "ChassisName",
                std::string("glacier-empty-off"));

    setProperty("/xyz/openbmc_project/inventory/gpio_ap_empty",
                gpioObjInterface, "MctpEID", static_cast<uint64_t>(49));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap_empty",
                gpioObjInterface, "GPIO", std::string("GPIO_AP_EMPTY"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap_empty",
                gpioObjInterface, "IsERoT", false);
    setProperty("/xyz/openbmc_project/inventory/gpio_ap_empty",
                gpioObjInterface, "RisingTarget", std::string("rise.empty"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap_empty",
                gpioObjInterface, "FallingTarget", std::string("fall.empty"));
    setProperty("/xyz/openbmc_project/inventory/gpio_ap_empty",
                gpioObjInterface, "Polarity", std::string("ActiveLow"));

    setProperty("/xyz/openbmc_project/inventory/mcu_valid_no_force",
                mcuObjInterface, "Name", std::string("mcu-valid"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_no_force",
                mcuObjInterface, "ResetGpioName", std::string("RST_VALID"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_no_force",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_VALID"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_no_force",
                mcuObjInterface, "USBPort", std::string("1-11"));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_no_force",
                mcuObjInterface, "ProductId", static_cast<uint64_t>(0x4000));
    setProperty("/xyz/openbmc_project/inventory/mcu_valid_no_force",
                mcuObjInterface, "MctpEID", static_cast<uint64_t>(50));

    setProperty("/xyz/openbmc_project/inventory/mcu_force_noname",
                mcuObjInterface, "MctpEID", static_cast<uint64_t>(51));
    setProperty("/xyz/openbmc_project/inventory/mcu_force_noname",
                mcuObjInterface, "ForceRecoveryChassisObject",
                std::string("mcu-force"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm_force_missing_chassis",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));
    test::fw_status_fake_dbus::managedObjects
        ["/xyz/openbmc_project/inventory/usbrcm_force_missing_both"]
        [usbRcmForceRecoveryObjInterface] = {};

    setProperty("/xyz/openbmc_project/inventory/usbrcm_valid",
                usbRcmObjInterface, "USBPort", std::string("1-12"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_valid",
                usbRcmObjInterface, "MctpEID", static_cast<uint64_t>(52));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_valid",
                usbRcmObjInterface, "FMCComponentName", std::string("fmc-min"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_valid",
                usbRcmObjInterface, "FWSComponentName", std::string("fws-min"));

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_EQ(resources.size(), 8u);
    EXPECT_EQ(test::fw_status_fake_mcu::initializedCount, "1");
    EXPECT_TRUE(test::fw_status_fake_mcu_mode::createdPaths.empty());
    EXPECT_TRUE(test::fw_status_fake_usbrcm_mode::createdPaths.empty());
    EXPECT_TRUE(recoveryModeManagers.empty());
    EXPECT_NE(chassisPowerStateMatch, nullptr);
    EXPECT_EQ(chassisDiscoveryRetryMatch, nullptr);
}

TEST_F(FWStatusMainTest, PublishRecoveryObjectCoversValidationFailures)
{
    using test::fw_status_fake_dbus::setProperty;

    test::fw_status_fake_usb_i2c::mappedBus = -1;

    setProperty("/xyz/openbmc_project/inventory/ocp_missing_eid",
                ocpObjInterface, "I2CAddress", static_cast<uint64_t>(0x50));
    setProperty("/xyz/openbmc_project/inventory/ocp_no_bus", ocpObjInterface,
                "MctpEID", static_cast<uint64_t>(2));
    setProperty("/xyz/openbmc_project/inventory/ocp_no_bus", ocpObjInterface,
                "I2CAddress", static_cast<uint64_t>(0x51));
    setProperty("/xyz/openbmc_project/inventory/ocp_bad_usb", ocpObjInterface,
                "MctpEID", static_cast<uint64_t>(3));
    setProperty("/xyz/openbmc_project/inventory/ocp_bad_usb", ocpObjInterface,
                "I2CAddress", static_cast<uint64_t>(0x52));
    setProperty("/xyz/openbmc_project/inventory/ocp_bad_usb", ocpObjInterface,
                "USBPort", std::string("1-1"));
    setProperty("/xyz/openbmc_project/inventory/ocp_bad_sma", ocpObjInterface,
                "MctpEID", static_cast<uint64_t>(4));
    setProperty("/xyz/openbmc_project/inventory/ocp_bad_sma", ocpObjInterface,
                "I2CBus", static_cast<uint64_t>(1));
    setProperty("/xyz/openbmc_project/inventory/ocp_bad_sma", ocpObjInterface,
                "I2CAddress", static_cast<uint64_t>(0x53));
    setProperty("/xyz/openbmc_project/inventory/ocp_bad_sma", ocpObjInterface,
                "SMAEID", static_cast<uint64_t>(0x1FF));

    setProperty("/xyz/openbmc_project/inventory/connectx_missing_eid",
                connectxObjInterface, "I2CBus", static_cast<uint64_t>(2));
    setProperty("/xyz/openbmc_project/inventory/connectx_missing_bus",
                connectxObjInterface, "EID", static_cast<uint64_t>(5));
    setProperty("/xyz/openbmc_project/inventory/connectx_missing_addr",
                connectxObjInterface, "EID", static_cast<uint64_t>(6));
    setProperty("/xyz/openbmc_project/inventory/connectx_missing_addr",
                connectxObjInterface, "I2CBus", static_cast<uint64_t>(3));
    setProperty("/xyz/openbmc_project/inventory/connectx_bad_sma",
                connectxObjInterface, "EID", static_cast<uint64_t>(7));
    setProperty("/xyz/openbmc_project/inventory/connectx_bad_sma",
                connectxObjInterface, "I2CBus", static_cast<uint64_t>(4));
    setProperty("/xyz/openbmc_project/inventory/connectx_bad_sma",
                connectxObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x54));
    setProperty("/xyz/openbmc_project/inventory/connectx_bad_sma",
                connectxObjInterface, "SMAEID", static_cast<uint64_t>(0x1FF));

    setProperty("/xyz/openbmc_project/inventory/nic_missing_eid",
                nvlinkMgmtNicObjInterface, "I2CBus", static_cast<uint64_t>(5));
    setProperty("/xyz/openbmc_project/inventory/nic_missing_bus",
                nvlinkMgmtNicObjInterface, "EID", static_cast<uint64_t>(8));
    setProperty("/xyz/openbmc_project/inventory/nic_missing_addr",
                nvlinkMgmtNicObjInterface, "EID", static_cast<uint64_t>(9));
    setProperty("/xyz/openbmc_project/inventory/nic_missing_addr",
                nvlinkMgmtNicObjInterface, "I2CBus", static_cast<uint64_t>(6));
    setProperty("/xyz/openbmc_project/inventory/nic_bad_sma",
                nvlinkMgmtNicObjInterface, "EID", static_cast<uint64_t>(10));
    setProperty("/xyz/openbmc_project/inventory/nic_bad_sma",
                nvlinkMgmtNicObjInterface, "I2CBus", static_cast<uint64_t>(7));
    setProperty("/xyz/openbmc_project/inventory/nic_bad_sma",
                nvlinkMgmtNicObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x55));
    setProperty("/xyz/openbmc_project/inventory/nic_bad_sma",
                nvlinkMgmtNicObjInterface, "SMAEID",
                static_cast<uint64_t>(0x1FF));

    setProperty("/xyz/openbmc_project/inventory/switch_missing_eid",
                nvswitchObjInterface, "I2CBus", static_cast<uint64_t>(8));
    setProperty("/xyz/openbmc_project/inventory/switch_missing_bus",
                nvswitchObjInterface, "EID", static_cast<uint64_t>(11));
    setProperty("/xyz/openbmc_project/inventory/switch_missing_addr",
                nvswitchObjInterface, "EID", static_cast<uint64_t>(12));
    setProperty("/xyz/openbmc_project/inventory/switch_missing_addr",
                nvswitchObjInterface, "I2CBus", static_cast<uint64_t>(9));
    setProperty("/xyz/openbmc_project/inventory/switch_bad_sma",
                nvswitchObjInterface, "EID", static_cast<uint64_t>(13));
    setProperty("/xyz/openbmc_project/inventory/switch_bad_sma",
                nvswitchObjInterface, "I2CBus", static_cast<uint64_t>(10));
    setProperty("/xyz/openbmc_project/inventory/switch_bad_sma",
                nvswitchObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x56));
    setProperty("/xyz/openbmc_project/inventory/switch_bad_sma",
                nvswitchObjInterface, "SMAEID", static_cast<uint64_t>(0x1FF));

    setProperty("/xyz/openbmc_project/inventory/glacier_missing_eid",
                glacierCrisisObjInterface, "isRecoverable", true);
    setProperty("/xyz/openbmc_project/inventory/glacier_missing_apeid",
                glacierCrisisObjInterface, "isRecoverable", true);
    setProperty("/xyz/openbmc_project/inventory/glacier_missing_apeid",
                glacierCrisisObjInterface, "MctpEID",
                static_cast<uint64_t>(14));
    setProperty("/xyz/openbmc_project/inventory/glacier_missing_apeid",
                glacierCrisisObjInterface, "I2CBus", static_cast<uint64_t>(11));
    setProperty("/xyz/openbmc_project/inventory/glacier_missing_apeid",
                glacierCrisisObjInterface, "I2CAddress",
                static_cast<uint64_t>(0x57));
    setProperty("/xyz/openbmc_project/inventory/glacier_missing_apeid",
                glacierCrisisObjInterface, "APBootStatusType",
                std::string("ERoTBootStatus"));

    setProperty("/xyz/openbmc_project/inventory/gpio_missing_eid",
                gpioObjInterface, "GPIO", std::string("GPIO_MISSING"));

    setProperty("/xyz/openbmc_project/inventory/cpld_missing_sma",
                cpldMonitorObjInterface, "DeviceId", std::string("CPLD_A"));
    setProperty("/xyz/openbmc_project/inventory/cpld_missing_device",
                cpldMonitorObjInterface, "SMAEID", static_cast<uint64_t>(15));
    setProperty("/xyz/openbmc_project/inventory/cpld_empty_device",
                cpldMonitorObjInterface, "SMAEID", static_cast<uint64_t>(16));
    setProperty("/xyz/openbmc_project/inventory/cpld_empty_device",
                cpldMonitorObjInterface, "DeviceId", std::string(""));

    setProperty("/xyz/openbmc_project/inventory/mcu_missing_eid",
                mcuObjInterface, "Name", std::string("mcu-missing-eid"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm_force_missing",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usb-force"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm_missing_usb",
                usbRcmObjInterface, "MctpEID", static_cast<uint64_t>(17));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_usb",
                usbRcmObjInterface, "USBPort", std::string(""));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_usb",
                usbRcmObjInterface, "MctpEID", static_cast<uint64_t>(18));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_missing_eid",
                usbRcmObjInterface, "USBPort", std::string("1-2"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_missing_fmc",
                usbRcmObjInterface, "USBPort", std::string("1-3"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_missing_fmc",
                usbRcmObjInterface, "MctpEID", static_cast<uint64_t>(19));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_fmc",
                usbRcmObjInterface, "USBPort", std::string("1-4"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_fmc",
                usbRcmObjInterface, "MctpEID", static_cast<uint64_t>(20));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_fmc",
                usbRcmObjInterface, "FMCComponentName", std::string(""));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_missing_fws",
                usbRcmObjInterface, "USBPort", std::string("1-5"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_missing_fws",
                usbRcmObjInterface, "MctpEID", static_cast<uint64_t>(21));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_missing_fws",
                usbRcmObjInterface, "FMCComponentName", std::string("fmc"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_fws",
                usbRcmObjInterface, "USBPort", std::string("1-6"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_fws",
                usbRcmObjInterface, "MctpEID", static_cast<uint64_t>(22));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_fws",
                usbRcmObjInterface, "FMCComponentName",
                std::string("fmc-empty"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm_empty_fws",
                usbRcmObjInterface, "FWSComponentName", std::string(""));

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_TRUE(resources.empty());
    EXPECT_TRUE(recoveryModeManagers.empty());
}

TEST_F(FWStatusMainTest, PublishRecoveryObjectCoversManagerCreationFailures)
{
    using test::fw_status_fake_dbus::setProperty;

    test::fw_status_fake_mcu_mode::throwOnCreate = true;
    test::fw_status_fake_usbrcm_mode::throwOnCreate = true;

    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface, "Name",
                std::string("mcu0"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "ResetGpioName", std::string("RST"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "RecoveryGpioName", std::string("REC"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "USBPort", std::string("2-1"));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "ProductId", static_cast<uint64_t>(0x1234));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "MctpEID", static_cast<uint64_t>(23));
    setProperty("/xyz/openbmc_project/inventory/mcu0", mcuObjInterface,
                "ForceRecoveryChassisObject", std::string("mcu0"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm-force0",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usb0"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force0",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(test::fw_status_fake_mcu::initializedCount, "1");
    EXPECT_TRUE(test::fw_status_fake_mcu_mode::createdPaths.empty());
    EXPECT_TRUE(test::fw_status_fake_usbrcm_mode::createdPaths.empty());
    EXPECT_TRUE(recoveryModeManagers.empty());
}

TEST_F(
    FWStatusMainTest,
    PublishRecoveryObjectCoversNullMCUManagerAndSingleMissingUSBRCMForceFields)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/mcu-no-config", mcuObjInterface,
                "MctpEID", static_cast<uint64_t>(60));
    setProperty("/xyz/openbmc_project/inventory/mcu-no-config", mcuObjInterface,
                "ForceRecoveryChassisObject", std::string("mcu-null"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-no-config",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usb-only-chassis"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-no-chassis",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_TRUE(test::fw_status_fake_mcu::initializedCount.empty());
    EXPECT_TRUE(recoveryModeManagers.empty());
}

TEST_F(FWStatusMainTest,
       PublishRecoveryObjectCatchesDedicatedRecoveryManagerConstructionFailures)
{
    using test::fw_status_fake_dbus::setProperty;

    test::fw_status_fake_mcu_mode::throwOnCreate = true;
    test::fw_status_fake_usbrcm_mode::throwOnCreate = true;

    setProperty("/xyz/openbmc_project/inventory/mcu-throw", mcuObjInterface,
                "Name", std::string("mcu-throw"));
    setProperty("/xyz/openbmc_project/inventory/mcu-throw", mcuObjInterface,
                "ResetGpioName", std::string("RST_THROW"));
    setProperty("/xyz/openbmc_project/inventory/mcu-throw", mcuObjInterface,
                "RecoveryGpioName", std::string("REC_THROW"));
    setProperty("/xyz/openbmc_project/inventory/mcu-throw", mcuObjInterface,
                "USBPort", std::string("4-1"));
    setProperty("/xyz/openbmc_project/inventory/mcu-throw", mcuObjInterface,
                "ProductId", static_cast<uint64_t>(0x4567));
    setProperty("/xyz/openbmc_project/inventory/mcu-throw", mcuObjInterface,
                "MctpEID", static_cast<uint64_t>(70));
    setProperty("/xyz/openbmc_project/inventory/mcu-throw", mcuObjInterface,
                "ForceRecoveryChassisObject", std::string("mcu-throw"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-throw",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usb-throw"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-throw",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_TRUE(recoveryModeManagers.empty());
    EXPECT_TRUE(test::fw_status_fake_mcu_mode::createdPaths.empty());
    EXPECT_TRUE(test::fw_status_fake_usbrcm_mode::createdPaths.empty());
}

TEST_F(FWStatusMainTest,
       PublishRecoveryObjectCoversIsolatedUSBRCMForceShortCircuitBranches)
{
    using test::fw_status_fake_dbus::setProperty;

    auto resetPublishState = [&] {
        test::fw_status_fake_dbus::reset();
        test::fw_status_fake_usbrcm_mode::reset();
        resources.clear();
        recoveryModeManagers.clear();
        mcuRecoveryManager.reset();
    };

    resetPublishState();
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-missing-chassis",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));
    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_TRUE(recoveryModeManagers.empty());

    resetPublishState();
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-missing-config",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usb-force-only"));
    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_TRUE(recoveryModeManagers.empty());

    resetPublishState();
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-valid",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usb-force-valid"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-valid",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));
    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_EQ(recoveryModeManagers.size(), 1u);
    EXPECT_EQ(
        test::fw_status_fake_usbrcm_mode::createdPaths,
        std::vector<std::string>{
            "/xyz/openbmc_project/inventory/system/chassis/usb-force-valid"});
}

TEST_F(FWStatusMainTest,
       PublishRecoveryObjectExhaustivelyCoversUSBRCMForcePropertyMasks)
{
    using test::fw_status_fake_dbus::setProperty;

    auto resetPublishState = [&] {
        test::fw_status_fake_dbus::reset();
        test::fw_status_fake_usbrcm_mode::reset();
        resources.clear();
        recoveryModeManagers.clear();
        mcuRecoveryManager.reset();
    };

    for (int mask = 0; mask < 4; ++mask)
    {
        SCOPED_TRACE(::testing::Message() << "usbrcm-force-mask=" << mask);
        resetPublishState();

        const std::string path = std::format(
            "/xyz/openbmc_project/inventory/usbrcm-force-mask-{}", mask);
        const std::string chassisName = std::format("usb-force-mask-{}", mask);

        if (mask & 0x1)
        {
            setProperty(path, usbRcmForceRecoveryObjInterface,
                        "ForceRecoveryChassisObject", chassisName);
        }
        if (mask & 0x2)
        {
            setProperty(path, usbRcmForceRecoveryObjInterface, "ConfigType",
                        std::string("vera"));
        }

        EXPECT_NO_THROW(publishDBusRecoveryObject());
        const bool shouldCreateManager = (mask == 0x3);
        EXPECT_EQ(!recoveryModeManagers.empty(), shouldCreateManager);
        EXPECT_EQ(!test::fw_status_fake_usbrcm_mode::createdPaths.empty(),
                  shouldCreateManager);
        if (shouldCreateManager)
        {
            EXPECT_EQ(test::fw_status_fake_usbrcm_mode::createdPaths,
                      std::vector<std::string>{std::format(
                          "/xyz/openbmc_project/inventory/system/chassis/{}",
                          chassisName)});
        }
    }
}

TEST_F(FWStatusMainTest,
       PublishRecoveryObjectCoversUSBRCMForceTypeMismatchBranches)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/mcu-type-mismatch",
                mcuObjInterface, "Name", std::string("mcu-type-mismatch"));
    setProperty("/xyz/openbmc_project/inventory/mcu-type-mismatch",
                mcuObjInterface, "ResetGpioName", std::string("RST_TYPE"));
    setProperty("/xyz/openbmc_project/inventory/mcu-type-mismatch",
                mcuObjInterface, "RecoveryGpioName", std::string("REC_TYPE"));
    setProperty("/xyz/openbmc_project/inventory/mcu-type-mismatch",
                mcuObjInterface, "USBPort", std::string("8-1"));
    setProperty("/xyz/openbmc_project/inventory/mcu-type-mismatch",
                mcuObjInterface, "ProductId", static_cast<uint64_t>(0x9911));
    setProperty("/xyz/openbmc_project/inventory/mcu-type-mismatch",
                mcuObjInterface, "MctpEID", static_cast<uint64_t>(81));
    setProperty("/xyz/openbmc_project/inventory/mcu-type-mismatch",
                mcuObjInterface, "ForceRecoveryChassisObject",
                std::string("mcu-type-mismatch"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-bad-chassis",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                static_cast<uint64_t>(1));
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-bad-chassis",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                std::string("vera"));

    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-bad-config",
                usbRcmForceRecoveryObjInterface, "ForceRecoveryChassisObject",
                std::string("usb-type-mismatch"));
    setProperty("/xyz/openbmc_project/inventory/usbrcm-force-bad-config",
                usbRcmForceRecoveryObjInterface, "ConfigType",
                static_cast<uint64_t>(2));

    EXPECT_NO_THROW(publishDBusRecoveryObject());
    EXPECT_EQ(resources.size(), 1u);
    ASSERT_EQ(recoveryModeManagers.size(), 3u);
    EXPECT_EQ(test::fw_status_fake_usbrcm_mode::createdPaths.size(), 2u);
    EXPECT_EQ(test::fw_status_fake_usbrcm_mode::createdPaths[0],
              "/xyz/openbmc_project/inventory/system/chassis/");
    EXPECT_EQ(
        test::fw_status_fake_usbrcm_mode::createdPaths[1],
        "/xyz/openbmc_project/inventory/system/chassis/usb-type-mismatch");
}

TEST_F(FWStatusMainTest, CheckEntityManagerAvailabilityIgnoresUnrelatedSignal)
{
    checkEntityManagerAvailability();
    ASSERT_NE(entityManagerServiceMatch, nullptr);

    auto signalBus = sdbusplus::bus::new_default();
    InterfaceMap unrelatedInterfaces{{"xyz.openbmc_project.Unrelated", {}}};
    auto signal = makeObjectManagerSignal(
        signalBus, entityManagerObjManager, "InterfacesAdded",
        "/xyz/openbmc_project/inventory/other0", unrelatedInterfaces);
    EXPECT_GT(dispatchMatchCallback(entityManagerServiceMatch, signal), 0);
    EXPECT_NE(entityManagerServiceMatch, nullptr);
    EXPECT_TRUE(resources.empty());
}

TEST_F(FWStatusMainTest, CheckEntityManagerAvailabilityRegistersWatcher)
{
    checkEntityManagerAvailability();
    ASSERT_NE(entityManagerServiceMatch, nullptr);
    EXPECT_TRUE(resources.empty());
}

TEST_F(FWStatusMainTest, CheckEntityManagerAvailabilityPublishesOnSignal)
{
    using test::fw_status_fake_dbus::setProperty;

    checkEntityManagerAvailability();
    ASSERT_NE(entityManagerServiceMatch, nullptr);

    setProperty("/xyz/openbmc_project/inventory/cpld-signal",
                recoveryConfigIntfName, "Present", true);
    setProperty("/xyz/openbmc_project/inventory/cpld-signal",
                cpldMonitorObjInterface, "SMAEID", static_cast<uint64_t>(44));
    setProperty("/xyz/openbmc_project/inventory/cpld-signal",
                cpldMonitorObjInterface, "DeviceId", std::string("CPLD_SIG"));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/44",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(44));

    auto signalBus = sdbusplus::bus::new_default();
    InterfaceMap interfaces{
        {recoveryConfigIntfName, {{"Marker", std::string("signal")}}}};
    auto signal = makeObjectManagerSignal(
        signalBus, entityManagerObjManager, "InterfacesAdded",
        "/xyz/openbmc_project/inventory/cpld-signal", interfaces);
    try
    {
        EXPECT_GT(dispatchMatchCallback(entityManagerServiceMatch, signal), 0);
    }
    catch (const std::exception&)
    {}

    if (resources.empty())
    {
        EXPECT_NO_THROW(publishDBusRecoveryObject());
    }
    // The match stays armed for the service lifetime (see the enumeration-race
    // fix), so it remains non-null after publishing rather than being reset.
    EXPECT_NE(entityManagerServiceMatch, nullptr);
    ASSERT_EQ(resources.size(), 1u);
}

TEST_F(FWStatusMainTest,
       CheckEntityManagerAvailabilityPublishesImmediatelyWhenPresent)
{
    using test::fw_status_fake_dbus::setProperty;

    setProperty("/xyz/openbmc_project/inventory/recovery-marker",
                recoveryConfigIntfName, "Present", true);
    setProperty("/xyz/openbmc_project/inventory/cpld-immediate",
                cpldMonitorObjInterface, "SMAEID", static_cast<uint64_t>(46));
    setProperty("/xyz/openbmc_project/inventory/cpld-immediate",
                cpldMonitorObjInterface, "DeviceId",
                std::string("CPLD_IMMEDIATE"));
    test::fw_status_fake_dbus::setProperty(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/46",
        mctpEndpointIntfName, "EID", static_cast<uint8_t>(46));

    checkEntityManagerAvailability();
    // The interfacesAdded match is kept armed for the service lifetime, even
    // when recovery configs are already present, so that configs published
    // later (EntityManager publishes incrementally) are still picked up.
    EXPECT_NE(entityManagerServiceMatch, nullptr);
    ASSERT_EQ(resources.size(), 1u);
}

TEST_F(FWStatusMainTest, CpldResourceCoversLookupAndWrongRemoveBranches)
{
    using test::fw_status_fake_dbus::setProperty;

    wrap_state::pushStringVectorReply({});
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/ignored"] =
        {{"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/badtype"] =
        {{mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    setProperty("/au/com/codeconstruct/mctp1/networks/1/endpoints/120",
                mctpEndpointIntfName, "EID", static_cast<uint8_t>(120));
    setProperty("/au/com/codeconstruct/mctp1/networks/1/endpoints/121",
                mctpEndpointIntfName, "EID", static_cast<uint8_t>(121));

    CpldResource resource(getBus(), "/xyz/openbmc_project/software/cpld-main",
                          121, "CPLD_MAIN");
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/121");

    auto signalBus = sdbusplus::bus::new_default();
    InterfaceMap wrongInterfaces{
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(120)}}}};
    auto wrongAdd = makeObjectManagerSignal(
        signalBus, mctpObjMgrPath.data(), "InterfacesAdded",
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/120",
        wrongInterfaces);
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, wrongAdd),
              0);
    EXPECT_EQ(resource.smaEndpointObjectPath,
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/121");

    InterfaceMap matchingInterfaces{
        {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(121)}}}};
    auto matchingAdd = makeObjectManagerSignal(
        signalBus, mctpObjMgrPath.data(), "InterfacesAdded",
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/121",
        matchingInterfaces);
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, matchingAdd), 0);
    EXPECT_EQ(resource.smaEndpointObjectPath,
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/121");

    auto wrongRemove = signalBus.new_signal(
        mctpObjMgrPath.data(), "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved");
    wrongRemove.append(
        sdbusplus::object_path(
            "/au/com/codeconstruct/mctp1/networks/1/endpoints/other"),
        std::vector<std::string>{mctpEndpointIntfName});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, wrongRemove),
        0);
    EXPECT_EQ(resource.smaEndpointObjectPath,
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/121");
}

TEST_F(FWStatusMainTest,
       CpldResourceLookupScansIgnoredAndWrongTypeEntriesBeforeMatching)
{
    using test::fw_status_fake_dbus::setProperty;

    wrap_state::pushStringVectorReply({});
    test::fw_status_fake_dbus::managedObjects.clear();
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/"
         "networks/1/endpoints/00-ignored"] = {
            {"xyz.openbmc_project.Fake", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/"
         "networks/1/endpoints/01-badtype"] = {
            {mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/02-wrong"] =
        {{mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(54)}}}};

    CpldResource resource(getBus(),
                          "/xyz/openbmc_project/software/cpld-ordered", 55,
                          "CPLD_ORDERED");
    EXPECT_TRUE(resource.smaEndpointObjectPath.empty());
    EXPECT_EQ(resource.getSMAMCTPObjectPath(), "");
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);

    setProperty("/au/com/codeconstruct/mctp1/networks/1/endpoints/zz-match",
                mctpEndpointIntfName, "EID", static_cast<uint8_t>(55));
    EXPECT_EQ(resource.getSMAMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/zz-match");
}

TEST_F(FWStatusMainTest,
       CpldResourceConstructorThrowsWhenExistingEndpointLacksEidProperty)
{
    wrap_state::pushStringVectorReply({});
    test::fw_status_fake_dbus::managedObjects.clear();
    test::fw_status_fake_dbus::managedObjects["/au/com/codeconstruct/mctp1/"
                                              "networks/1/endpoints/broken"] = {
        {mctpEndpointIntfName, {}}};

    EXPECT_THROW(CpldResource resource(
                     getBus(), "/xyz/openbmc_project/software/cpld-broken", 88,
                     "CPLD_BROKEN"),
                 std::out_of_range);
}

TEST_F(FWStatusMainTest, StartCentralizedPowerStateWatcherHandlesSignals)
{
    auto watched = std::make_unique<ThrowingResource>(
        getBus(), "/xyz/openbmc_project/software/watch0");
    watched->setConnectedToChassis(true);
    auto* watchedPtr = watched.get();
    resources.push_back(std::move(watched));

    auto ignored = std::make_unique<ThrowingResource>(
        getBus(), "/xyz/openbmc_project/software/watch1");
    auto* ignoredPtr = ignored.get();
    resources.push_back(std::move(ignored));

    expectSubTreePathsReply({"/xyz/openbmc_project/state/chassis/chassis0"});
    EXPECT_TRUE(startCentralizedPowerStateWatcher());
    ASSERT_NE(chassisPowerStateMatch, nullptr);

    auto signalBus = sdbusplus::bus::new_default();
    auto signal = makePropertiesChangedSignal(
        signalBus, "/xyz/openbmc_project/state/chassis/chassis0",
        {{"CurrentPowerState",
          std::string("xyz.openbmc_project.State.Chassis.PowerState.Off")}});
    // A power-state change updates the cached state synchronously, but the
    // fw-status re-check is deferred to a settle timer, so updateHealth() has
    // not run yet.
    EXPECT_GT(dispatchMatchCallback(chassisPowerStateMatch, signal), 0);
    EXPECT_EQ(watchedPtr->getChassisPowerState(),
              "xyz.openbmc_project.State.Chassis.PowerState.Off");
    EXPECT_EQ(watchedPtr->updateCalls, 0);
    EXPECT_EQ(ignoredPtr->updateCalls, 0);

    // Firing the deferred refresh probes only chassis-powered resources.
    refreshChassisPoweredResourcesHealth();
    EXPECT_EQ(watchedPtr->updateCalls, 1);
    EXPECT_EQ(ignoredPtr->updateCalls, 0);

    // An unrelated property change is ignored: the cached power state is left
    // untouched.
    auto ignoredSignal = makePropertiesChangedSignal(
        signalBus, "/xyz/openbmc_project/state/chassis/chassis0",
        {{"SomeOtherProperty", std::string("ignored")}});
    EXPECT_GT(dispatchMatchCallback(chassisPowerStateMatch, ignoredSignal), 0);
    EXPECT_EQ(watchedPtr->getChassisPowerState(),
              "xyz.openbmc_project.State.Chassis.PowerState.Off");

    // A malformed payload must not throw out of the watcher callback.
    auto badSignal = signalBus.new_signal(
        "/xyz/openbmc_project/state/chassis/chassis0",
        "org.freedesktop.DBus.Properties", "PropertiesChanged");
    badSignal.append(std::string("bad-payload"));
    EXPECT_GT(dispatchMatchCallback(chassisPowerStateMatch, badSignal), 0);

    auto secondSignal = makePropertiesChangedSignal(
        signalBus, "/xyz/openbmc_project/state/chassis/chassis0",
        {{"CurrentPowerState",
          std::string("xyz.openbmc_project.State.Chassis.PowerState.On")}});
    EXPECT_GT(dispatchMatchCallback(chassisPowerStateMatch, secondSignal), 0);

    // The deferred refresh swallows updateHealth() exceptions so a single
    // throwing resource does not abort the refresh of the others.
    watchedPtr->throwOnUpdate = true;
    EXPECT_NO_THROW(refreshChassisPoweredResourcesHealth());
    EXPECT_GE(watchedPtr->updateCalls, 2);
}

TEST_F(FWStatusMainTest, CentralizedWatcherRetryCoversRetryCallback)
{
    testing::InSequence seq;
    expectSubTreePathsError(ENOENT);
    armCentralizedPowerStateWatcherRetry();
    ASSERT_NE(chassisDiscoveryRetryMatch, nullptr);
    EXPECT_EQ(chassisPowerStateMatch, nullptr);

    auto signalBus = sdbusplus::bus::new_default();
    auto badSignal = makeMalformedObjectManagerSignal(signalBus, stateBasePath,
                                                      "InterfacesAdded");
    EXPECT_GT(dispatchMatchCallback(chassisDiscoveryRetryMatch, badSignal), 0);

    expectSubTreePathsReply({"/xyz/openbmc_project/state/chassis/chassis1"});
    InterfaceMap interfaces{
        {chassisInterface,
         {{"CurrentPowerState",
           std::string("xyz.openbmc_project.State.Chassis.PowerState.On")}}}};
    auto goodSignal = makeObjectManagerSignal(
        signalBus, stateBasePath, "InterfacesAdded",
        "/xyz/openbmc_project/state/chassis/chassis1", interfaces);
    EXPECT_GT(dispatchMatchCallback(chassisDiscoveryRetryMatch, goodSignal), 0);

    EXPECT_NE(chassisPowerStateMatch, nullptr);
    EXPECT_EQ(chassisDiscoveryRetryMatch, nullptr);
}

TEST_F(FWStatusMainTest,
       CentralizedWatcherCoversReplyDecodeFailureAndRetryFalseBranch)
{
    wrap_state::responses.push_back([](sd_bus_message* request,
                                       sd_bus_message** reply) {
        return wrap_state::makeMethodReturn(
            request, reply,
            [&](auto& response) { response.append(static_cast<uint64_t>(1)); });
    });
    EXPECT_FALSE(startCentralizedPowerStateWatcher());
    EXPECT_EQ(chassisPowerStateMatch, nullptr);

    armCentralizedPowerStateWatcherRetry();
    ASSERT_NE(chassisDiscoveryRetryMatch, nullptr);

    wrap_state::responses.push_back([](sd_bus_message* request,
                                       sd_bus_message** reply) {
        return wrap_state::makeMethodReturn(
            request, reply,
            [&](auto& response) { response.append(static_cast<uint64_t>(2)); });
    });

    auto signalBus = sdbusplus::bus::new_default();
    InterfaceMap interfaces{
        {chassisInterface,
         {{"CurrentPowerState",
           std::string("xyz.openbmc_project.State.Chassis.PowerState.On")}}}};
    auto goodSignal = makeObjectManagerSignal(
        signalBus, stateBasePath, "InterfacesAdded",
        "/xyz/openbmc_project/state/chassis/chassis-decode-fail", interfaces);
    EXPECT_GT(dispatchMatchCallback(chassisDiscoveryRetryMatch, goodSignal), 0);

    EXPECT_EQ(chassisPowerStateMatch, nullptr);
    EXPECT_NE(chassisDiscoveryRetryMatch, nullptr);
}

TEST_F(FWStatusMainTest, CentralizedWatcherRetryCoversImmediateRaceSuccess)
{
    expectSubTreePathsReply(
        {"/xyz/openbmc_project/state/chassis/chassis-race"});
    armCentralizedPowerStateWatcherRetry();

    EXPECT_NE(chassisPowerStateMatch, nullptr);
    EXPECT_EQ(chassisDiscoveryRetryMatch, nullptr);
}

TEST_F(FWStatusMainTest, CentralizedWatcherCoversEarlyReturnsAndUnrelatedRetry)
{
    expectSubTreePathsReply({"/xyz/openbmc_project/state/chassis/chassis0"});
    EXPECT_TRUE(startCentralizedPowerStateWatcher());
    auto* activeMatch = chassisPowerStateMatch.get();
    ASSERT_NE(activeMatch, nullptr);

    EXPECT_TRUE(startCentralizedPowerStateWatcher());
    EXPECT_EQ(chassisPowerStateMatch.get(), activeMatch);

    armCentralizedPowerStateWatcherRetry();
    EXPECT_EQ(chassisDiscoveryRetryMatch, nullptr);

    chassisPowerStateMatch.reset();

    expectSubTreePathsError(ENOENT);
    armCentralizedPowerStateWatcherRetry();
    auto* retryMatch = chassisDiscoveryRetryMatch.get();
    ASSERT_NE(retryMatch, nullptr);
    EXPECT_EQ(chassisPowerStateMatch, nullptr);

    armCentralizedPowerStateWatcherRetry();
    EXPECT_EQ(chassisDiscoveryRetryMatch.get(), retryMatch);

    auto signalBus = sdbusplus::bus::new_default();
    InterfaceMap unrelatedInterfaces{{"xyz.openbmc_project.Unrelated", {}}};
    auto unrelatedSignal = makeObjectManagerSignal(
        signalBus, stateBasePath, "InterfacesAdded",
        "/xyz/openbmc_project/state/chassis/chassis2", unrelatedInterfaces);
    EXPECT_GT(
        dispatchMatchCallback(chassisDiscoveryRetryMatch, unrelatedSignal), 0);
    EXPECT_EQ(chassisPowerStateMatch, nullptr);
    EXPECT_EQ(chassisDiscoveryRetryMatch.get(), retryMatch);
}

TEST_F(FWStatusMainTest, StartCentralizedWatcherHandlesDiscoveryFailure)
{
    testing::InSequence seq;
    expectSubTreePathsError(ENOENT);
    EXPECT_FALSE(startCentralizedPowerStateWatcher());
    EXPECT_EQ(chassisPowerStateMatch, nullptr);

    expectSubTreePathsReply({});
    EXPECT_FALSE(startCentralizedPowerStateWatcher());
    EXPECT_EQ(chassisPowerStateMatch, nullptr);
}

} // namespace
