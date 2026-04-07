#include <libusb-1.0/libusb.h>

#include <cstdarg>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace
{

struct FakeDeviceInfo
{
    uint16_t vendor = 0x0955;
    uint16_t product = 0x7410;
    uint8_t bus = 1;
    std::vector<uint8_t> ports{1};
    int descriptorResult = LIBUSB_SUCCESS;
    int activeConfigResult = LIBUSB_SUCCESS;
    uint8_t interfaceNumber = 3;
    std::vector<uint8_t> endpointAddresses{0x08};
    int openResult = LIBUSB_SUCCESS;
    int kernelDriverActiveResult = 0;
    int detachResult = LIBUSB_SUCCESS;
    int setConfigurationResult = LIBUSB_SUCCESS;
    int claimResult = LIBUSB_SUCCESS;
    int releaseResult = LIBUSB_SUCCESS;
    int attachResult = LIBUSB_SUCCESS;
    int refCalls = 0;
    int unrefCalls = 0;
    int closeCalls = 0;
    int freeListUnrefCalls = 0;
};

struct ConfigOwnership
{
    libusb_interface* interfaces = nullptr;
    libusb_interface_descriptor* altsettings = nullptr;
    libusb_endpoint_descriptor* endpoints = nullptr;
};

std::map<libusb_device*, FakeDeviceInfo> fakeDevices;
std::map<libusb_device_handle*, libusb_device*> handleOwners;
std::map<libusb_config_descriptor*, ConfigOwnership> configOwnership;
int libusbInitResult = LIBUSB_SUCCESS;
int libusbExitCalls = 0;
int libusbSetOptionCalls = 0;
ssize_t deviceListResult = 0;
std::vector<libusb_device*> deviceListEntries;
int freeDeviceListCalls = 0;

libusb_device* makeDevice(int id, FakeDeviceInfo info)
{
    auto* device = reinterpret_cast<libusb_device*>(
        static_cast<uintptr_t>(0x1000 + id * 0x100));
    fakeDevices.emplace(device, std::move(info));
    return device;
}

libusb_device_handle* makeHandle(libusb_device* device)
{
    return reinterpret_cast<libusb_device_handle*>(
        static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(device) + 0x10));
}

template <typename T>
void performSelfMoveAssign(T& value)
{
    auto* ptr = &value;
    value = std::move(*ptr);
}

class UsbDeviceManagerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fakeDevices.clear();
        handleOwners.clear();
        for (auto& [config, ownership] : configOwnership)
        {
            delete[] ownership.endpoints;
            delete[] ownership.altsettings;
            delete[] ownership.interfaces;
            delete config;
        }
        configOwnership.clear();
        libusbInitResult = LIBUSB_SUCCESS;
        libusbExitCalls = 0;
        libusbSetOptionCalls = 0;
        deviceListResult = 0;
        deviceListEntries.clear();
        freeDeviceListCalls = 0;
    }
};

} // namespace

extern "C" int __wrap_libusb_init(libusb_context** ctx)
{
    if (libusbInitResult == LIBUSB_SUCCESS)
    {
        *ctx = reinterpret_cast<libusb_context*>(0x4444);
    }
    else if (ctx != nullptr)
    {
        *ctx = nullptr;
    }
    return libusbInitResult;
}

extern "C" void __wrap_libusb_exit(libusb_context*)
{
    ++libusbExitCalls;
}

extern "C" int __wrap_libusb_set_option(libusb_context*, enum libusb_option,
                                        ...)
{
    ++libusbSetOptionCalls;
    return LIBUSB_SUCCESS;
}

extern "C" const char* __wrap_libusb_error_name(int)
{
    return "WRAPPED_LIBUSB_ERROR";
}

extern "C" const char* __wrap_libusb_strerror(enum libusb_error)
{
    return "wrapped libusb strerror";
}

extern "C" int
    __wrap_libusb_get_device_descriptor(libusb_device* device,
                                        libusb_device_descriptor* desc)
{
    auto it = fakeDevices.find(device);
    if (it == fakeDevices.end())
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    if (it->second.descriptorResult != LIBUSB_SUCCESS)
    {
        return it->second.descriptorResult;
    }

    desc->idVendor = it->second.vendor;
    desc->idProduct = it->second.product;
    return LIBUSB_SUCCESS;
}

