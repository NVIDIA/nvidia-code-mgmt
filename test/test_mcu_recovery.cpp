#include "dbusutils.hpp"

#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#define private public
#include "../recovery_tool/mcu_recovery_tool/mcu_recovery_manager.hpp"
#include "../recovery_tool/mcu_recovery_tool/utils.hpp"
#undef private

namespace
{

struct FakeUsbDeviceInfo
{
    uint16_t vendor = mcu_recovery_manager::nvdaVendorId;
    uint16_t product = 0x7410;
    uint8_t bus = 1;
    uint8_t address = 2;
    std::vector<uint8_t> ports{1};
    std::vector<uint8_t> interfaceClasses{
        mcu_recovery_manager::LIBUSB_CLASS_MCTP};
    int descriptorResult = LIBUSB_SUCCESS;
    int configResult = LIBUSB_SUCCESS;
    int portNumbersResult = -999;
    int freeListUnrefCalls = 0;
};

struct ConfigOwnership
{
    libusb_interface* interfaces = nullptr;
    libusb_interface_descriptor* altsettings = nullptr;
};

struct DeviceListResponse
{
    ssize_t result = 0;
    std::vector<libusb_device*> devices;
};

struct IoctlExpectation
{
    unsigned long request;
    int ret = 0;
    int errnoValue = 0;
};

struct PopenExpectation
{
    std::string needle;
    std::string output;
    bool failOpen = false;
};

std::map<libusb_device*, FakeUsbDeviceInfo> fakeUsbDevices;
std::map<libusb_config_descriptor*, ConfigOwnership> configOwnership;
std::deque<DeviceListResponse> deviceListResponses;
std::vector<std::string> freedDeviceLists;
std::vector<unsigned int> sleepCalls;
std::vector<unsigned int> usleepCalls;
std::vector<std::string> openedPaths;
std::vector<int> closedFds;
std::deque<IoctlExpectation> ioctlExpectations;
std::vector<std::string> popenCommands;
std::deque<PopenExpectation> popenExpectations;
int libusbInitResult = LIBUSB_SUCCESS;
int libusbExitCalls = 0;
bool failI2cOpen = false;
int fakeI2cFd = 42;
int fakeI2cOpenErrno = ENOENT;

libusb_device* makeUsbDevice(int id, FakeUsbDeviceInfo info)
{
    auto* device = reinterpret_cast<libusb_device*>(
        static_cast<uintptr_t>(0x1000 + id * 0x100));
    fakeUsbDevices.emplace(device, std::move(info));
    return device;
}

std::filesystem::path writeTempFile(std::string_view name,
                                    const std::vector<uint8_t>& data)
{
    const auto path =
        std::filesystem::temp_directory_path() / std::string(name);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    return path;
}

std::filesystem::path writeTempText(std::string_view name,
                                    std::string_view data)
{
    const auto path =
        std::filesystem::temp_directory_path() / std::string(name);
    std::ofstream output(path, std::ios::trunc);
    output << data;
    return path;
}

void pushDeviceListResponse(ssize_t result, std::vector<libusb_device*> devices)
{
    DeviceListResponse response;
    response.result = result;
    response.devices = std::move(devices);
    deviceListResponses.push_back(std::move(response));
}

void pushProbeSuccess()
{
    ioctlExpectations.push_back({I2C_SLAVE, 0});
    ioctlExpectations.push_back({I2C_SMBUS, 0});
}

void pushProbeFailureAtSlave()
{
    ioctlExpectations.push_back({I2C_SLAVE, -1, EIO});
}

void pushProbeFailureAtQuick()
{
    ioctlExpectations.push_back({I2C_SLAVE, 0});
    ioctlExpectations.push_back({I2C_SMBUS, -1, EIO});
}

class MCURecoveryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fakeUsbDevices.clear();
        for (auto& [config, ownership] : configOwnership)
        {
            delete[] ownership.altsettings;
            delete[] ownership.interfaces;
            delete config;
        }
        configOwnership.clear();
        deviceListResponses.clear();
        freedDeviceLists.clear();
        sleepCalls.clear();
        usleepCalls.clear();
        openedPaths.clear();
        closedFds.clear();
        ioctlExpectations.clear();
        popenCommands.clear();
        popenExpectations.clear();
        libusbInitResult = LIBUSB_SUCCESS;
        libusbExitCalls = 0;
        failI2cOpen = false;
        fakeI2cFd = 42;
        fakeI2cOpenErrno = ENOENT;
        test::mcu_fake_dbus::reset();
        test::mcu_fake_gpio::reset();
    }
};

} // namespace

extern "C" int __real_open(const char* path, int flags, ...);
extern "C" int __real_open64(const char* path, int flags, ...);
extern "C" int __real_close(int fd);

namespace
{

int wrapOpenCommon(const char* path, int flags, bool useOpen64, va_list args)
{
    constexpr std::string_view fakeI2cPrefix = "/dev/i2c-";
    if (std::strncmp(path, fakeI2cPrefix.data(), fakeI2cPrefix.size()) != 0)
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
    if (failI2cOpen)
    {
        errno = fakeI2cOpenErrno;
        return -1;
    }
    return fakeI2cFd;
}

} // namespace

extern "C" int __wrap_open(const char* path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    const int result = wrapOpenCommon(path, flags, false, args);
    va_end(args);
    return result;
}

extern "C" int __wrap_open64(const char* path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    const int result = wrapOpenCommon(path, flags, true, args);
    va_end(args);
    return result;
}

extern "C" int __wrap_close(int fd)
{
    if (fd != fakeI2cFd)
    {
        return __real_close(fd);
    }
    closedFds.push_back(fd);
    return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    if (fd != fakeI2cFd)
    {
        return -1;
    }

    if (ioctlExpectations.empty())
    {
        ADD_FAILURE() << "Unexpected ioctl call for request " << request;
        errno = EIO;
        return -1;
    }
    auto expected = ioctlExpectations.front();
    ioctlExpectations.pop_front();
    EXPECT_EQ(request, expected.request);
    if (expected.ret < 0)
    {
        errno = expected.errnoValue;
    }
    return expected.ret;
}

extern "C" FILE* __wrap_popen(const char* command, const char* mode)
{
    EXPECT_STREQ(mode, "r");
    popenCommands.emplace_back(command);
    EXPECT_FALSE(popenExpectations.empty());
    auto expected = popenExpectations.front();
    popenExpectations.pop_front();
    EXPECT_NE(popenCommands.back().find(expected.needle), std::string::npos);
    if (expected.failOpen)
    {
        return nullptr;
    }

    FILE* file = std::tmpfile();
    EXPECT_NE(file, nullptr);
    const auto bytesWritten =
        std::fwrite(expected.output.data(), 1, expected.output.size(), file);
    EXPECT_EQ(bytesWritten, expected.output.size());
    std::rewind(file);
    return file;
}

extern "C" int __wrap_pclose(FILE* stream)
{
    return std::fclose(stream);
}

extern "C" unsigned int __wrap_sleep(unsigned int seconds)
{
    sleepCalls.push_back(seconds);
    return 0;
}

extern "C" int __wrap_usleep(useconds_t usec)
{
    usleepCalls.push_back(usec);
    return 0;
}

extern "C" int __wrap_libusb_init(libusb_context** context)
{
    if (libusbInitResult == LIBUSB_SUCCESS)
    {
        *context = reinterpret_cast<libusb_context*>(0x4444);
    }
    else
    {
        *context = nullptr;
    }
    return libusbInitResult;
}

extern "C" void __wrap_libusb_exit(libusb_context*)
{
    ++libusbExitCalls;
}

