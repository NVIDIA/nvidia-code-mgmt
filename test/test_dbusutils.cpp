/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"

// gtest FAIL() macro conflicts with phosphor-logging elog-errors.hpp FAIL type
#undef FAIL

#include "../src/dbusutils.hpp"

#include <unistd.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include <cstring>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using namespace nvidia::software::updater;

extern "C" int __wrap_usleep(useconds_t)
{
    return 0;
}

class DBUSUtilsTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    DBUSUtils dbusUtils{bus};
};

// ========================== createVersionID ==========================

TEST_F(DBUSUtilsTest, CreateVersionIDValid)
{
    auto id = dbusUtils.createVersionID("PSU", "1.1");
    EXPECT_EQ(id, "PSU_11");
}

TEST_F(DBUSUtilsTest, CreateVersionIDStripsMultipleDots)
{
    auto id = dbusUtils.createVersionID("GPU", "2.3.4");
    EXPECT_EQ(id, "GPU_234");
}

TEST_F(DBUSUtilsTest, CreateVersionIDNoDots)
{
    auto id = dbusUtils.createVersionID("CPLD", "100");
    EXPECT_EQ(id, "CPLD_100");
}

TEST_F(DBUSUtilsTest, CreateVersionIDEmptyVersion)
{
    auto id = dbusUtils.createVersionID("PSU", "");
    EXPECT_TRUE(id.empty());
}

TEST_F(DBUSUtilsTest, CreateVersionIDEmptyUpdaterName)
{
    auto id = dbusUtils.createVersionID("", "1.0");
    EXPECT_TRUE(id.empty());
}

TEST_F(DBUSUtilsTest, CreateVersionIDBothEmpty)
{
    auto id = dbusUtils.createVersionID("", "");
    EXPECT_TRUE(id.empty());
}

TEST_F(DBUSUtilsTest, CreateVersionIDComplexName)
{
    // createVersionID strips '.' characters from the concatenation
    auto id =
        dbusUtils.createVersionID("HGX_FW_ERoT_GPU_SPI", "0.02.0084.0000");
    EXPECT_EQ(id, "HGX_FW_ERoT_GPU_SPI_00200840000");
}

// ========================== getHostPwrStatus ==========================

TEST_F(DBUSUtilsTest, GetHostPwrStatusReturnsEmptyOnError)
{
    // With mock bus, the D-Bus call will fail - should return empty
    auto status = dbusUtils.getHostPwrStatus();
    EXPECT_TRUE(status.empty());
}

// ========================== getManagedObjects ==========================

TEST_F(DBUSUtilsTest, GetManagedObjectsReturnsEmptyOnError)
{
    // With mock bus, the D-Bus call will fail - should return empty
    auto objects = dbusUtils.getManagedObjects("test.service", "/test/path");
    EXPECT_TRUE(objects.empty());
}

// ========================== createLog ==========================

TEST_F(DBUSUtilsTest, CreateLogNoThrow)
{
    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = "test";
    Level level = Level::Informational;
    EXPECT_NO_THROW(dbusUtils.createLog("test.message", addData, level));
}

// ========================== createMessageRegistryResourceErrors
// ==========================

TEST_F(DBUSUtilsTest, CreateMessageRegistryResourceErrorsNoThrow)
{
    EXPECT_NO_THROW(dbusUtils.createMessageRegistryResourceErrors(
        "Update.1.0.TransferFailed", "TestDevice", "Test error",
        "Retry the operation"));
}

TEST_F(DBUSUtilsTest, CreateMessageRegistryEmptyResolution)
{
    EXPECT_NO_THROW(dbusUtils.createMessageRegistryResourceErrors(
        "Update.1.0.TransferFailed", "TestDevice", "Test error", ""));
}

// ========================== Low-level SdBusMock for D-Bus calls
// ========================== Following the pldm pattern: mock sd_bus_call +
// sd_bus_message_read_basic