extern "C" int __wrap_libusb_get_active_config_descriptor(
    libusb_device* device, libusb_config_descriptor** config)
{
    auto it = fakeDevices.find(device);
    if (it == fakeDevices.end())
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (it->second.activeConfigResult != LIBUSB_SUCCESS)
    {
        return it->second.activeConfigResult;
    }

    auto* endpoints =
        new libusb_endpoint_descriptor[it->second.endpointAddresses.size()]();
    for (size_t i = 0; i < it->second.endpointAddresses.size(); ++i)
    {
        endpoints[i].bEndpointAddress = it->second.endpointAddresses[i];
    }

    auto* altsettings = new libusb_interface_descriptor[1]();
    altsettings[0].bInterfaceNumber = it->second.interfaceNumber;
    altsettings[0].bNumEndpoints =
        static_cast<uint8_t>(it->second.endpointAddresses.size());
    altsettings[0].endpoint = endpoints;

    auto* interfaces = new libusb_interface[1]();
    interfaces[0].altsetting = altsettings;
    interfaces[0].num_altsetting = 1;

    auto* descriptor = new libusb_config_descriptor();
    descriptor->bNumInterfaces = 1;
    descriptor->interface = interfaces;

    configOwnership.emplace(
        descriptor, ConfigOwnership{interfaces, altsettings, endpoints});
    *config = descriptor;
    return LIBUSB_SUCCESS;
}

extern "C" void
    __wrap_libusb_free_config_descriptor(libusb_config_descriptor* config)
{
    auto it = configOwnership.find(config);
    if (it != configOwnership.end())
    {
        delete[] it->second.endpoints;
        delete[] it->second.altsettings;
        delete[] it->second.interfaces;
        delete config;
        configOwnership.erase(it);
    }
}

extern "C" int __wrap_libusb_get_port_numbers(libusb_device* device,
                                              uint8_t* port_numbers,
                                              int port_numbers_len)
{
    auto it = fakeDevices.find(device);
    if (it == fakeDevices.end() || it->second.ports.empty())
    {
        return 0;
    }

    const auto bytesToCopy = std::min<int>(
        port_numbers_len, static_cast<int>(it->second.ports.size()));
    for (int i = 0; i < bytesToCopy; ++i)
    {
        port_numbers[i] = it->second.ports[static_cast<size_t>(i)];
    }
    return bytesToCopy;
}

extern "C" uint8_t __wrap_libusb_get_bus_number(libusb_device* device)
{
    return fakeDevices.at(device).bus;
}

extern "C" int __wrap_libusb_open(libusb_device* device,
                                  libusb_device_handle** handle)
{
    auto& info = fakeDevices.at(device);
    if (info.openResult != LIBUSB_SUCCESS)
    {
        *handle = nullptr;
        return info.openResult;
    }

    auto* createdHandle = makeHandle(device);
    handleOwners[createdHandle] = device;
    *handle = createdHandle;
    return LIBUSB_SUCCESS;
}

extern "C" int __wrap_libusb_kernel_driver_active(libusb_device_handle* handle,
                                                  int)
{
    return fakeDevices.at(handleOwners.at(handle)).kernelDriverActiveResult;
}

extern "C" int __wrap_libusb_detach_kernel_driver(libusb_device_handle* handle,
                                                  int)
{
    return fakeDevices.at(handleOwners.at(handle)).detachResult;
}

extern "C" int __wrap_libusb_set_configuration(libusb_device_handle* handle,
                                               int)
{
    return fakeDevices.at(handleOwners.at(handle)).setConfigurationResult;
}

extern "C" int __wrap_libusb_claim_interface(libusb_device_handle* handle, int)
{
    return fakeDevices.at(handleOwners.at(handle)).claimResult;
}

extern "C" int __wrap_libusb_release_interface(libusb_device_handle* handle,
                                               int)
{
    return fakeDevices.at(handleOwners.at(handle)).releaseResult;
}

extern "C" int __wrap_libusb_attach_kernel_driver(libusb_device_handle* handle,
                                                  int)
{
    return fakeDevices.at(handleOwners.at(handle)).attachResult;
}

extern "C" void __wrap_libusb_close(libusb_device_handle* handle)
{
    auto device = handleOwners.at(handle);
    ++fakeDevices.at(device).closeCalls;
}