extern "C" int __wrap_libusb_get_port_numbers(libusb_device* device,
                                              uint8_t* ports,
                                              int portNumbersLen)
{
    auto& info = fakeUsbDevices.at(device);
    if (info.portNumbersResult != -999)
    {
        return info.portNumbersResult;
    }

    const auto bytesToCopy =
        std::min<int>(portNumbersLen, static_cast<int>(info.ports.size()));
    for (int i = 0; i < bytesToCopy; ++i)
    {
        ports[i] = info.ports[static_cast<size_t>(i)];
    }
    return bytesToCopy;
}

extern "C" uint8_t __wrap_libusb_get_bus_number(libusb_device* device)
{
    return fakeUsbDevices.at(device).bus;
}

extern "C" uint8_t __wrap_libusb_get_device_address(libusb_device* device)
{
    return fakeUsbDevices.at(device).address;
}

extern "C" ssize_t __wrap_libusb_get_device_list(libusb_context*,
                                                 libusb_device*** list)
{
    if (deviceListResponses.empty())
    {
        ADD_FAILURE() << "Unexpected libusb_get_device_list call";
        *list = nullptr;
        return LIBUSB_ERROR_IO;
    }
    auto response = deviceListResponses.front();
    deviceListResponses.pop_front();
    if (response.result < 0)
    {
        *list = nullptr;
        return response.result;
    }

    auto** allocated = new libusb_device*[response.devices.size() + 1]();
    for (size_t i = 0; i < response.devices.size(); ++i)
    {
        allocated[i] = response.devices[i];
    }
    allocated[response.devices.size()] = nullptr;
    *list = allocated;
    return static_cast<ssize_t>(response.devices.size());
}

extern "C" void __wrap_libusb_free_device_list(libusb_device** list,
                                               int unrefDevices)
{
    if (list != nullptr)
    {
        for (size_t i = 0; list[i] != nullptr; ++i)
        {
            if (unrefDevices != 0)
            {
                ++fakeUsbDevices.at(list[i]).freeListUnrefCalls;
            }
        }
        delete[] list;
    }
}

extern "C" int
    __wrap_libusb_get_device_descriptor(libusb_device* device,
                                        libusb_device_descriptor* desc)
{
    auto& info = fakeUsbDevices.at(device);
    if (info.descriptorResult != LIBUSB_SUCCESS)
    {
        return info.descriptorResult;
    }

    desc->idVendor = info.vendor;
    desc->idProduct = info.product;
    return LIBUSB_SUCCESS;
}

extern "C" int __wrap_libusb_get_active_config_descriptor(
    libusb_device* device, libusb_config_descriptor** config)
{
    auto& info = fakeUsbDevices.at(device);
    if (info.configResult != LIBUSB_SUCCESS)
    {
        return info.configResult;
    }

    auto* altsettings =
        new libusb_interface_descriptor[info.interfaceClasses.size()]();
    for (size_t i = 0; i < info.interfaceClasses.size(); ++i)
    {
        altsettings[i].bInterfaceClass = info.interfaceClasses[i];
    }

    auto* interfaces = new libusb_interface[1]();
    interfaces[0].altsetting = altsettings;
    interfaces[0].num_altsetting =
        static_cast<int>(info.interfaceClasses.size());

    auto* descriptor = new libusb_config_descriptor();
    descriptor->bNumInterfaces = 1;
    descriptor->interface = interfaces;

    configOwnership.emplace(descriptor,
                            ConfigOwnership{interfaces, altsettings});
    *config = descriptor;
    return LIBUSB_SUCCESS;
}

extern "C" void
    __wrap_libusb_free_config_descriptor(libusb_config_descriptor* config)
{
    auto it = configOwnership.find(config);
    if (it != configOwnership.end())
    {
        delete[] it->second.altsettings;
        delete[] it->second.interfaces;
        delete config;
        configOwnership.erase(it);
    }
}

extern "C" const char* __wrap_libusb_error_name(int)
{
    return "FAKE_LIBUSB_ERROR";
}

TEST_F(MCURecoveryTest, ParseJsonFileCoversUsbI2CAndSkippedEntries)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::parseJsonFile;

    EXPECT_THROW(parseJsonFile("/tmp/does-not-exist-mcu-config.json"),
                 std::runtime_error);

    auto jsonPath = writeTempText("mcu_recovery_config.json",
                                  R"({
  "Exposes": [
    {
      "Type": "OtherType",
      "Name": "skip"
    },
    {
      "Type": "MCURecovery",
      "Name": "usb-default",
      "ResetGpioName": "RESET_USB",
      "RecoveryGpioName": "REC_USB",
      "USBPort": "1-1",
      "ProductId": "0x7410"
    },
    {
      "Type": "MCURecovery",
      "Name": "i2c-dev",
      "Interface": "I2C",
      "ResetGpioName": "RESET_I2C",
      "RecoveryGpioName": "REC_I2C",
      "I2CBus": 7,
      "NormalI2CAddress": "0x10",
      "RecoveryI2CAddress": 33
    },
    {
      "Type": "MCURecovery",
      "Name": "bad-interface",
      "Interface": "SPI",
      "ResetGpioName": "X",
      "RecoveryGpioName": "Y"
    },
    {
      "Type": "MCURecovery",
      "Name": "bad-usb",
      "Interface": "USB",
      "ResetGpioName": "RESET_BAD",
      "RecoveryGpioName": "REC_BAD",
      "USBPort": "1-2",
      "ProductId": []
    },
    {
      "Type": "MCURecovery",
      "Name": "bad-i2c",
      "Interface": "I2C",
      "ResetGpioName": "RESET_BAD_I2C",
      "RecoveryGpioName": "REC_BAD_I2C",
      "I2CBus": "invalid",
      "NormalI2CAddress": "0x20",
      "RecoveryI2CAddress": "0x30"
    },
    {
      "Type": "MCURecovery",
      "Name": "bad-i2c-missing-normal",
      "Interface": "I2C",
      "ResetGpioName": "RESET_BAD_NORMAL",
      "RecoveryGpioName": "REC_BAD_NORMAL",
      "I2CBus": 8,
      "RecoveryI2CAddress": "0x31"
    },
    {
      "Type": "MCURecovery",
      "Name": "bad-i2c-missing-recovery",
      "Interface": "I2C",
      "ResetGpioName": "RESET_BAD_RECOVERY",
      "RecoveryGpioName": "REC_BAD_RECOVERY",
      "I2CBus": 8,
      "NormalI2CAddress": "0x21"
    }
  ]
})");

    const auto parsed = parseJsonFile(jsonPath.string());
    ASSERT_EQ(parsed.size(), 2u);
    ASSERT_TRUE(parsed.contains("usb-default"));
    ASSERT_TRUE(parsed.contains("i2c-dev"));
    EXPECT_EQ(parsed.at("usb-default").interfaceType,
              MCUInfo::InterfaceType::USB);
    EXPECT_EQ(parsed.at("usb-default").functionalPid, 0x7410);
    EXPECT_EQ(parsed.at("usb-default").usbPort, "1-1");
    EXPECT_EQ(parsed.at("i2c-dev").interfaceType, MCUInfo::InterfaceType::I2C);
    EXPECT_EQ(parsed.at("i2c-dev").i2cBus, 7);
    EXPECT_EQ(parsed.at("i2c-dev").normalI2cAddress, 0x10);
    EXPECT_EQ(parsed.at("i2c-dev").recoveryI2cAddress, 33);
}

