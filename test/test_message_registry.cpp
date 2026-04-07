/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"

// gtest FAIL() macro conflicts with phosphor-logging elog-errors.hpp FAIL type
#undef FAIL

#include "../recovery_tool/common/message_registry.hpp"

#include <sdbusplus/test/sdbus_mock.hpp>

#include "../recovery_tool/common/message_registry.cpp"

using ::testing::NiceMock;
using ::testing::Return;

class MessageRegistryTest : public testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
    MessageRegistry registry{bus};
};

// ========================== createMessageRegistry ==========================

TEST_F(MessageRegistryTest, RecoveryStarted)
{
    EXPECT_NO_THROW(registry.createMessageRegistry(recoveryStarted, "GPU_0"));
}

TEST_F(MessageRegistryTest, FirmwareNotInRecovery)
{
    EXPECT_NO_THROW(
        registry.createMessageRegistry(firmwareNotInRecovery, "ERoT_0"));
}

TEST_F(MessageRegistryTest, RecoverySuccessful)
{
    EXPECT_NO_THROW(
        registry.createMessageRegistry(recoverySuccessful, "GPU_1"));
}

TEST_F(MessageRegistryTest, EnterDOTRecovery)
{
    EXPECT_NO_THROW(registry.createMessageRegistry(enterDOTRecovery, "MCU_0"));
}

TEST_F(MessageRegistryTest, UnrecognizedMessageID)
{
    EXPECT_NO_THROW(
        registry.createMessageRegistry("Unknown.Message.ID", "device"));
}

// ========================== getMessage ==========================

TEST_F(MessageRegistryTest, GetMessageGlacierRecoveryKnownCode)
{
    auto msg = registry.getMessage(RecoveryProtocol::GlacierRecovery,
                                   deviceRecoveryFailed);
    EXPECT_TRUE(msg.has_value());
}

TEST_F(MessageRegistryTest, GetMessageOCPRecoveryKnownCode)
{
    // OCPRecovery may not have deviceRecoveryFailed; test common codes
    auto msg =
        registry.getMessage(RecoveryProtocol::OCPRecovery, deviceNotResponding);
    // Result depends on mapping table; just verify no crash
    (void)msg;
}

TEST_F(MessageRegistryTest, GetMessageMCURecoveryKnownCode)
{
    auto msg = registry.getMessage(RecoveryProtocol::MCURecovery,
                                   deviceRecoveryFailed);
    EXPECT_TRUE(msg.has_value());
}

TEST_F(MessageRegistryTest, GetMessageUSBRCMRecoveryKnownCode)
{
    auto msg = registry.getMessage(RecoveryProtocol::USBRCMRecovery,
                                   deviceRecoveryFailed);
    EXPECT_TRUE(msg.has_value());
}

TEST_F(MessageRegistryTest, GetMessageUnknownProtocol)
{
    auto msg = registry.getMessage(static_cast<RecoveryProtocol>(0xFF),
                                   deviceRecoveryFailed);
    EXPECT_FALSE(msg.has_value());
}

TEST_F(MessageRegistryTest, GetMessageUnknownErrorCode)
{
    auto msg = registry.getMessage(RecoveryProtocol::GlacierRecovery, 0xFF);
    EXPECT_FALSE(msg.has_value());
}

TEST_F(MessageRegistryTest, GetMessageDeviceNotResponding)
{
    auto msg = registry.getMessage(RecoveryProtocol::GlacierRecovery,
                                   deviceNotResponding);
    EXPECT_TRUE(msg.has_value());
}

TEST_F(MessageRegistryTest, GetMessageNoDevicesFound)
{
    auto msg =
        registry.getMessage(RecoveryProtocol::GlacierRecovery, noDevicesFound);
    EXPECT_TRUE(msg.has_value());
}

// Test various OCP sub-protocols
TEST_F(MessageRegistryTest, GetMessageOCPStatusError)
{
    auto msg = registry.getMessage(RecoveryProtocol::OCPRecoveryStatusError,
                                   deviceRecoveryFailed);
    if (msg.has_value())
    {
        EXPECT_FALSE(std::get<0>(*msg).empty());
    }
}

TEST_F(MessageRegistryTest, GetMessageOCPProtocolError)
{
    auto msg = registry.getMessage(RecoveryProtocol::OCPRecoveryProtocolError,
                                   deviceRecoveryFailed);
    if (msg.has_value())
    {
        EXPECT_FALSE(std::get<0>(*msg).empty());
    }
}

TEST_F(MessageRegistryTest, GetMessageOCPDeviceStatusCode)
{
    auto msg = registry.getMessage(RecoveryProtocol::OCPDeviceStatusCode, 0x00);
    if (msg.has_value())
    {
        EXPECT_FALSE(std::get<0>(*msg).empty());
    }
}

// ========================== createMessageRegistryResourceErrors
// ==========================

TEST_F(MessageRegistryTest, CreateResourceErrorsKnownProtocol)
{
    EXPECT_NO_THROW(registry.createMessageRegistryResourceErrors(
        resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
        deviceRecoveryFailed, "GPU_0"));
}

TEST_F(MessageRegistryTest, CreateResourceErrorsUnknownProtocol)
{
    EXPECT_NO_THROW(registry.createMessageRegistryResourceErrors(
        resourceErrorsDetected, static_cast<RecoveryProtocol>(0xFF), 0xFF,
        "GPU_0"));
}

TEST_F(MessageRegistryTest, CreateResourceErrorsEmptyResolution)
{
    EXPECT_NO_THROW(registry.createMessageRegistryResourceErrors(
        resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
        noDevicesFound, "GPU_0"));
}

TEST_F(MessageRegistryTest, CreateMessageRegistryCatchesDbusFailure)
{
    EXPECT_CALL(sdbusMock,
                sd_bus_call(nullptr, nullptr, testing::_, testing::_, nullptr))
        .WillOnce(Return(-EIO));

    EXPECT_NO_THROW(registry.createMessageRegistry(recoveryStarted, "GPU_2"));
}

TEST_F(MessageRegistryTest, CreateResourceErrorsCatchesDbusFailure)
{
    EXPECT_CALL(sdbusMock,
                sd_bus_call(nullptr, nullptr, testing::_, testing::_, nullptr))
        .WillOnce(Return(-EIO));

    EXPECT_NO_THROW(registry.createMessageRegistryResourceErrors(
        resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
        deviceRecoveryFailed, "GPU_3"));
}

TEST_F(MessageRegistryTest, CreateMessageRegistryCatchesMethodBuildFailure)
{
    EXPECT_CALL(sdbusMock, sd_bus_message_new_method_call(
                               testing::_, testing::_, testing::_, testing::_,
                               testing::_, testing::_))
        .WillOnce(Return(-ENOMEM));

    EXPECT_NO_THROW(registry.createMessageRegistry(recoveryStarted, "GPU_4"));
}

TEST_F(MessageRegistryTest, CreateResourceErrorsCatchesMethodBuildFailure)
{
    EXPECT_CALL(sdbusMock, sd_bus_message_new_method_call(
                               testing::_, testing::_, testing::_, testing::_,
                               testing::_, testing::_))
        .WillOnce(Return(-ENOMEM));

    EXPECT_NO_THROW(registry.createMessageRegistryResourceErrors(
        resourceErrorsDetected, RecoveryProtocol::GlacierRecovery,
        deviceRecoveryFailed, "GPU_5"));
}