class DBUSUtilsMockBusTest : public testing::Test
{
  protected:
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    DBUSUtils dbusUtils{bus};
};

TEST_F(DBUSUtilsMockBusTest, GetHostPwrStatusSuccess)
{
    // Mock bus.call() to succeed
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });

    // Mock reading a variant<string> response:
    // enter variant container
    EXPECT_CALL(sdbusMock,
                sd_bus_message_enter_container(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(0));

    // Read the string value
    static const char* pwrState =
        "xyz.openbmc_project.State.Chassis.PowerState.On";
    EXPECT_CALL(sdbusMock,
                sd_bus_message_read_basic(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, void* output) {
            if (type == 's')
            {
                *static_cast<const char**>(output) = pwrState;
            }
            return 0;
        });

    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(testing::Return(0));

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(testing::Return(1));

    // Match only the string alternative in DBUSUtils::Value. Returning true for
    // every alternative makes sdbusplus decode the variant as bool first.
    EXPECT_CALL(sdbusMock,
                sd_bus_message_verify_type(nullptr, testing::_, testing::_))
        .WillRepeatedly([](sd_bus_message*, char type, const char* contents) {
            return type == SD_BUS_TYPE_VARIANT && contents != nullptr &&
                   std::strcmp(contents, "s") == 0;
        });

    auto status = dbusUtils.getHostPwrStatus();
    EXPECT_EQ(status, pwrState);
}

TEST_F(DBUSUtilsMockBusTest, GetManagedObjectsSuccess)
{
    // Mock bus.call() to succeed with empty response
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });

    // Mock reading an empty array (enter array, immediately at_end)
    EXPECT_CALL(sdbusMock,
                sd_bus_message_enter_container(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(0));

    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(testing::Return(1));

    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(testing::Return(0));

    auto objects = dbusUtils.getManagedObjects("test.service", "/test");
    EXPECT_TRUE(objects.empty());
}

TEST_F(DBUSUtilsMockBusTest, GetManagedObjectsCallFails)
{
    // Mock bus.call() to throw
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce(testing::Return(-ENOENT));

    auto objects = dbusUtils.getManagedObjects("test.service", "/test");
    EXPECT_TRUE(objects.empty());
}

TEST_F(DBUSUtilsMockBusTest, GetServicesThrowsWhenMapperResponseIsEmpty)
{
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });

    EXPECT_CALL(sdbusMock,
                sd_bus_message_enter_container(nullptr, testing::_, testing::_))
        .WillRepeatedly(testing::Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, testing::_))
        .WillRepeatedly(testing::Return(1));
    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillRepeatedly(testing::Return(0));

    EXPECT_THROW(dbusUtils.getServices("/test/path", "test.Interface"),
                 std::runtime_error);
}

TEST_F(DBUSUtilsMockBusTest, GetServicesThrowsAfterRetryingMapperFailures)
{
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .Times(10)
        .WillRepeatedly(testing::Return(-ENOENT));

    EXPECT_THROW(dbusUtils.getServices("/test/path", "test.Interface"),
                 std::runtime_error);
}

// ========================== controlSystemUnit via public wrappers
// ==========================

TEST_F(DBUSUtilsMockBusTest, RestartSystemUnitNoThrow)
{
    // restartSystemUnit calls controlSystemUnit("RestartUnit")
    // With mock bus, call_noreply will use sd_bus_send which is mocked
    EXPECT_NO_THROW(dbusUtils.restartSystemUnit("test.service"));
}

TEST_F(DBUSUtilsMockBusTest, RestartSystemUnitHandlesSendFailure)
{
    EXPECT_CALL(sdbusMock, sd_bus_send(nullptr, nullptr, testing::_))
        .WillRepeatedly(testing::Return(-EIO));

    EXPECT_NO_THROW(dbusUtils.restartSystemUnit("test.service"));
}

TEST_F(DBUSUtilsMockBusTest, StartSystemUnitNoThrow)
{
    EXPECT_NO_THROW(dbusUtils.startSystemUnit("test.service"));
}