TEST_F(MCURecoveryTest, DbusConfigQueriesCoverAdditionalPopulateEdgeCases)
{
    using nvidia::software::updater::GetSubTreeResponse;
    using nvidia::software::updater::Interfaces;

    const std::string iface = "xyz.openbmc_project.Configuration.MCURecovery";
    test::mcu_fake_dbus::setSubTree(GetSubTreeResponse{
        {"/xyz/openbmc_project/inventory/device4",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device5",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device6",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device7",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device8",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device9",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device10",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device11",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device12",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device13",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device14",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
    });

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device4", iface, "Interface", "SPI");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device4", iface, "ResetGpioName",
        "RESET4");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device4", iface, "RecoveryGpioName",
        "REC4");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device4", iface, "Target", "GPU");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device4", iface, "ChassisName", "HGX");

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device5", iface, "Interface", "USB");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device5", iface, "ResetGpioName",
        "RESET5");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device5", iface, "RecoveryGpioName",
        "REC5");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device5", iface, "USBPort", "5-1");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device5", iface, "ProductId", 0x7410);
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device5", iface, "ChassisName", "HGX");

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device6", iface, "Interface", "I2C");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device6", iface, "ResetGpioName",
        "RESET6");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device6", iface, "RecoveryGpioName",
        "REC6");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device6", iface, "I2CBus", 6);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device6", iface, "NormalI2CAddress",
        0x20);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device6", iface, "RecoveryI2CAddress",
        0x21);
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device6", iface, "Target", "GPU");

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device7", iface, "Interface", "USB");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device7", iface, "ResetGpioName",
        "RESET7");
    test::mcu_fake_dbus::throwProperty("/xyz/openbmc_project/inventory/device7",
                                       iface, "RecoveryGpioName");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device7", iface, "USBPort", "7-1");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device7", iface, "ProductId", 0x7410);

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device8", iface, "Interface", "USB");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device8", iface, "ResetGpioName",
        "RESET8");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device8", iface, "RecoveryGpioName",
        "REC8");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device8", iface, "USBPort", "8-1");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device8", iface, "ProductId", 70000);

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device9", iface, "Interface", "USB");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device9", iface, "ResetGpioName",
        "RESET9");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device9", iface, "RecoveryGpioName",
        "REC9");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device9", iface, "ProductId", 0x7411);

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device10", iface, "Interface", "USB");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device10", iface, "ResetGpioName",
        "RESET10");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device10", iface, "RecoveryGpioName",
        "REC10");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device10", iface, "USBPort", "10-1");

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device11", iface, "Interface", "I2C");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device11", iface, "ResetGpioName",
        "RESET11");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device11", iface, "RecoveryGpioName",
        "REC11");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device11", iface, "NormalI2CAddress",
        0x30);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device11", iface, "RecoveryI2CAddress",
        0x31);

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device12", iface, "Interface", "I2C");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device12", iface, "ResetGpioName",
        "RESET12");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device12", iface, "RecoveryGpioName",
        "REC12");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device12", iface, "I2CBus", 12);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device12", iface, "NormalI2CAddress",
        0x32);

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device13", iface, "Interface", "I2C");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device13", iface, "ResetGpioName",
        "RESET13");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device13", iface, "RecoveryGpioName",
        "REC13");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device13", iface, "I2CBus", 13);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device13", iface, "RecoveryI2CAddress",
        0x33);

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device14", iface, "Interface", "I2C");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device14", iface, "ResetGpioName",
        "RESET14");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device14", iface, "RecoveryGpioName",
        "REC14");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device14", iface, "I2CBus", 300);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device14", iface, "NormalI2CAddress",
        0x34);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device14", iface, "RecoveryI2CAddress",
        0x35);

    const auto all = mcu_recovery_manager::getAllMCUConfigFromDbus();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_TRUE(all.contains("device5"));
    EXPECT_TRUE(all.contains("device6"));

    const auto byTarget =
        mcu_recovery_manager::getMCUConfigByTargetFromDbus("GPU");
    ASSERT_EQ(byTarget.size(), 1u);
    EXPECT_TRUE(byTarget.contains("device6"));

    const auto byChassis =
        mcu_recovery_manager::getMCUConfigByChassisFromDbus("HGX");
    ASSERT_EQ(byChassis.size(), 1u);
    EXPECT_TRUE(byChassis.contains("device5"));
}

TEST_F(MCURecoveryTest, DbusConfigQueriesCoverPopulateAndFilters)
{
    using nvidia::software::updater::GetSubTreeResponse;
    using nvidia::software::updater::Interfaces;

    const std::string iface = "xyz.openbmc_project.Configuration.MCURecovery";
    test::mcu_fake_dbus::setSubTree(GetSubTreeResponse{
        {"/xyz/openbmc_project/inventory/device0",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device1",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device2",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
        {"/xyz/openbmc_project/inventory/device3",
         {{"xyz.openbmc_project.EntityManager", Interfaces{iface}}}},
    });

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device0", iface, "Interface", "USB");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device0", iface, "ResetGpioName",
        "RESET0");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device0", iface, "RecoveryGpioName",
        "REC0");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device0", iface, "USBPort", "1-2");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device0", iface, "ProductId", 0x7410);
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device0", iface, "Target", "GPU");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device0", iface, "ChassisName", "HGX");

    test::mcu_fake_dbus::throwProperty("/xyz/openbmc_project/inventory/device1",
                                       iface, "Interface");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device1", iface, "ResetGpioName",
        "RESET1");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device1", iface, "RecoveryGpioName",
        "REC1");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device1", iface, "USBPort", "1-3");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device1", iface, "ProductId", 0x7400);
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device1", iface, "Target", "GPU");

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device2", iface, "Interface", "I2C");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device2", iface, "ResetGpioName",
        "RESET2");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device2", iface, "RecoveryGpioName",
        "REC2");
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device2", iface, "I2CBus", 5);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device2", iface, "NormalI2CAddress",
        0x20);
    test::mcu_fake_dbus::setProperty<uint64_t>(
        "/xyz/openbmc_project/inventory/device2", iface, "RecoveryI2CAddress",
        0x21);
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device2", iface, "Target", "CPU");
    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device2", iface, "ChassisName",
        "Baseboard");

    test::mcu_fake_dbus::setProperty<std::string>(
        "/xyz/openbmc_project/inventory/device3", iface, "Interface", "USB");
    test::mcu_fake_dbus::throwProperty("/xyz/openbmc_project/inventory/device3",
                                       iface, "ResetGpioName");

    const auto all = mcu_recovery_manager::getAllMCUConfigFromDbus();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all.at("device0").usbPort, "1-2");
    EXPECT_EQ(all.at("device1").interfaceType,
              mcu_recovery_manager::MCUInfo::InterfaceType::USB);
    EXPECT_EQ(all.at("device2").i2cBus, 5);

    const auto byTarget =
        mcu_recovery_manager::getMCUConfigByTargetFromDbus("GPU");
    ASSERT_EQ(byTarget.size(), 2u);
    EXPECT_TRUE(byTarget.contains("device0"));
    EXPECT_TRUE(byTarget.contains("device1"));

    const auto byChassis =
        mcu_recovery_manager::getMCUConfigByChassisFromDbus("Baseboard");
    ASSERT_EQ(byChassis.size(), 1u);
    EXPECT_TRUE(byChassis.contains("device2"));
}

