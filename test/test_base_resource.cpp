/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "../fw-status/base_resource.hpp"

#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::_;
using ::testing::Return;

class BaseResourceTest : public testing::Test
{
  protected:
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);

    static constexpr const char* testPath =
        "/xyz/openbmc_project/state/decorator/test_resource";
};

// ========================== Basic construction ==========================

TEST_F(BaseResourceTest, ConstructAndGetObjectPath)
{
    BaseResource resource(bus, testPath);
    EXPECT_EQ(resource.getObjectPath(), testPath);
}

// ========================== Health setter/getter ==========================

TEST_F(BaseResourceTest, SetHealthCreatesDbusObject)
{
    BaseResource resource(bus, testPath);
    // First call creates the ResourceInterfaces D-Bus object
    EXPECT_NO_THROW(resource.health(HealthServer::HealthType::OK));
}

TEST_F(BaseResourceTest, SetHealthTwiceReusesObject)
{
    BaseResource resource(bus, testPath);
    resource.health(HealthServer::HealthType::OK);
    // Second call should reuse existing D-Bus object
    EXPECT_NO_THROW(resource.health(HealthServer::HealthType::Critical));
}

TEST_F(BaseResourceTest, GetHealthAfterSet)
{
    BaseResource resource(bus, testPath);
    resource.health(HealthServer::HealthType::OK);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::OK);
}

TEST_F(BaseResourceTest, GetHealthWithoutSetThrows)
{
    BaseResource resource(bus, testPath);
    EXPECT_THROW(resource.health(), std::runtime_error);
}

// ========================== State setter/getter ==========================

TEST_F(BaseResourceTest, SetStateCreatesDbusObject)
{
    BaseResource resource(bus, testPath);
    EXPECT_NO_THROW(
        resource.state(OperationalStatusServer::StateType::Enabled));
}

TEST_F(BaseResourceTest, GetStateAfterSet)
{
    BaseResource resource(bus, testPath);
    resource.state(OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
}

TEST_F(BaseResourceTest, GetStateWithoutSetThrows)
{
    BaseResource resource(bus, testPath);
    EXPECT_THROW(resource.state(), std::runtime_error);
}

TEST_F(BaseResourceTest, SetStateThenHealth)
{
    BaseResource resource(bus, testPath);
    // state() creates the D-Bus object
    resource.state(OperationalStatusServer::StateType::Enabled);
    // health() reuses it
    resource.health(HealthServer::HealthType::OK);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::OK);
}

// ========================== deleteDbusObject ==========================

TEST_F(BaseResourceTest, DeleteDbusObjectWhenExists)
{
    BaseResource resource(bus, testPath);
    resource.health(HealthServer::HealthType::OK);
    EXPECT_NO_THROW(resource.deleteDbusObject());
    // After delete, getting health should throw
    EXPECT_THROW(resource.health(), std::runtime_error);
}

TEST_F(BaseResourceTest, DeleteDbusObjectWhenNotExists)
{
    BaseResource resource(bus, testPath);
    // Should be no-op
    EXPECT_NO_THROW(resource.deleteDbusObject());
}

// ========================== Chassis power state ==========================

TEST_F(BaseResourceTest, DefaultChassisPowerState)
{
    BaseResource resource(bus, testPath);
    EXPECT_TRUE(resource.getChassisPowerState().empty());
}

TEST_F(BaseResourceTest, SetAndGetChassisPowerState)
{
    BaseResource resource(bus, testPath);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    EXPECT_EQ(resource.getChassisPowerState(),
              "xyz.openbmc_project.State.Chassis.PowerState.On");
}

TEST_F(BaseResourceTest, DefaultHasChassisPowerSource)
{
    BaseResource resource(bus, testPath);
    EXPECT_FALSE(resource.hasChassisPowerSource());
}

TEST_F(BaseResourceTest, SetConnectedToChassis)
{
    BaseResource resource(bus, testPath);
    resource.setConnectedToChassis(true);
    EXPECT_TRUE(resource.hasChassisPowerSource());
}

TEST_F(BaseResourceTest, SetConnectedToChassisDisconnect)
{
    BaseResource resource(bus, testPath);
    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    // Disconnecting clears power state
    resource.setConnectedToChassis(false);
    EXPECT_FALSE(resource.hasChassisPowerSource());
    EXPECT_TRUE(resource.getChassisPowerState().empty());
}

// ========================== isChassisPoweredOff ==========================

TEST_F(BaseResourceTest, IsChassisPoweredOffNotConnected)
{
    BaseResource resource(bus, testPath);
    EXPECT_FALSE(resource.isChassisPoweredOff());
}