extern "C" ssize_t __wrap_libusb_get_device_list(libusb_context*,
                                                 libusb_device*** list)
{
    if (deviceListResult < 0)
    {
        *list = nullptr;
        return deviceListResult;
    }

    auto** allocated = new libusb_device*[deviceListEntries.size() + 1]();
    for (size_t i = 0; i < deviceListEntries.size(); ++i)
    {
        allocated[i] = deviceListEntries[i];
    }
    allocated[deviceListEntries.size()] = nullptr;
    *list = allocated;
    return static_cast<ssize_t>(deviceListEntries.size());
}

extern "C" void __wrap_libusb_free_device_list(libusb_device** list,
                                               int unref_devices)
{
    ++freeDeviceListCalls;
    if (unref_devices != 0)
    {
        for (size_t i = 0; list[i] != nullptr; ++i)
        {
            ++fakeDevices.at(list[i]).freeListUnrefCalls;
        }
    }
    delete[] list;
}

extern "C" libusb_device* __wrap_libusb_ref_device(libusb_device* device)
{
    ++fakeDevices.at(device).refCalls;
    return device;
}

extern "C" void __wrap_libusb_unref_device(libusb_device* device)
{
    ++fakeDevices.at(device).unrefCalls;
}

#include "../recovery_tool/usbrcm_recovery_tool/usb_device_manager.cpp"

TEST_F(UsbDeviceManagerTest, UsbContextHandlesInitSuccessAndFailure)
{
    libusbInitResult = LIBUSB_ERROR_NO_DEVICE;
    usb::UsbContext badContext;
    EXPECT_FALSE(badContext.isValid());
    EXPECT_EQ(libusbExitCalls, 0);

    libusbInitResult = LIBUSB_SUCCESS;
    {
        usb::UsbContext context;
        EXPECT_TRUE(context.isValid());
        EXPECT_EQ(libusbSetOptionCalls, 1);
    }
    EXPECT_EQ(libusbExitCalls, 1);
}

TEST_F(UsbDeviceManagerTest, IsDeviceInRcmModeChecksDescriptorAndEndpoints)
{
    auto* good =
        makeDevice(1, FakeDeviceInfo{.vendor = 0x0955, .product = 0x7410});
    auto* wrongVid =
        makeDevice(2, FakeDeviceInfo{.vendor = 0x1234, .product = 0x7410});
    auto* missingEndpoint =
        makeDevice(3, FakeDeviceInfo{.vendor = 0x0955,
                                     .product = 0x7410,
                                     .endpointAddresses = {0x03}});
    auto* badDescriptor =
        makeDevice(4, FakeDeviceInfo{.descriptorResult = LIBUSB_ERROR_IO});

    EXPECT_TRUE(usb::isDeviceInRcmMode(good, true));
    EXPECT_FALSE(usb::isDeviceInRcmMode(wrongVid, true));
    EXPECT_FALSE(usb::isDeviceInRcmMode(missingEndpoint, true));
    EXPECT_FALSE(usb::isDeviceInRcmMode(badDescriptor, true));
    EXPECT_FALSE(usb::isDeviceInRcmMode(nullptr, true));
}

TEST_F(UsbDeviceManagerTest,
       FindDevicesByVidPidFiltersDevicesAndBuildsPortPaths)
{
    auto* good = makeDevice(
        1, FakeDeviceInfo{
               .vendor = 0x0955, .product = 0x7410, .bus = 2, .ports = {1, 3}});
    auto* bad = makeDevice(
        2, FakeDeviceInfo{
               .vendor = 0x0955, .product = 0x9999, .bus = 4, .ports = {7}});
    deviceListEntries = {good, bad};

    {
        usb::UsbContext context;
        auto devices = usb::findDevicesByVidPid(context, true);
        ASSERT_EQ(devices.size(), 1u);
        EXPECT_EQ(devices.front().getPortPath(), "2-1.3");
    }

    EXPECT_EQ(fakeDevices.at(good).refCalls, 1);
    EXPECT_EQ(fakeDevices.at(good).freeListUnrefCalls, 1);
    EXPECT_EQ(fakeDevices.at(good).unrefCalls, 1);
    EXPECT_EQ(fakeDevices.at(bad).freeListUnrefCalls, 1);
    EXPECT_EQ(freeDeviceListCalls, 1);
}