TEST_F(MCURecoveryTest,
       DbusConfigHandlesEmptyTreeMapperFailureAndCommandHelpers)
{
    using nvidia::software::updater::GetSubTreeResponse;

    test::mcu_fake_dbus::setSubTree(GetSubTreeResponse{});
    EXPECT_TRUE(mcu_recovery_manager::getAllMCUConfigFromDbus().empty());
    EXPECT_TRUE(
        mcu_recovery_manager::getMCUConfigByTargetFromDbus("GPU").empty());
    EXPECT_TRUE(
        mcu_recovery_manager::getMCUConfigByChassisFromDbus("HGX").empty());

    test::mcu_fake_dbus::failSubTree("fake mapper failure");
    EXPECT_TRUE(mcu_recovery_manager::getAllMCUConfigFromDbus().empty());

    EXPECT_EQ(mcu_recovery_manager::toHexString(0x2A), "002A");
    EXPECT_TRUE(mcu_recovery_manager::isCommandSuccessful(
        "Some text\nResponse status = Success.\n"));
    EXPECT_FALSE(mcu_recovery_manager::isCommandSuccessful(
        "Some text\nResponse status = Failure.\n"));

    popenExpectations.push_back(
        {.needle = "blhost one", .output = "cmd output\n", .failOpen = false});
    EXPECT_EQ(mcu_recovery_manager::executeCommand("blhost one"),
              "cmd output\n");

    popenExpectations.push_back(
        {.needle = "blhost two", .output = "", .failOpen = true});
    EXPECT_THROW(mcu_recovery_manager::executeCommand("blhost two"),
                 std::runtime_error);
}

TEST_F(MCURecoveryTest, InitializeProbeI2cAndGpioPathsAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo usbInfo{};
    usbInfo.device = "usb0";
    usbInfo.usbPort = "1-2";
    usbInfo.resetGpioName = "USB_RESET";
    usbInfo.recoveryGpioName = "USB_REC";
    usbInfo.functionalPid = 0x7410;
    usbInfo.interfaceType = MCUInfo::InterfaceType::USB;

    {
        MCURecoveryManager manager;
        EXPECT_TRUE(manager.initialize({{"usb0", usbInfo}}));
    }
    EXPECT_EQ(libusbExitCalls, 1);

    libusbInitResult = LIBUSB_ERROR_NO_DEVICE;
    {
        MCURecoveryManager manager;
        EXPECT_FALSE(manager.initialize({{"usb0", usbInfo}}));
    }

    MCUInfo gpioA{};
    gpioA.device = "gpioA";
    gpioA.resetGpioName = "MISSING_RESET";
    gpioA.recoveryGpioName = "REC_A";
    gpioA.interfaceType = MCUInfo::InterfaceType::I2C;
    MCUInfo gpioB{};
    gpioB.device = "gpioB";
    gpioB.resetGpioName = "RESET_B";
    gpioB.recoveryGpioName = "MISSING_REC";
    gpioB.interfaceType = MCUInfo::InterfaceType::I2C;
    MCUInfo gpioC{};
    gpioC.device = "gpioC";
    gpioC.resetGpioName = "RESET_C";
    gpioC.recoveryGpioName = "REC_C";
    gpioC.interfaceType = MCUInfo::InterfaceType::I2C;

    test::mcu_fake_gpio::lines["REC_A"] = {};
    test::mcu_fake_gpio::lines["RESET_B"] = {};
    test::mcu_fake_gpio::lines["RESET_C"] = {};
    auto throwingLine = test::mcu_fake_gpio::LineState{};
    throwingLine.throwOnRequest = true;
    test::mcu_fake_gpio::lines["REC_C"] = throwingLine;

    MCURecoveryManager gpioManager;
    gpioManager.mcuMap = {{"a", gpioA}, {"b", gpioB}, {"c", gpioC}};
    EXPECT_FALSE(gpioManager.initGpioLines());

    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c0";
    i2cInfo.resetGpioName = "RESET_OK";
    i2cInfo.recoveryGpioName = "REC_OK";
    i2cInfo.i2cBus = 7;
    i2cInfo.normalI2cAddress = 0x10;
    i2cInfo.recoveryI2cAddress = 0x20;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;
    MCURecoveryManager manager;
    manager.mcuMap = {{"dev", i2cInfo}};

    failI2cOpen = true;
    EXPECT_FALSE(manager.probeI2cAddress("dev", 0x10));
    failI2cOpen = false;

    pushProbeFailureAtSlave();
    EXPECT_FALSE(manager.probeI2cAddress("dev", 0x10));

    pushProbeFailureAtQuick();
    EXPECT_FALSE(manager.probeI2cAddress("dev", 0x10));

    pushProbeSuccess();
    EXPECT_TRUE(manager.probeI2cAddress("dev", 0x10));
    EXPECT_EQ(manager.getI2cTarget(7, 0x2A), "/dev/i2c-7,0x2A");

    pushProbeSuccess();
    pushProbeFailureAtQuick();
    EXPECT_TRUE(manager.updateI2cDevInfo("dev"));
    EXPECT_TRUE(manager.isHealthy("dev"));
    EXPECT_FALSE(manager.isInRecoveryMode("dev"));

    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    EXPECT_FALSE(manager.updateI2cDevInfo("dev"));
}

