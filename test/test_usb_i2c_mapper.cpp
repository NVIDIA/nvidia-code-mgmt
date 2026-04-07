#include "../recovery_tool/ocp_recovery_tool/usb_i2c_mapper.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace test_usb_i2c_mapper
{

inline std::string fakeUsbDevicePath;

} // namespace test_usb_i2c_mapper

#define usbDevicePath test_usb_i2c_mapper::fakeUsbDevicePath
#include "../recovery_tool/ocp_recovery_tool/usb_i2c_mapper.cpp"
#undef usbDevicePath

namespace
{

class UsbI2CMapperTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tempRoot = std::filesystem::temp_directory_path() /
                   std::filesystem::path("usb_i2c_mapper_test");
        tempRoot += std::to_string(::getpid());
        tempRoot += "_";
        tempRoot += std::to_string(counter++);

        std::filesystem::create_directories(tempRoot);
        test_usb_i2c_mapper::fakeUsbDevicePath = tempRoot.string() + "/";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(tempRoot, ec);
    }

    static inline int counter = 0;
    std::filesystem::path tempRoot;
};

TEST_F(UsbI2CMapperTest, MissingUsbPortReturnsMinusOne)
{
    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("9-9", true), -1);
}

TEST_F(UsbI2CMapperTest, RecursiveSearchSkipsInvalidDirectoryAndFindsBus)
{
    const auto base = tempRoot / "1-1";
    std::filesystem::create_directories(base / "nested-a" / "i2c-not-a-number");
    std::filesystem::create_directories(base / "nested-b" / "i2c-17");

    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("1-1", true), 17);
}

TEST_F(UsbI2CMapperTest, RecursiveSearchFindsDirectBusDirectory)
{
    std::filesystem::create_directories(tempRoot / "2-1" / "i2c-42");

    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("2-1", false), 42);
}

TEST_F(UsbI2CMapperTest, InterfaceFallbackFindsBusInsideSymlinkedInterfacePath)
{
    const auto base = tempRoot / "3-1";
    const auto interfaceTarget = tempRoot / "3-1-interface-target";
    std::filesystem::create_directories(base);
    std::filesystem::create_directories(interfaceTarget / "i2c-23");
    std::filesystem::create_directory_symlink(interfaceTarget,
                                              base / "3-1:1.0");

    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("3-1", true), 23);
}

TEST_F(UsbI2CMapperTest, ReturnsMinusOneWhenNoValidI2CBusExists)
{
    const auto base = tempRoot / "4-1";
    std::filesystem::create_directories(base / "ignored");
    std::filesystem::create_directories(base / "i2c-invalid");
    std::filesystem::create_directories(base / "4-1:1.0" / "i2c-also-invalid");

    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("4-1", true), -1);
}

TEST_F(UsbI2CMapperTest, MissingUsbPortQuietModeReturnsMinusOne)
{
    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("9-8", false), -1);
}

TEST_F(UsbI2CMapperTest,
       QuietInterfaceFallbackSkipsFilesAndInvalidInterfaceEntries)
{
    const auto base = tempRoot / "5-1";
    const auto interfaceTarget = tempRoot / "5-1-interface-target";
    std::filesystem::create_directories(base);
    std::filesystem::create_directories(interfaceTarget / "ignored-dir");
    std::filesystem::create_directories(interfaceTarget / "i2c-invalid");
    std::filesystem::create_directories(interfaceTarget / "i2c-88");
    std::filesystem::create_directory_symlink(interfaceTarget,
                                              base / "5-1:1.0");

    std::ofstream(base / "root-marker.txt") << "root";
    std::ofstream(interfaceTarget / "leaf.txt") << "leaf";

    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("5-1", false), 88);
}

TEST_F(UsbI2CMapperTest, QuietFailurePathReturnsMinusOneWithoutVerboseLogging)
{
    const auto base = tempRoot / "6-1";
    const auto interfaceTarget = tempRoot / "6-1-interface-target";
    std::filesystem::create_directories(base / "ignored");
    std::filesystem::create_directories(interfaceTarget / "not-i2c");
    std::filesystem::create_directories(interfaceTarget / "i2c-invalid");
    std::filesystem::create_directory_symlink(interfaceTarget,
                                              base / "6-1:1.0");

    std::ofstream(base / "root-file.txt") << "root";
    std::ofstream(interfaceTarget / "leaf.txt") << "leaf";

    EXPECT_EQ(recovery_tool::usb_i2c::getI2CBusFromUSBPort("6-1", false), -1);
}

} // namespace