TEST_F(UsbDeviceManagerTest, FindDeviceByPortPathHandlesMatchesAndBadInput)
{
    auto* first = makeDevice(
        1, FakeDeviceInfo{
               .vendor = 0x0955, .product = 0x7410, .bus = 1, .ports = {1}});
    auto* second = makeDevice(
        2, FakeDeviceInfo{
               .vendor = 0x0955, .product = 0x7410, .bus = 1, .ports = {2}});
    deviceListEntries = {first, second};

    usb::UsbContext context;
    auto found = usb::findDeviceByPortPath(context, "1-2", true);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->getPortPath(), "1-2");

    EXPECT_FALSE(usb::findDeviceByPortPath(context, "", true).has_value());
    EXPECT_FALSE(usb::findDeviceByPortPath(context, "1-3", true).has_value());
}

TEST_F(UsbDeviceManagerTest, UsbControlSessionAndHandleManageOpenAndCleanup)
{
    auto* device = makeDevice(
        1, FakeDeviceInfo{.vendor = 0x0955,
                          .product = 0x7410,
                          .bus = 7,
                          .ports = {9},
                          .kernelDriverActiveResult = 1,
                          .setConfigurationResult = LIBUSB_ERROR_BUSY});

    usb::UsbContext context;

    {
        usb::UsbControlSession session(device, context);
        ASSERT_TRUE(session.isValid());
        EXPECT_EQ(session.getPortPath(), "7-9");

        usb::UsbDeviceHandle handle(device, context, usb::INTERFACE_RECOVERY);
        ASSERT_TRUE(handle.isValid());
        EXPECT_EQ(handle.getPortPath(), "7-9");
    }

    EXPECT_EQ(fakeDevices.at(device).closeCalls, 2);
}

TEST_F(UsbDeviceManagerTest, UsbDeviceHandleClaimFailureTriggersCleanup)
{
    auto* device =
        makeDevice(1, FakeDeviceInfo{.vendor = 0x0955,
                                     .product = 0x7410,
                                     .claimResult = LIBUSB_ERROR_BUSY});

    usb::UsbContext context;
    usb::UsbDeviceHandle handle(device, context, usb::INTERFACE_RECOVERY);
    EXPECT_FALSE(handle.isValid());
    EXPECT_EQ(fakeDevices.at(device).closeCalls, 1);
}

TEST_F(UsbDeviceManagerTest, UsbContextMoveAndDeviceMoveAssignmentCoverCleanup)
{
    libusbInitResult = LIBUSB_SUCCESS;
    {
        usb::UsbContext first;
        usb::UsbContext second;
        EXPECT_TRUE(first.isValid());
        EXPECT_TRUE(second.isValid());
        first = std::move(second);
    }
    EXPECT_EQ(libusbExitCalls, 2);

    auto* firstDevice = makeDevice(1, FakeDeviceInfo{});
    auto* secondDevice = makeDevice(2, FakeDeviceInfo{});
    {
        usb::UsbDevice first(firstDevice, "1-1");
        usb::UsbDevice second(secondDevice, "1-2");
        first = std::move(second);
        EXPECT_EQ(first.getPortPath(), "1-2");
    }
    EXPECT_EQ(fakeDevices.at(firstDevice).unrefCalls, 1);
    EXPECT_EQ(fakeDevices.at(secondDevice).unrefCalls, 1);
}