TEST_F(MCURecoveryTest, MessageRegistryAndHelperBranchesAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo gpioInfo{};
    gpioInfo.device = "gpio-dev";
    gpioInfo.resetGpioName = "GPIO_RESET";
    gpioInfo.recoveryGpioName = "GPIO_REC";
    gpioInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    test::mcu_fake_gpio::lines["GPIO_RESET"] = {};
    auto throwingRecovery = test::mcu_fake_gpio::LineState{};
    throwingRecovery.throwOnRequest = true;
    test::mcu_fake_gpio::lines["GPIO_REC"] = throwingRecovery;

    MCURecoveryManager initManager;
    EXPECT_FALSE(initManager.initialize(
        {{"gpio", gpioInfo}}, std::make_unique<MessageRegistry>(), true));

    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c-dev";
    i2cInfo.resetGpioName = "RESET_HELPER";
    i2cInfo.recoveryGpioName = "REC_HELPER";
    i2cInfo.i2cBus = 8;
    i2cInfo.normalI2cAddress = 0x10;
    i2cInfo.recoveryI2cAddress = 0x20;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    MCURecoveryManager manager;
    manager.mcuMap = {{"i2c", i2cInfo}};
    manager.messageRegistry = std::make_unique<MessageRegistry>();

    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    EXPECT_FALSE(manager.updateI2cDevInfo("i2c"));

    MCURecoveryManager emptyManager;
    emptyManager.enterRecoveryModeAll();

    MCURecoveryManager missingPinsManager;
    missingPinsManager.mcuMap = {{"i2c", i2cInfo}};
    missingPinsManager.mcuDevices["i2c"] = {};
    missingPinsManager.enterRecoveryModeAll();

    manager.handleRecoveryError("i2c");

    auto mismatchPath =
        writeTempFile("bad_mismatch_header.sb3",
                      {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
    EXPECT_FALSE(manager.isSB3FileValid(mismatchPath.string()));

    manager.performRecoveryFlow(mismatchPath.string(), false);
}

TEST_F(MCURecoveryTest,
       GpioInitializationAndRecoveryModeCoverSinglePinAndResetFailureBranches)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo throwingReset{};
    throwingReset.device = "throwing-reset";
    throwingReset.resetGpioName = "RESET_THROW";
    throwingReset.recoveryGpioName = "REC_OK";
    throwingReset.interfaceType = MCUInfo::InterfaceType::I2C;

    auto resetLine = test::mcu_fake_gpio::LineState{};
    resetLine.throwOnRequest = true;
    test::mcu_fake_gpio::lines["RESET_THROW"] = resetLine;
    test::mcu_fake_gpio::lines["REC_OK"] = {};

    MCURecoveryManager initManager;
    initManager.mcuMap = {{"dev", throwingReset}};
    EXPECT_FALSE(initManager.initGpioLines());

    MCUInfo singlePin{};
    singlePin.device = "single-pin";
    singlePin.resetGpioName = "RESET_SINGLE";
    singlePin.recoveryGpioName = "REC_SINGLE";
    singlePin.interfaceType = MCUInfo::InterfaceType::I2C;

    test::mcu_fake_gpio::lines["REC_SINGLE"] = {};

    MCURecoveryManager modeManager;
    modeManager.mcuMap = {{"single", singlePin}};
    modeManager.mcuDevices["single"].recoveryPin =
        gpiod::find_line("REC_SINGLE");

    modeManager.enterRecoveryMode("single");
    modeManager.exitRecoveryMode("single");
    modeManager.enterRecoveryModeAll();
}

TEST_F(MCURecoveryTest, RecoveryModeUsbStatusAndResetFlowsAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo usbInfo{};
    usbInfo.device = "usb0";
    usbInfo.usbPort = "2-3";
    usbInfo.resetGpioName = "RESET_USB";
    usbInfo.recoveryGpioName = "REC_USB";
    usbInfo.functionalPid = 0x7410;
    usbInfo.interfaceType = MCUInfo::InterfaceType::USB;
    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c0";
    i2cInfo.resetGpioName = "RESET_I2C";
    i2cInfo.recoveryGpioName = "REC_I2C";
    i2cInfo.i2cBus = 4;
    i2cInfo.normalI2cAddress = 0x10;
    i2cInfo.recoveryI2cAddress = 0x20;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    test::mcu_fake_gpio::lines["RESET_USB"] = {};
    test::mcu_fake_gpio::lines["REC_USB"] = {};
    test::mcu_fake_gpio::lines["RESET_I2C"] = {};
    test::mcu_fake_gpio::lines["REC_I2C"] = {};

    MCURecoveryManager manager;
    ASSERT_TRUE(manager.initialize({{"usb", usbInfo}, {"i2c", i2cInfo}},
                                   nullptr, true));

    manager.enterRecoveryMode("usb");
    manager.exitRecoveryMode("usb");
    manager.enterRecoveryModeAll();
    EXPECT_FALSE(usleepCalls.empty());
    EXPECT_FALSE(sleepCalls.empty());
    EXPECT_EQ(test::mcu_fake_gpio::lines["REC_USB"].setValues.front(), 0);
    EXPECT_EQ(test::mcu_fake_gpio::lines["RESET_USB"].setValues.front(), 0);

    manager.mcuDevices["missing"] = {};
    MCUInfo missingInfo{};
    missingInfo.device = "missing";
    missingInfo.interfaceType = MCUInfo::InterfaceType::I2C;
    manager.mcuMap["missing"] = missingInfo;
    manager.enterRecoveryMode("missing");
    manager.exitRecoveryMode("missing");

    FakeUsbDeviceInfo okInfo;
    okInfo.product = 0x7410;
    okInfo.bus = 2;
    okInfo.address = 9;
    okInfo.ports = {3};
    okInfo.interfaceClasses = {mcu_recovery_manager::LIBUSB_CLASS_MCTP};
    auto* okDevice = makeUsbDevice(1, okInfo);

    FakeUsbDeviceInfo hidInfo;
    hidInfo.product = 0x7401;
    hidInfo.bus = 5;
    hidInfo.address = 1;
    hidInfo.ports = {8};
    hidInfo.interfaceClasses = {LIBUSB_CLASS_HID};
    auto* hidDevice = makeUsbDevice(2, hidInfo);

    EXPECT_EQ(manager.getFullPortPath(okDevice), "2-3");

    fakeUsbDevices.at(okDevice).portNumbersResult = -5;
    EXPECT_TRUE(manager.getFullPortPath(okDevice).empty());
    fakeUsbDevices.at(okDevice).portNumbersResult = -999;

    libusb_config_descriptor config{};
    libusb_interface_descriptor altsettings[2]{};
    libusb_interface interfaces[1]{};
    altsettings[0].bInterfaceClass = mcu_recovery_manager::LIBUSB_CLASS_MCTP;
    altsettings[1].bInterfaceClass = LIBUSB_CLASS_HID;
    interfaces[0].altsetting = altsettings;
    interfaces[0].num_altsetting = 2;
    config.bNumInterfaces = 1;
    config.interface = interfaces;
    manager.updateDevHealth("usb", &config);
    EXPECT_TRUE(manager.mcuDevices["usb"].hasMctpClass);

    altsettings[0].bInterfaceClass = LIBUSB_CLASS_HID;
    altsettings[1].bInterfaceClass = 0xFF;
    manager.updateDevHealth("usb", &config);
    EXPECT_TRUE(manager.mcuDevices["usb"].inRecoveryMode);

    pushDeviceListResponse(LIBUSB_ERROR_NO_DEVICE, {});
    pushDeviceListResponse(1, {hidDevice});
    pushDeviceListResponse(1, {okDevice});
    EXPECT_TRUE(manager.updateUsbDevInfo("usb"));
    EXPECT_TRUE(manager.mcuDevices["usb"].hasMctpClass);
    EXPECT_EQ(manager.mcuDevices["usb"].curUsbDesc.idProduct, 0x7410);

    pushDeviceListResponse(1, {hidDevice});
    pushDeviceListResponse(1, {hidDevice});
    pushDeviceListResponse(1, {hidDevice});
    pushDeviceListResponse(1, {hidDevice});
    pushDeviceListResponse(1, {hidDevice});
    EXPECT_FALSE(manager.updateUsbDevInfo("usb"));

    manager.mcuDevices["usb"].recoveryPin = gpiod::find_line("REC_USB");
    manager.mcuDevices["usb"].resetPin = gpiod::find_line("RESET_USB");
    test::mcu_fake_gpio::lines["REC_USB"].throwOnRelease = true;
    manager.releaseGpioLines();

    manager.mcuDevices["i2c"].resetPin = gpiod::find_line("RESET_I2C");
    manager.mcuDevices["i2c"].recoveryPin = gpiod::find_line("REC_I2C");
    manager.mcuMap.erase("usb");
    manager.mcuDevices.erase("usb");
    manager.mcuMap.erase("missing");
    manager.mcuDevices.erase("missing");
    pushProbeSuccess();
    pushProbeFailureAtQuick();
    manager.performResetFlow();
    EXPECT_TRUE(test::mcu_fake_gpio::lines["RESET_I2C"].releaseCount >= 1);
    EXPECT_TRUE(test::mcu_fake_gpio::lines["REC_I2C"].releaseCount >= 1);
}

TEST_F(MCURecoveryTest, ValidationStatusAndRecoveryCommandsAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCURecoveryManager manager;

    EXPECT_FALSE(manager.isHealthy("missing"));
    EXPECT_FALSE(manager.isInRecoveryMode("missing"));

    auto badPath = writeTempFile("bad_sb.bin", {0x00, 0x01, 0x02, 0x03});
    auto shortPath = writeTempFile("short_sb.bin", {0x73, 0x62, 0x76});
    auto sb3Path = writeTempFile(
        "good_sb3.bin", {0x73, 0x62, 0x76, 0x33, 0x01, 0x00, 0x03, 0x00, 0xAA});
    auto sb4Path = writeTempFile(
        "good_sb4.bin", {0x02, 0x00, 0x00, 0x87, 0x00, 0x00, 0x00, 0x00});

    EXPECT_FALSE(manager.isSB3FileValid("/tmp/missing_sb_file.bin"));
    EXPECT_FALSE(manager.isSB3FileValid(shortPath.string()));
    EXPECT_FALSE(manager.isSB3FileValid(badPath.string()));
    EXPECT_TRUE(manager.isSB3FileValid(sb3Path.string()));
    EXPECT_TRUE(manager.isSB3FileValid(sb4Path.string()));

    EXPECT_FALSE(manager.isEncryptKeyEmpty("read-memory\nshort"));
    EXPECT_TRUE(manager.isEncryptKeyEmpty(
        "Successful response to command 'read-memory'\n"
        "00000000000000000000000000000000\n"
        "00000000000000000000000000000000\n"
        "00000000000000000000000000000000\n"));
    EXPECT_FALSE(manager.isEncryptKeyEmpty(
        "Successful response to command 'read-memory'\n"
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n"
        "00000000000000000000000000000000\n"
        "00000000000000000000000000000000\n"));

    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c-dev";
    i2cInfo.resetGpioName = "RESET_FLOW";
    i2cInfo.recoveryGpioName = "REC_FLOW";
    i2cInfo.i2cBus = 3;
    i2cInfo.normalI2cAddress = 0x10;
    i2cInfo.recoveryI2cAddress = 0x20;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;
    MCUInfo unknownInfo{};
    unknownInfo.device = "unknown-dev";
    unknownInfo.interfaceType = MCUInfo::InterfaceType::Unknown;
    manager.mcuMap = {{"i2c", i2cInfo}, {"unknown", unknownInfo}};
    test::mcu_fake_gpio::lines["RESET_FLOW"] = {};
    test::mcu_fake_gpio::lines["REC_FLOW"] = {};
    manager.mcuDevices["i2c"].resetPin = gpiod::find_line("RESET_FLOW");
    manager.mcuDevices["i2c"].recoveryPin = gpiod::find_line("REC_FLOW");

    popenExpectations.push_back(
        {.needle = "get-property security-state", .output = "Failure\n"});
    manager.performRecovery("i2c", sb3Path.string());

    popenExpectations.push_back(
        {.needle = "get-property security-state",
         .output = "Response status = Success\nUNSECURE\n"});
    popenExpectations.push_back(
        {.needle = "read-memory 0x1004160 48", .output = "Failure\n"});
    manager.performRecovery("i2c", sb3Path.string());

    popenExpectations.push_back(
        {.needle = "get-property security-state",
         .output = "Response status = Success\nUNSECURE\n"});
    popenExpectations.push_back(
        {.needle = "read-memory 0x1004160 48",
         .output = "Response status = Success\n"
                   "Successful response to command 'read-memory'\n"
                   "00000000000000000000000000000000\n"
                   "00000000000000000000000000000000\n"
                   "00000000000000000000000000000000\n"});
    manager.performRecovery("i2c", sb3Path.string());

    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "Response status = Success\n"});
    popenExpectations.push_back(
        {.needle = "receive-sb-file", .output = "Failure\n"});
    manager.performRecovery("i2c", sb3Path.string());

    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "Response status = Success\n"});
    popenExpectations.push_back(
        {.needle = "receive-sb-file", .output = "Response status = Success\n"});
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    manager.performRecovery("i2c", sb3Path.string());

    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "Response status = Success\n"});
    popenExpectations.push_back(
        {.needle = "receive-sb-file", .output = "Response status = Success\n"});
    pushProbeSuccess();
    pushProbeFailureAtQuick();
    manager.performRecovery("i2c", sb3Path.string());
    EXPECT_FALSE(popenCommands.empty());
    EXPECT_NE(popenCommands.back().find("/dev/i2c-3,0x20"), std::string::npos);

    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "",
                                 .failOpen = true});
    manager.performRecovery("i2c", sb3Path.string());

    manager.performRecovery("unknown", sb3Path.string());
    manager.mcuMap.erase("unknown");

    popenCommands.clear();
    manager.performRecoveryFlow(badPath.string(), false);
    EXPECT_TRUE(popenCommands.empty());

    pushProbeSuccess();
    pushProbeFailureAtQuick();
    manager.performRecoveryFlow(sb3Path.string(), false);
    EXPECT_TRUE(popenCommands.empty());

    pushProbeFailureAtQuick();
    pushProbeSuccess();
    pushProbeFailureAtQuick();
    pushProbeSuccess();
    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "Response status = Success\n"});
    popenExpectations.push_back(
        {.needle = "receive-sb-file", .output = "Response status = Success\n"});
    pushProbeSuccess();
    pushProbeFailureAtQuick();
    manager.performRecoveryFlow(sb3Path.string(), false);

    manager.showAllDeviceStatus();
}

