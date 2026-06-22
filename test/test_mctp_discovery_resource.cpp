/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "dbusutils.hpp"

#include <systemd/sd-bus.h>

#include <sdbusplus/test/sdbus_mock.hpp>

// Redefine getBit helper from erot_resource.hpp (can't include full header
// due to transitive hardware deps)
inline bool getBit(const std::vector<uint8_t>& status, size_t bit)
{
    size_t maxIdx = status.size() - 1;
    return (status[maxIdx - bit / 8] >> (bit % 8)) & 1;
}
constexpr static size_t AP0_BOOT_COMPLETE_BIT = 5;
constexpr static size_t AP0_BOOT_COMPLETE_TIMEOUT_BIT = 27;

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#define private public
#define protected public
#include "../fw-status/mctp_discovery_resource.hpp"
#undef protected
#undef private

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

using nvidia::software::updater::InterfaceMap;

// Concrete test subclass to override pure virtual updateHealth()
class TestMCTPResource : public MCTPDiscoveryResource
{
  public:
    TestMCTPResource(sdbusplus::bus_t& bus, const std::string& objPath,
                     uint8_t eid) : MCTPDiscoveryResource(bus, objPath, eid)
    {}

    int updateHealthCallCount = 0;

    void updateHealth() override
    {
        updateHealthCallCount++;
        if (isDeviceEnumerated())
        {
            health(HealthServer::HealthType::OK);
            state(OperationalStatusServer::StateType::Enabled);
        }
        else
        {
            health(HealthServer::HealthType::Critical);
            state(OperationalStatusServer::StateType::Disabled);
        }
    }

    // Public accessors for protected members (test-only)
    void setMctpObjectPath(const std::string& path)
    {
        mctpObjectPath = path;
    }
    void clearMctpObjectPath()
    {
        mctpObjectPath.clear();
    }
    std::string getMctpObjectPath() const
    {
        return mctpObjectPath;
    }
    void setWasEnumeratedOnce(bool val)
    {
        wasEnumeratedOnce = val;
    }
};

class MCTPDiscoveryResourceTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        test::fw_status_fake_dbus::reset();
    }

    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);

    static constexpr const char* testPath =
        "/xyz/openbmc_project/state/decorator/test_mctp";
};

namespace
{

sdbusplus::message::message
    makeInterfacesAddedMessage(sdbusplus::bus_t& bus, const std::string& path,
                               const InterfaceMap& interfaces)
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
    InterfaceMap interfaces{{mctpEndpointIntfName, {{"EID", eid}}}};
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

} // namespace

// ========================== Construction ==========================

TEST_F(MCTPDiscoveryResourceTest, ConstructAndFetchEid)
{
    TestMCTPResource resource(bus, testPath, 42);
    EXPECT_EQ(resource.fetchEid(), 42);
}

TEST_F(MCTPDiscoveryResourceTest, InitiallyNotEnumerated)
{
    TestMCTPResource resource(bus, testPath, 10);
    EXPECT_FALSE(resource.isDeviceEnumerated());
}

TEST_F(MCTPDiscoveryResourceTest, WasNeverEnumerated)
{
    TestMCTPResource resource(bus, testPath, 10);
    EXPECT_FALSE(resource.wasDeviceEnumeratedBefore());
}

// ========================== MCTP object path ==========================

TEST_F(MCTPDiscoveryResourceTest, GetMCTPObjectPathNoMatch)
{
    TestMCTPResource resource(bus, testPath, 10);
    EXPECT_TRUE(resource.getMctpObjectPath().empty());
    EXPECT_TRUE(resource.getMCTPObjectPath().empty());
}