TEST_F(UsbDeviceManagerTest,
       SessionsAndHandlesCoverNullInvalidOpenAndCleanupWarnings)
{
    usb::UsbContext validContext;
    libusbInitResult = LIBUSB_ERROR_IO;
    usb::UsbContext invalidContext;

    usb::UsbControlSession nullSession(nullptr, validContext);
    EXPECT_FALSE(nullSession.isValid());

    auto* closedDevice =
        makeDevice(1, FakeDeviceInfo{.vendor = 0x0955,
                                     .product = 0x7410,
                                     .openResult = LIBUSB_ERROR_IO});
    usb::UsbControlSession invalidCtxSession(closedDevice, invalidContext);
    EXPECT_FALSE(invalidCtxSession.isValid());

    usb::UsbControlSession openFailSession(closedDevice, validContext);
    EXPECT_FALSE(openFailSession.isValid());

    auto* first = makeDevice(
        2, FakeDeviceInfo{
               .vendor = 0x0955, .product = 0x7410, .bus = 3, .ports = {4}});
    auto* second = makeDevice(
        3, FakeDeviceInfo{
               .vendor = 0x0955, .product = 0x7410, .bus = 3, .ports = {5}});
    {
        usb::UsbControlSession sessionOne(first, validContext);
        usb::UsbControlSession sessionTwo(second, validContext);
        ASSERT_TRUE(sessionOne.isValid());
        ASSERT_TRUE(sessionTwo.isValid());
        sessionOne = std::move(sessionTwo);
        EXPECT_EQ(sessionOne.getPortPath(), "3-5");
    }
    EXPECT_EQ(fakeDevices.at(first).closeCalls, 1);
    EXPECT_EQ(fakeDevices.at(second).closeCalls, 1);

    auto* warningDevice =
        makeDevice(4, FakeDeviceInfo{.vendor = 0x0955,
                                     .product = 0x7410,
                                     .ports = {},
                                     .kernelDriverActiveResult = 1,
                                     .detachResult = LIBUSB_ERROR_BUSY,
                                     .setConfigurationResult = LIBUSB_ERROR_IO,
                                     .releaseResult = LIBUSB_ERROR_BUSY,
                                     .attachResult = LIBUSB_ERROR_BUSY});
    {
        usb::UsbDeviceHandle handle(warningDevice, validContext,
                                    usb::INTERFACE_RECOVERY);
        ASSERT_TRUE(handle.isValid());

        auto* movedTo = makeDevice(
            5,
            FakeDeviceInfo{.vendor = 0x0955, .product = 0x7410, .ports = {6}});
        usb::UsbDeviceHandle other(movedTo, validContext,
                                   usb::INTERFACE_RECOVERY);
        ASSERT_TRUE(other.isValid());
        handle = std::move(other);
        EXPECT_EQ(handle.getPortPath(), "1-6");
    }
    EXPECT_EQ(fakeDevices.at(warningDevice).closeCalls, 1);
}

TEST_F(UsbDeviceManagerTest,
       DiscoveryHandlesInvalidContextErrorsAndEmptyPortPaths)
{
    libusbInitResult = LIBUSB_ERROR_IO;
    usb::UsbContext invalidContext;
    EXPECT_TRUE(usb::findDevicesByVidPid(invalidContext, true).empty());
    EXPECT_FALSE(
        usb::findDeviceByPortPath(invalidContext, "1-1", true).has_value());

    libusbInitResult = LIBUSB_SUCCESS;
    usb::UsbContext context;

    deviceListResult = LIBUSB_ERROR_IO;
    EXPECT_TRUE(usb::findDevicesByVidPid(context, true).empty());

    auto* noPortDevice = makeDevice(
        1, FakeDeviceInfo{.vendor = 0x0955, .product = 0x7410, .ports = {}});
    deviceListResult = 0;
    deviceListEntries = {noPortDevice};
    auto devices = usb::findDevicesByVidPid(context, true);
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_TRUE(devices.front().getPortPath().empty());

    EXPECT_FALSE(usb::findDeviceByPortPath(context, "", true).has_value());
}