TEST_F(MCURecoveryTest, HeaderAndUsbRetryHelpersAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCURecoveryManager manager;

    MCUInfo usbInfo{};
    usbInfo.device = "usb-helper";
    usbInfo.usbPort = "7-9";
    usbInfo.functionalPid = 0x7410;
    usbInfo.interfaceType = MCUInfo::InterfaceType::USB;

    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c-helper";
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    FakeUsbDeviceInfo usbGoodInfo;
    usbGoodInfo.product = 0x7410;
    usbGoodInfo.bus = 7;
    usbGoodInfo.address = 9;
    usbGoodInfo.ports = {9};
    auto* usbDevice = makeUsbDevice(1, usbGoodInfo);

    FakeUsbDeviceInfo usbBadDescriptorInfo = usbGoodInfo;
    usbBadDescriptorInfo.address = 10;
    usbBadDescriptorInfo.descriptorResult = LIBUSB_ERROR_IO;
    auto* usbBadDescriptor = makeUsbDevice(2, usbBadDescriptorInfo);

    FakeUsbDeviceInfo usbBadConfigInfo = usbGoodInfo;
    usbBadConfigInfo.address = 11;
    usbBadConfigInfo.configResult = LIBUSB_ERROR_IO;
    auto* usbBadConfig = makeUsbDevice(3, usbBadConfigInfo);

    manager.mcuMap = {{"usb", usbInfo}, {"i2c", i2cInfo}};
    manager.mcuDevices["usb"].curUsbDevice = usbDevice;
    manager.mcuDevices["usb"].curUsbDesc.idVendor =
        mcu_recovery_manager::nvdaVendorId;
    manager.mcuDevices["usb"].curUsbDesc.idProduct = 0x7410;
    manager.mcuDevices["usb"].hasMctpClass = false;
    manager.mcuDevices["usb"].inRecoveryMode = true;
    manager.mcuDevices["i2c"].i2cHealthy = true;

    EXPECT_TRUE(manager.isHealthy("usb"));
    manager.mcuDevices["usb"].hasMctpClass = true;
    EXPECT_TRUE(manager.isHealthy("usb"));
    EXPECT_TRUE(manager.isInRecoveryMode("usb"));
    EXPECT_TRUE(manager.isDeviceProvisioned("usb"));
    manager.mcuDevices["usb"].curUsbDesc.idVendor = 0x1234;
    EXPECT_FALSE(manager.isDeviceProvisioned("usb"));
    EXPECT_EQ(manager.getBusNumber("usb"), 7);
    EXPECT_EQ(manager.getDeviceNumber("usb"), 9);
    EXPECT_TRUE(manager.isDeviceProvisioned("i2c"));

    deviceListResponses.clear();
    pushDeviceListResponse(1, {usbBadDescriptor});
    pushDeviceListResponse(1, {usbBadConfig});
    pushDeviceListResponse(1, {usbDevice});
    EXPECT_TRUE(manager.updateUsbDevInfo("usb"));
    EXPECT_EQ(manager.mcuDevices["usb"].curUsbDesc.idProduct, 0x7410);
}