TEST_F(MCTPDiscoveryResourceTest,
       GetMCTPObjectPathCoversFilteringAndInitialEnumerationBranches)
{
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/networks/1/endpoints/ignored"] = {
            {"xyz.openbmc_project.Unrelated", {{"Present", true}}}};
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/networks/1/endpoints/bad-type"] = {
            {mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}};
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/networks/1/endpoints/wrong"] = {
            {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(41)}}}};
    test::fw_status_fake_dbus::managedObjects
        ["/au/com/codeconstruct/mctp1/networks/1/endpoints/right"] = {
            {mctpEndpointIntfName, {{"EID", static_cast<uint8_t>(42)}}}};

    TestMCTPResource resource(bus, testPath, 42);
    EXPECT_EQ(resource.getMctpObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/right");
    EXPECT_EQ(resource.getMCTPObjectPath(),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/right");
    EXPECT_TRUE(resource.isDeviceEnumerated());
    EXPECT_TRUE(resource.wasDeviceEnumeratedBefore());

    test::fw_status_fake_dbus::reset();
    EXPECT_TRUE(resource.getMCTPObjectPath().empty());
}

// ========================== Device enumeration state
// ==========================

TEST_F(MCTPDiscoveryResourceTest, SimulateEnumeration)
{
    TestMCTPResource resource(bus, testPath, 10);
    // Simulate setting the MCTP object path (as if MCTP discovery found it)
    resource.setMctpObjectPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/10");
    resource.setWasEnumeratedOnce(true);
    EXPECT_TRUE(resource.isDeviceEnumerated());
    EXPECT_TRUE(resource.wasDeviceEnumeratedBefore());
}

TEST_F(MCTPDiscoveryResourceTest, SimulateDeenumeration)
{
    TestMCTPResource resource(bus, testPath, 10);
    resource.setMctpObjectPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/10");
    resource.setWasEnumeratedOnce(true);
    // Now device goes offline
    resource.clearMctpObjectPath();
    EXPECT_FALSE(resource.isDeviceEnumerated());
    EXPECT_TRUE(resource.wasDeviceEnumeratedBefore());
}

TEST_F(MCTPDiscoveryResourceTest,
       MonitorMCTPEndpointCoversCallbacksCatchPathsAndReuseBranches)
{
    TestMCTPResource resource(bus, testPath, 55);
    ASSERT_NE(resource.endpointAddedMatch, nullptr);
    ASSERT_NE(resource.endpointRemovedMatch, nullptr);

    auto* addedMatch = resource.endpointAddedMatch.get();
    auto* removedMatch = resource.endpointRemovedMatch.get();
    resource.monitorMCTPEndpoint();
    EXPECT_EQ(resource.endpointAddedMatch.get(), addedMatch);
    EXPECT_EQ(resource.endpointRemovedMatch.get(), removedMatch);

    auto signalBus = sdbusplus::bus::new_default();
    const std::string path =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/55";

    auto missingInterface =
        makeInterfacesAddedMessage(signalBus, path, InterfaceMap{});
    EXPECT_GT(
        dispatchMatchCallback(resource.endpointAddedMatch, missingInterface),
        0);
    EXPECT_TRUE(resource.getMctpObjectPath().empty());
    EXPECT_EQ(resource.updateHealthCallCount, 0);

    auto wrongType = makeInterfacesAddedMessage(
        signalBus, path,
        InterfaceMap{{mctpEndpointIntfName, {{"EID", std::string("bad")}}}});
    EXPECT_GT(dispatchMatchCallback(resource.endpointAddedMatch, wrongType), 0);
    EXPECT_TRUE(resource.getMctpObjectPath().empty());

    auto missingEid = makeInterfacesAddedMessage(
        signalBus, path, InterfaceMap{{mctpEndpointIntfName, {}}});
    EXPECT_GT(dispatchMatchCallback(resource.endpointAddedMatch, missingEid),
              0);
    EXPECT_TRUE(resource.getMctpObjectPath().empty());

    auto wrongValue =
        makeInterfacesAddedMessage(signalBus, path, static_cast<uint8_t>(54));
    EXPECT_GT(dispatchMatchCallback(resource.endpointAddedMatch, wrongValue),
              0);
    EXPECT_TRUE(resource.getMctpObjectPath().empty());

    auto matchingAdd =
        makeInterfacesAddedMessage(signalBus, path, static_cast<uint8_t>(55));
    EXPECT_GT(dispatchMatchCallback(resource.endpointAddedMatch, matchingAdd),
              0);
    EXPECT_EQ(resource.getMctpObjectPath(), path);
    EXPECT_TRUE(resource.wasDeviceEnumeratedBefore());
    EXPECT_EQ(resource.updateHealthCallCount, 1);

    auto malformedAdd =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesAdded");
    EXPECT_GT(dispatchMatchCallback(resource.endpointAddedMatch, malformedAdd),
              0);
    EXPECT_EQ(resource.updateHealthCallCount, 1);

    auto wrongRemove = makeInterfacesRemovedMessage(signalBus, path + "-other");
    EXPECT_GT(dispatchMatchCallback(resource.endpointRemovedMatch, wrongRemove),
              0);
    EXPECT_EQ(resource.getMctpObjectPath(), path);
    EXPECT_EQ(resource.updateHealthCallCount, 1);

    auto matchingRemove = makeInterfacesRemovedMessage(signalBus, path);
    EXPECT_GT(
        dispatchMatchCallback(resource.endpointRemovedMatch, matchingRemove),
        0);
    EXPECT_TRUE(resource.getMctpObjectPath().empty());
    EXPECT_EQ(resource.updateHealthCallCount, 2);

    auto malformedRemove =
        makeMalformedObjectManagerMessage(signalBus, "InterfacesRemoved");
    EXPECT_GT(
        dispatchMatchCallback(resource.endpointRemovedMatch, malformedRemove),
        0);
    EXPECT_EQ(resource.updateHealthCallCount, 2);
}

// ========================== updateHealth override ==========================

TEST_F(MCTPDiscoveryResourceTest, UpdateHealthWhenEnumerated)
{
    TestMCTPResource resource(bus, testPath, 10);
    resource.setMctpObjectPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/10");
    resource.updateHealth();
    EXPECT_EQ(resource.health(), HealthServer::HealthType::OK);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);
}