TEST_F(UsbDeviceManagerTest,
       AdditionalHelperAndMoveBranchesCoverInterfaceMismatchAndMoveConstructors)
{
    auto* wrongInterface =
        makeDevice(1, FakeDeviceInfo{.vendor = 0x0955,
                                     .product = 0x7410,
                                     .interfaceNumber = 2,
                                     .endpointAddresses = {0x08}});
    auto* badConfig =
        makeDevice(2, FakeDeviceInfo{.vendor = 0x0955,
                                     .product = 0x7410,
                                     .activeConfigResult = LIBUSB_ERROR_IO});

    EXPECT_FALSE(usb::isDeviceInRcmMode(wrongInterface, false));
    EXPECT_FALSE(usb::isDeviceInRcmMode(badConfig, false));

    usb::UsbContext firstContext;
    usb::UsbContext movedContext(std::move(firstContext));
    EXPECT_FALSE(firstContext.isValid());
    EXPECT_TRUE(static_cast<bool>(movedContext));

    auto* sessionDevice = makeDevice(
        3, FakeDeviceInfo{.vendor = 0x0955, .product = 0x7410, .ports = {7}});
    usb::UsbControlSession session(sessionDevice, movedContext);
    ASSERT_TRUE(session.isValid());
    usb::UsbControlSession movedSession(std::move(session));
    EXPECT_FALSE(session.isValid());
    EXPECT_TRUE(static_cast<bool>(movedSession));
    EXPECT_EQ(movedSession.getPortPath(), "1-7");

    auto* handleDevice = makeDevice(
        4, FakeDeviceInfo{.vendor = 0x0955, .product = 0x7410, .ports = {8}});
    usb::UsbDeviceHandle handle(handleDevice, movedContext,
                                usb::INTERFACE_RECOVERY);
    ASSERT_TRUE(handle.isValid());
    usb::UsbDeviceHandle movedHandle(std::move(handle));
    EXPECT_FALSE(handle.isValid());
    EXPECT_TRUE(static_cast<bool>(movedHandle));
    EXPECT_EQ(movedHandle.getPortPath(), "1-8");

    auto* originalDevice = makeDevice(5, FakeDeviceInfo{});
    usb::UsbDevice original(originalDevice, "9-1");
    usb::UsbDevice movedDevice(std::move(original));
    EXPECT_EQ(movedDevice.getPortPath(), "9-1");
    EXPECT_EQ(movedDevice.get(), originalDevice);
    EXPECT_EQ(original.get(), nullptr);
}

TEST_F(UsbDeviceManagerTest,
       AdditionalOpenFailureAndVerboseDiscoveryBranchesAreCovered)
{
    usb::UsbContext context;

    auto* noPortOpenFail = makeDevice(
        6, FakeDeviceInfo{.ports = {}, .openResult = LIBUSB_ERROR_BUSY});
    usb::UsbControlSession failedSession(noPortOpenFail, context);
    EXPECT_FALSE(failedSession.isValid());

    usb::UsbDeviceHandle nullHandle(nullptr, context, usb::INTERFACE_RECOVERY);
    EXPECT_FALSE(nullHandle.isValid());

    auto* noPortHandleFail = makeDevice(
        7, FakeDeviceInfo{.ports = {}, .openResult = LIBUSB_ERROR_IO});
    usb::UsbDeviceHandle failedHandle(noPortHandleFail, context,
                                      usb::INTERFACE_RECOVERY);
    EXPECT_FALSE(failedHandle.isValid());

    auto* cleanupDevice = makeDevice(
        8, FakeDeviceInfo{.vendor = 0x0955,
                          .product = 0x7410,
                          .ports = {10},
                          .releaseResult = LIBUSB_ERROR_NOT_FOUND,
                          .attachResult = LIBUSB_ERROR_NOT_SUPPORTED});
    {
        usb::UsbDeviceHandle handle(cleanupDevice, context,
                                    usb::INTERFACE_RECOVERY);
        ASSERT_TRUE(handle.isValid());
    }
    EXPECT_EQ(fakeDevices.at(cleanupDevice).closeCalls, 1);

    auto* mismatch = makeDevice(
        9, FakeDeviceInfo{.vendor = 0x1111, .product = 0x2222, .ports = {1}});
    deviceListEntries = {mismatch};
    auto found = usb::findDevicesByVidPid(context, true);
    EXPECT_TRUE(found.empty());
    EXPECT_FALSE(usb::findDeviceByPortPath(context, "1-99", true).has_value());
}