TEST_F(MCURecoveryTest,
       RecoveryFlowForceUpdateAndProvisioningBranchesAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    auto validSb3 =
        writeTempFile("force_update.sb3",
                      {0x73, 0x62, 0x76, 0x33, 0x01, 0x00, 0x03, 0x00, 0xAA});

    MCUInfo usbInfo{};
    usbInfo.device = "usb-force";
    usbInfo.usbPort = "4-1";
    usbInfo.functionalPid = 0x7410;
    usbInfo.interfaceType = MCUInfo::InterfaceType::USB;

    MCURecoveryManager usbManager;
    usbManager.mcuMap = {{"usb", usbInfo}};
    FakeUsbDeviceInfo unprovisionedInfo;
    unprovisionedInfo.vendor = 0x1234;
    unprovisionedInfo.product = 0x7410;
    unprovisionedInfo.bus = 4;
    unprovisionedInfo.ports = {1};
    auto* unprovisioned = makeUsbDevice(1, unprovisionedInfo);
    pushDeviceListResponse(1, {unprovisioned});
    usbManager.performRecoveryFlow(validSb3.string(), false);
    EXPECT_TRUE(popenCommands.empty());

    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c-force";
    i2cInfo.resetGpioName = "RESET_FORCE";
    i2cInfo.recoveryGpioName = "REC_FORCE";
    i2cInfo.i2cBus = 5;
    i2cInfo.normalI2cAddress = 0x10;
    i2cInfo.recoveryI2cAddress = 0x20;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    MCURecoveryManager i2cManager;
    i2cManager.mcuMap = {{"i2c", i2cInfo}};
    test::mcu_fake_gpio::lines["RESET_FORCE"] = {};
    test::mcu_fake_gpio::lines["REC_FORCE"] = {};
    i2cManager.mcuDevices["i2c"].resetPin = gpiod::find_line("RESET_FORCE");
    i2cManager.mcuDevices["i2c"].recoveryPin = gpiod::find_line("REC_FORCE");

    pushProbeSuccess();
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    pushProbeSuccess();
    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "Response status = Success\n"});
    popenExpectations.push_back(
        {.needle = "receive-sb-file", .output = "Response status = Success\n"});
    pushProbeSuccess();
    pushProbeFailureAtQuick();
    i2cManager.performRecoveryFlow(validSb3.string(), true);
    EXPECT_FALSE(popenCommands.empty());
}

TEST_F(MCURecoveryTest, ResetFlowPersistentFailureAndStatusBranchesAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c-fail";
    i2cInfo.resetGpioName = "RESET_FAIL";
    i2cInfo.recoveryGpioName = "REC_FAIL";
    i2cInfo.i2cBus = 6;
    i2cInfo.normalI2cAddress = 0x30;
    i2cInfo.recoveryI2cAddress = 0x31;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    MCUInfo usbInfo{};
    usbInfo.device = "usb-unknown";
    usbInfo.usbPort = "9-1";
    usbInfo.functionalPid = 0x7410;
    usbInfo.interfaceType = MCUInfo::InterfaceType::USB;

    MCURecoveryManager manager;
    manager.mcuMap = {{"i2c", i2cInfo}, {"usb", usbInfo}};
    test::mcu_fake_gpio::lines["RESET_FAIL"] = {};
    test::mcu_fake_gpio::lines["REC_FAIL"] = {};
    manager.mcuDevices["i2c"].resetPin = gpiod::find_line("RESET_FAIL");
    manager.mcuDevices["i2c"].recoveryPin = gpiod::find_line("REC_FAIL");
    manager.mcuDevices["i2c"].i2cHealthy = false;
    manager.mcuDevices["i2c"].inRecoveryMode = true;
    manager.mcuDevices["usb"].curUsbDesc.idProduct = 0x9999;

    manager.showAllDeviceStatus();

    manager.mcuMap.erase("usb");
    manager.mcuDevices.erase("usb");
    for (int i = 0; i < 10; ++i)
    {
        pushProbeFailureAtQuick();
        pushProbeFailureAtQuick();
    }
    manager.performResetFlow();
    EXPECT_FALSE(test::mcu_fake_gpio::lines["RESET_FAIL"].setValues.empty());
}

TEST_F(MCURecoveryTest, UpdateAllDeviceInfoAndUnknownStatusBranchesAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo first{};
    first.device = "i2c-a";
    first.resetGpioName = "RESET_A";
    first.recoveryGpioName = "REC_A";
    first.i2cBus = 11;
    first.normalI2cAddress = 0x10;
    first.recoveryI2cAddress = 0x20;
    first.interfaceType = MCUInfo::InterfaceType::I2C;

    MCUInfo second = first;
    second.device = "i2c-b";
    second.i2cBus = 12;
    second.normalI2cAddress = 0x11;
    second.recoveryI2cAddress = 0x21;

    MCURecoveryManager manager;
    manager.mcuMap = {{"a", first}, {"b", second}};

    pushProbeSuccess();
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();

    manager.updateAllDeviceInfo();
    EXPECT_TRUE(manager.isHealthy("a"));
    EXPECT_FALSE(manager.isHealthy("b"));
    EXPECT_FALSE(manager.isInRecoveryMode("b"));

    manager.showAllDeviceStatus();
}

TEST_F(MCURecoveryTest, UsbRecoveryRegistryBranchesAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo usbInfo{};
    usbInfo.device = "usb-registry";
    usbInfo.usbPort = "4-1";
    usbInfo.resetGpioName = "RESET_USB_REG";
    usbInfo.recoveryGpioName = "REC_USB_REG";
    usbInfo.functionalPid = 0x7410;
    usbInfo.interfaceType = MCUInfo::InterfaceType::USB;

    test::mcu_fake_gpio::lines["RESET_USB_REG"] = {};
    test::mcu_fake_gpio::lines["REC_USB_REG"] = {};

    FakeUsbDeviceInfo okInfo;
    okInfo.product = 0x7410;
    okInfo.bus = 4;
    okInfo.address = 2;
    okInfo.ports = {1};
    okInfo.interfaceClasses = {mcu_recovery_manager::LIBUSB_CLASS_MCTP};
    auto* okDevice = makeUsbDevice(10, okInfo);

    MCURecoveryManager manager;
    manager.mcuMap = {{"usb", usbInfo}};
    manager.messageRegistry = std::make_unique<MessageRegistry>();
    manager.mcuDevices["usb"].curUsbDevice = okDevice;
    manager.mcuDevices["usb"].curUsbDesc.idVendor =
        mcu_recovery_manager::nvdaVendorId;
    manager.mcuDevices["usb"].curUsbDesc.idProduct = 0x7410;
    manager.mcuDevices["usb"].resetPin = gpiod::find_line("RESET_USB_REG");
    manager.mcuDevices["usb"].recoveryPin = gpiod::find_line("REC_USB_REG");

    popenExpectations.push_back(
        {.needle = "get-property security-state", .output = "Failure\n"});
    manager.performRecovery("usb", "/tmp/usb-registry.sb3");

    popenExpectations.push_back(
        {.needle = "get-property security-state",
         .output = "Response status = Success\nUNSECURE\n"});
    popenExpectations.push_back(
        {.needle = "read-memory 0x1004160 48", .output = "Failure\n"});
    manager.performRecovery("usb", "/tmp/usb-registry.sb3");

    popenExpectations.push_back(
        {.needle = "get-property security-state",
         .output = "Response status = Success\nUNSECURE\n"});
    popenExpectations.push_back(
        {.needle = "read-memory 0x1004160 48",
         .output = "Response status = Success\n"
                   "Successful response to command 'read-memory'\n"
                   "00000000000000000000000000000000\n"
                   "00000000000000000000000000000000\n"
                   "00000000000000000000000000000000\n"});
    manager.performRecovery("usb", "/tmp/usb-registry.sb3");

    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "Response status = Success\n"});
    popenExpectations.push_back(
        {.needle = "receive-sb-file", .output = "Failure\n"});
    manager.performRecovery("usb", "/tmp/usb-registry.sb3");

    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "Response status = Success\n"});
    popenExpectations.push_back(
        {.needle = "receive-sb-file", .output = "Response status = Success\n"});
    pushDeviceListResponse(1, {okDevice});
    manager.performRecovery("usb", "/tmp/usb-registry.sb3");

    popenExpectations.push_back({.needle = "get-property security-state",
                                 .output = "",
                                 .failOpen = true});
    manager.performRecovery("usb", "/tmp/usb-registry.sb3");

    EXPECT_FALSE(popenCommands.empty());
    EXPECT_NE(popenCommands.back().find("--usb-bus-device 4:2"),
              std::string::npos);
    EXPECT_NE(popenCommands.back().find("--usb-id 0955:7410"),
              std::string::npos);
}