TEST_F(MCTPDiscoveryResourceTest, UpdateHealthWhenNotEnumerated)
{
    TestMCTPResource resource(bus, testPath, 10);
    resource.updateHealth();
    EXPECT_EQ(resource.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Disabled);
}

// ========================== Health + chassis power interaction
// ==========================

TEST_F(MCTPDiscoveryResourceTest, ChassisPowerOffAffectsResource)
{
    TestMCTPResource resource(bus, testPath, 10);
    resource.setConnectedToChassis(true);
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.Off");
    EXPECT_TRUE(resource.isChassisPoweredOff());

    // Transition to On
    resource.setChassisPowerState(
        "xyz.openbmc_project.State.Chassis.PowerState.On");
    EXPECT_FALSE(resource.isChassisPoweredOff());
}

// ========================== BaseResource through MCTPDiscoveryResource
// ==========================

TEST_F(MCTPDiscoveryResourceTest, DeleteAndRecreateDbus)
{
    TestMCTPResource resource(bus, testPath, 10);
    resource.health(HealthServer::HealthType::OK);
    resource.deleteDbusObject();
    // After delete, setting health recreates
    resource.health(HealthServer::HealthType::Warning);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::Warning);
}

// ========================== getBit helper (erot_resource.hpp)
// ==========================

TEST(GetBitHelper, Bit0)
{
    std::vector<uint8_t> status = {0x01};
    EXPECT_TRUE(getBit(status, 0));
}

TEST(GetBitHelper, Bit7)
{
    std::vector<uint8_t> status = {0x80};
    EXPECT_TRUE(getBit(status, 7));
}

TEST(GetBitHelper, Bit0NotSet)
{
    std::vector<uint8_t> status = {0xFE};
    EXPECT_FALSE(getBit(status, 0));
}

TEST(GetBitHelper, MultiByte)
{
    // 2 bytes: [0x00, 0x01] -> bit 8 is set (in second byte from right)
    std::vector<uint8_t> status = {0x01, 0x00};
    EXPECT_TRUE(getBit(status, 8));
    EXPECT_FALSE(getBit(status, 0));
}

TEST(GetBitHelper, AP0BootCompleteBit)
{
    // AP0_BOOT_COMPLETE_BIT = 5, need at least 1 byte
    std::vector<uint8_t> status = {0x20}; // bit 5 set
    EXPECT_TRUE(getBit(status, AP0_BOOT_COMPLETE_BIT));
}

TEST(GetBitHelper, AP0BootCompleteTimeoutBit)
{
    // AP0_BOOT_COMPLETE_TIMEOUT_BIT = 27, need at least 4 bytes
    std::vector<uint8_t> status = {0x08, 0x00, 0x00, 0x00}; // bit 27 set
    EXPECT_TRUE(getBit(status, AP0_BOOT_COMPLETE_TIMEOUT_BIT));
}

TEST(GetBitHelper, AllZeros)
{
    std::vector<uint8_t> status = {0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 32; i++)
    {
        EXPECT_FALSE(getBit(status, i));
    }
}

TEST(GetBitHelper, AllOnes)
{
    std::vector<uint8_t> status = {0xFF, 0xFF, 0xFF, 0xFF};
    for (int i = 0; i < 32; i++)
    {
        EXPECT_TRUE(getBit(status, i));
    }
}

// ========================== MCTP constants ==========================

TEST(MCTPConstants, ServiceAndPaths)
{
    EXPECT_STREQ(mctpEndpointIntfName, "xyz.openbmc_project.MCTP.Endpoint");
    EXPECT_STREQ(mctpService, "au.com.codeconstruct.MCTP1");
    EXPECT_EQ(std::string(mctpObjPathPrefix),
              "/au/com/codeconstruct/mctp1/networks/1/endpoints/");
    EXPECT_EQ(std::string(mctpObjMgrPath), "/au/com/codeconstruct/mctp1");
}