TEST_F(UsbDeviceManagerTest,
       SilentHelpersAndAssignmentEdgeCasesCoverAdditionalBranches)
{
    EXPECT_FALSE(usb::hasCorrectVidPid(nullptr, false));
    auto* badDescriptor =
        makeDevice(10, FakeDeviceInfo{.descriptorResult = LIBUSB_ERROR_IO});
    EXPECT_FALSE(usb::hasCorrectVidPid(badDescriptor, false));

    EXPECT_FALSE(usb::hasRecoveryEndpoints(nullptr, false));
    auto* badConfig =
        makeDevice(11, FakeDeviceInfo{.activeConfigResult = LIBUSB_ERROR_IO});
    EXPECT_FALSE(usb::hasRecoveryEndpoints(badConfig, false));
    EXPECT_TRUE(usb::getDevicePortPath(nullptr).empty());

    libusbInitResult = LIBUSB_SUCCESS;
    usb::UsbContext validContext;
    libusbInitResult = LIBUSB_ERROR_IO;
    usb::UsbContext invalidContext;

    performSelfMoveAssign(validContext);
    EXPECT_TRUE(validContext.isValid());

    validContext = std::move(invalidContext);
    EXPECT_FALSE(validContext.isValid());

    auto* firstDevice = makeDevice(12, FakeDeviceInfo{});
    auto* secondDevice = makeDevice(13, FakeDeviceInfo{});
    {
        usb::UsbDevice self(firstDevice, "4-1");
        performSelfMoveAssign(self);
        EXPECT_EQ(self.get(), firstDevice);

        usb::UsbDevice source(secondDevice, "4-2");
        usb::UsbDevice emptied(std::move(source));
        self = std::move(source);
        EXPECT_EQ(self.get(), nullptr);
        EXPECT_TRUE(self.getPortPath().empty());
    }

    libusbInitResult = LIBUSB_SUCCESS;
    usb::UsbContext ctx;
    auto* sourceSessionDevice = makeDevice(
        14, FakeDeviceInfo{.vendor = 0x0955, .product = 0x7410, .ports = {5}});
    auto* sourceHandleDevice = makeDevice(
        15, FakeDeviceInfo{.vendor = 0x0955, .product = 0x7410, .ports = {6}});

    usb::UsbControlSession nullSession(nullptr, ctx);
    usb::UsbControlSession sourceSession(sourceSessionDevice, ctx);
    ASSERT_TRUE(sourceSession.isValid());
    nullSession = std::move(sourceSession);
    EXPECT_TRUE(nullSession.isValid());
    EXPECT_EQ(nullSession.getPortPath(), "1-5");
    performSelfMoveAssign(nullSession);
    EXPECT_TRUE(nullSession.isValid());

    usb::UsbDeviceHandle nullHandle(nullptr, ctx, usb::INTERFACE_RECOVERY);
    usb::UsbDeviceHandle sourceHandle(sourceHandleDevice, ctx,
                                      usb::INTERFACE_RECOVERY);
    ASSERT_TRUE(sourceHandle.isValid());
    nullHandle = std::move(sourceHandle);
    EXPECT_TRUE(nullHandle.isValid());
    EXPECT_EQ(nullHandle.getPortPath(), "1-6");
    performSelfMoveAssign(nullHandle);
    EXPECT_TRUE(nullHandle.isValid());
}

TEST_F(UsbDeviceManagerTest,
       SilentDiscoveryAndPortAwareFailuresCoverAdditionalBranches)
{
    libusbInitResult = LIBUSB_SUCCESS;
    usb::UsbContext context;

    auto* pathOpenFail =
        makeDevice(16, FakeDeviceInfo{.vendor = 0x0955,
                                      .product = 0x7410,
                                      .ports = {9},
                                      .openResult = LIBUSB_ERROR_BUSY});
    usb::UsbDeviceHandle failedHandle(pathOpenFail, context,
                                      usb::INTERFACE_RECOVERY);
    EXPECT_FALSE(failedHandle.isValid());

    EXPECT_FALSE(usb::isDeviceInRcmMode(nullptr, false));

    libusbInitResult = LIBUSB_ERROR_IO;
    usb::UsbContext invalidContext;
    EXPECT_TRUE(usb::findDevicesByVidPid(invalidContext, false).empty());
    EXPECT_FALSE(
        usb::findDeviceByPortPath(invalidContext, "1-1", false).has_value());

    deviceListResult = LIBUSB_ERROR_IO;
    EXPECT_TRUE(usb::findDevicesByVidPid(context, false).empty());
    EXPECT_FALSE(usb::findDeviceByPortPath(context, "", false).has_value());

    auto* matched = makeDevice(
        17, FakeDeviceInfo{
                .vendor = 0x0955, .product = 0x7410, .bus = 3, .ports = {7}});
    deviceListResult = 0;
    deviceListEntries = {matched};
    auto found = usb::findDeviceByPortPath(context, "3-7", false);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->getPortPath(), "3-7");
    EXPECT_FALSE(usb::findDeviceByPortPath(context, "3-8", false).has_value());
}