TEST_F(MCURecoveryTest, RecoveryFlowRegistryBranchesAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    auto validSb3 =
        writeTempFile("registry_flow.sb3",
                      {0x73, 0x62, 0x76, 0x33, 0x01, 0x00, 0x03, 0x00, 0xAA});

    MCUInfo unprovInfo{};
    unprovInfo.device = "usb-unprov";
    unprovInfo.usbPort = "6-1";
    unprovInfo.functionalPid = 0x7410;
    unprovInfo.interfaceType = MCUInfo::InterfaceType::USB;

    FakeUsbDeviceInfo unprovDeviceInfo;
    unprovDeviceInfo.vendor = 0x1234;
    unprovDeviceInfo.product = 0x7410;
    unprovDeviceInfo.bus = 6;
    unprovDeviceInfo.ports = {1};
    auto* unprovDevice = makeUsbDevice(20, unprovDeviceInfo);

    MCURecoveryManager unprovManager;
    unprovManager.mcuMap = {{"usb", unprovInfo}};
    unprovManager.messageRegistry = std::make_unique<MessageRegistry>();
    pushDeviceListResponse(1, {unprovDevice});
    unprovManager.performRecoveryFlow(validSb3.string(), false);

    MCUInfo healthyInfo{};
    healthyInfo.device = "usb-healthy";
    healthyInfo.usbPort = "7-2";
    healthyInfo.functionalPid = 0x7410;
    healthyInfo.interfaceType = MCUInfo::InterfaceType::USB;

    FakeUsbDeviceInfo healthyDeviceInfo;
    healthyDeviceInfo.product = 0x7410;
    healthyDeviceInfo.bus = 7;
    healthyDeviceInfo.ports = {2};
    healthyDeviceInfo.interfaceClasses = {
        mcu_recovery_manager::LIBUSB_CLASS_MCTP};
    auto* healthyDevice = makeUsbDevice(21, healthyDeviceInfo);

    MCURecoveryManager healthyManager;
    healthyManager.mcuMap = {{"usb", healthyInfo}};
    healthyManager.messageRegistry = std::make_unique<MessageRegistry>();
    pushDeviceListResponse(1, {healthyDevice});
    healthyManager.performRecoveryFlow(validSb3.string(), false);
}

TEST_F(MCURecoveryTest, UsbPathFormattingAndRegistryRetryBranchesAreCovered)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo usbInfo{};
    usbInfo.device = "usb-retry";
    usbInfo.usbPort = "2-9";
    usbInfo.functionalPid = 0x7410;
    usbInfo.interfaceType = MCUInfo::InterfaceType::USB;

    FakeUsbDeviceInfo multiHopInfo;
    multiHopInfo.product = 0x7410;
    multiHopInfo.bus = 2;
    multiHopInfo.ports = {3, 5};
    auto* multiHopDevice = makeUsbDevice(30, multiHopInfo);

    MCURecoveryManager manager;
    manager.mcuMap = {{"usb", usbInfo}};
    manager.messageRegistry = std::make_unique<MessageRegistry>();

    EXPECT_EQ(manager.getFullPortPath(multiHopDevice), "2-3.5");

    for (int i = 0; i < 5; ++i)
    {
        pushDeviceListResponse(1, {multiHopDevice});
    }
    EXPECT_FALSE(manager.updateUsbDevInfo("usb"));
}

TEST_F(MCURecoveryTest, ResetFlowHandlesMissingPinsWhileHealthyDeviceRecovers)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    MCUInfo i2cInfo{};
    i2cInfo.device = "reset-no-pins";
    i2cInfo.i2cBus = 13;
    i2cInfo.normalI2cAddress = 0x10;
    i2cInfo.recoveryI2cAddress = 0x20;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    MCURecoveryManager manager;
    manager.mcuMap = {{"i2c", i2cInfo}};
    manager.mcuDevices["i2c"] = {};

    pushProbeSuccess();
    pushProbeFailureAtQuick();

    manager.performResetFlow();
    EXPECT_TRUE(manager.isHealthy("i2c"));
}

TEST_F(MCURecoveryTest, RecoveryFlowCoversUpdateFailuresBeforeAndAfterReset)
{
    using mcu_recovery_manager::MCUInfo;
    using mcu_recovery_manager::MCURecoveryManager;

    auto validSb3 =
        writeTempFile("flow_update_failures.sb3",
                      {0x73, 0x62, 0x76, 0x33, 0x01, 0x00, 0x03, 0x00, 0xAA});

    MCUInfo i2cInfo{};
    i2cInfo.device = "i2c-flow";
    i2cInfo.resetGpioName = "RESET_FLOW_FAIL";
    i2cInfo.recoveryGpioName = "REC_FLOW_FAIL";
    i2cInfo.i2cBus = 14;
    i2cInfo.normalI2cAddress = 0x10;
    i2cInfo.recoveryI2cAddress = 0x20;
    i2cInfo.interfaceType = MCUInfo::InterfaceType::I2C;

    test::mcu_fake_gpio::lines["RESET_FLOW_FAIL"] = {};
    test::mcu_fake_gpio::lines["REC_FLOW_FAIL"] = {};

    MCURecoveryManager earlyFailManager;
    earlyFailManager.mcuMap = {{"i2c", i2cInfo}};
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    earlyFailManager.performRecoveryFlow(validSb3.string(), false);

    MCURecoveryManager forceUpdateManager;
    forceUpdateManager.mcuMap = {{"i2c", i2cInfo}};
    forceUpdateManager.mcuDevices["i2c"].resetPin =
        gpiod::find_line("RESET_FLOW_FAIL");
    forceUpdateManager.mcuDevices["i2c"].recoveryPin =
        gpiod::find_line("REC_FLOW_FAIL");
    pushProbeSuccess();
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    forceUpdateManager.performRecoveryFlow(validSb3.string(), true);

    MCURecoveryManager unhealthyManager;
    unhealthyManager.mcuMap = {{"i2c", i2cInfo}};
    unhealthyManager.mcuDevices["i2c"].resetPin =
        gpiod::find_line("RESET_FLOW_FAIL");
    unhealthyManager.mcuDevices["i2c"].recoveryPin =
        gpiod::find_line("REC_FLOW_FAIL");
    pushProbeFailureAtQuick();
    pushProbeSuccess();
    pushProbeFailureAtQuick();
    pushProbeFailureAtQuick();
    unhealthyManager.performRecoveryFlow(validSb3.string(), false);
}