TEST_F(BaseResourceTest, IsChassisPoweredOffWhenOn)
{
    BaseResource resource(bus, testPath);
    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    EXPECT_FALSE(resource.isChassisPoweredOff());
}

TEST_F(BaseResourceTest, IsChassisPoweredOffWhenOff)
{
    BaseResource resource(bus, testPath);
    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    EXPECT_TRUE(resource.isChassisPoweredOff());
}

// ========================== Virtual updateHealth ==========================

TEST_F(BaseResourceTest, DefaultUpdateHealthIsNoOp)
{
    BaseResource resource(bus, testPath);
    // Default implementation is a no-op
    EXPECT_NO_THROW(resource.updateHealth());
}

// ========================== SetRecoveryModeInterface
// ==========================

TEST_F(BaseResourceTest, SetRecoveryModeInterfaceCallsCallback)
{
    bool called = false;
    auto callback = [&called]() { called = true; };
    SetRecoveryModeInterface iface(bus, testPath, callback);
    iface.setRecoveryMode();
    EXPECT_TRUE(called);
}

TEST_F(BaseResourceTest, SetRecoveryModeInterfaceThrowsOnError)
{
    auto callback = []() { throw std::runtime_error("test error"); };
    SetRecoveryModeInterface iface(bus, testPath, callback);
    EXPECT_THROW(
        iface.setRecoveryMode(),
        sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure);
}

// ========================== BootStatus ==========================

TEST_F(BaseResourceTest, BootStatusConstruction)
{
    BootStatus bs(bus, testPath);
    EXPECT_NO_THROW(
        bs.bootStatusType(BootStatusServer::BootStatusTypes::OCPDeviceStatus));
}

TEST_F(BaseResourceTest, BootStatusSetAndGet)
{
    BootStatus bs(bus, testPath);
    bs.bootStatusType(BootStatusServer::BootStatusTypes::OCPDeviceStatus);
    EXPECT_EQ(bs.bootStatusType(),
              BootStatusServer::BootStatusTypes::OCPDeviceStatus);
}

// ========================== ResourceInterfaces direct
// ==========================

TEST_F(BaseResourceTest, ResourceInterfacesConstruction)
{
    ResourceInterfaces ri(bus, testPath);
    EXPECT_NO_THROW(ri.health(HealthServer::HealthType::OK));
    EXPECT_NO_THROW(ri.state(OperationalStatusServer::StateType::Enabled));
}

TEST_F(BaseResourceTest, ResourceInterfacesHealthGetSet)
{
    ResourceInterfaces ri(bus, testPath);
    ri.health(HealthServer::HealthType::Warning);
    EXPECT_EQ(ri.health(), HealthServer::HealthType::Warning);
    ri.health(HealthServer::HealthType::Critical);
    EXPECT_EQ(ri.health(), HealthServer::HealthType::Critical);
}

TEST_F(BaseResourceTest, ResourceInterfacesStateGetSet)
{
    ResourceInterfaces ri(bus, testPath);
    ri.state(OperationalStatusServer::StateType::Disabled);
    EXPECT_EQ(ri.state(), OperationalStatusServer::StateType::Disabled);
}

// ========================== More BaseResource edge cases
// ==========================

TEST_F(BaseResourceTest, HealthSetMultipleValues)
{
    BaseResource resource(bus, testPath);
    resource.health(HealthServer::HealthType::OK);
    resource.health(HealthServer::HealthType::Warning);
    resource.health(HealthServer::HealthType::Critical);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::Critical);
}

TEST_F(BaseResourceTest, StateSetMultipleValues)
{
    BaseResource resource(bus, testPath);
    resource.state(OperationalStatusServer::StateType::Enabled);
    resource.state(OperationalStatusServer::StateType::Disabled);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Disabled);
}

TEST_F(BaseResourceTest, DeleteThenResetHealth)
{
    BaseResource resource(bus, testPath);
    resource.health(HealthServer::HealthType::OK);
    resource.state(OperationalStatusServer::StateType::Enabled);
    resource.deleteDbusObject();
    // Should be able to recreate
    resource.health(HealthServer::HealthType::Warning);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::Warning);
}

TEST_F(BaseResourceTest, SetHealthThenState)
{
    BaseResource resource(bus, testPath);
    resource.health(HealthServer::HealthType::OK);
    // D-Bus object already exists, state() reuses it
    resource.state(OperationalStatusServer::StateType::Enabled);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
}

TEST_F(BaseResourceTest, SetStateThenGetHealth)
{
    BaseResource resource(bus, testPath);
    resource.state(OperationalStatusServer::StateType::Enabled);
    // Object exists now, health getter should work
    // But health hasn't been set yet - check what default is
    auto h = resource.health();
    // Default should be whatever sdbusplus default is
    (void)h; // Just ensure no throw
}