TEST_F(DBUSUtilsMockBusTest, StartSystemUnitHandlesSendFailure)
{
    EXPECT_CALL(sdbusMock, sd_bus_send(nullptr, nullptr, testing::_))
        .WillRepeatedly(testing::Return(-EIO));

    EXPECT_NO_THROW(dbusUtils.startSystemUnit("test.service"));
}

// ========================== createLog ==========================

TEST_F(DBUSUtilsMockBusTest, CreateLogSuccessPath)
{
    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = "test.message";
    Level level = Level::Informational;
    EXPECT_NO_THROW(dbusUtils.createLog("test.message", addData, level));
}

TEST_F(DBUSUtilsMockBusTest, CreateLogCriticalLevel)
{
    std::map<std::string, std::string> addData;
    addData["key"] = "value";
    Level level = Level::Critical;
    EXPECT_NO_THROW(dbusUtils.createLog("error.message", addData, level));
}

TEST_F(DBUSUtilsMockBusTest, CreateLogHandlesSendFailure)
{
    EXPECT_CALL(sdbusMock, sd_bus_send(nullptr, nullptr, testing::_))
        .WillRepeatedly(testing::Return(-EIO));

    std::map<std::string, std::string> addData;
    addData["key"] = "value";
    Level level = Level::Informational;
    EXPECT_NO_THROW(dbusUtils.createLog("error.message", addData, level));
}

// ========================== getinventoryPath ==========================

TEST_F(DBUSUtilsMockBusTest, GetInventoryPathCallFails)
{
    // sd_bus_call returns error -> SdBusError thrown -> propagated
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce(testing::Return(-ENOENT));

    // getinventoryPath does NOT catch exceptions - it propagates
    EXPECT_THROW(
        dbusUtils.getinventoryPath("xyz.openbmc_project.Inventory.Item"),
        std::exception);
}

// ========================== getPropertyImpl ==========================

TEST_F(DBUSUtilsMockBusTest, GetPropertyImplCallFails)
{
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce(testing::Return(-ENOENT));

    EXPECT_THROW(dbusUtils.getPropertyImpl("test.service", "/test/path",
                                           "test.Interface", "TestProperty"),
                 std::runtime_error);
}

// ========================== getSoftwareObjects ==========================

TEST_F(DBUSUtilsMockBusTest, GetSoftwareObjectsCallFails)
{
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce(testing::Return(-ENOENT));

    EXPECT_THROW(dbusUtils.getSoftwareObjects(), std::exception);
}

// ========================== findSoftwareObject ==========================

TEST_F(DBUSUtilsMockBusTest, FindSoftwareObjectCallFails)
{
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce(testing::Return(-ENOENT));

    std::string path = "/xyz/openbmc_project/software/test";
    // getSoftwareObjects will throw, findSoftwareObject propagates
    EXPECT_THROW(dbusUtils.findSoftwareObject(path), std::exception);
}

// ========================== getProperty template ==========================

TEST_F(DBUSUtilsMockBusTest, GetPropertyStringFails)
{
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                       testing::_))
        .WillOnce(testing::Return(-ENOENT));

    EXPECT_THROW(
        dbusUtils.getProperty<std::string>("test.service", "/test/path",
                                           "test.Interface", "TestProp"),
        std::exception);
}

// ========================== createMessageRegistryResourceErrors paths
// ===========

TEST_F(DBUSUtilsMockBusTest, CreateMsgRegistryWithResolution)
{
    EXPECT_NO_THROW(dbusUtils.createMessageRegistryResourceErrors(
        "Update.1.0.TransferFailed", "GPU_0", "Firmware update failed",
        "Retry the operation"));
}

TEST_F(DBUSUtilsMockBusTest, CreateMsgRegistryEmptyResolution)
{
    EXPECT_NO_THROW(dbusUtils.createMessageRegistryResourceErrors(
        "Update.1.0.TransferFailed", "GPU_0", "Firmware update failed", ""));
}
