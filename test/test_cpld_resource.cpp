/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <systemd/sd-bus.h>

#include <sdbusplus/message.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <cerrno>
#include <chrono>
#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#pragma GCC push_options
#pragma GCC optimize("O0")
#define private public
#define protected public
#include "../fw-status/cpld_resource.hpp"

#include "../fw-status/cpld_resource.cpp"
#undef private
#undef protected
#pragma GCC pop_options

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

namespace
{

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

void expectCallSuccess(NiceMock<sdbusplus::SdBusMock>& sdbusMock)
{
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            *reply = nullptr;
            return 0;
        });
}

void expectLogPathsReply(NiceMock<sdbusplus::SdBusMock>& sdbusMock,
                         const std::vector<std::string>& logPaths)
{
    expectCallSuccess(sdbusMock);
    EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    for (const auto& path : logPaths)
    {
        EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(0));
        EXPECT_CALL(sdbusMock, sd_bus_message_read_basic(nullptr, 's', _))
            .WillOnce([path](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = path.c_str();
                return 1;
            });
    }
    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, 0))
        .WillOnce(Return(1));
    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));
}

void expectAdditionalDataReply(
    NiceMock<sdbusplus::SdBusMock>& sdbusMock,
    const std::map<std::string, std::string>& additionalData)
{
    expectCallSuccess(sdbusMock);
    EXPECT_CALL(sdbusMock, sd_bus_message_verify_type(nullptr, 'v', _))
        .WillOnce(Return(1));
    EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'v', _))
        .WillOnce(Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    for (const auto& [key, value] : additionalData)
    {
        EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, 0))
            .WillOnce(Return(0));
        EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'e', _))
            .WillOnce(Return(0));
        EXPECT_CALL(sdbusMock, sd_bus_message_read_basic(nullptr, 's', _))
            .WillOnce([key](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = key.c_str();
                return 1;
            });
        EXPECT_CALL(sdbusMock, sd_bus_message_read_basic(nullptr, 's', _))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = value.c_str();
                return 1;
            });
        EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
            .WillOnce(Return(0));
    }
    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, 0))
        .WillOnce(Return(1));
    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));
}

void expectManagedObjectsReplyEmpty(NiceMock<sdbusplus::SdBusMock>& sdbusMock)
{
    expectCallSuccess(sdbusMock);
    EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_at_end(nullptr, 0))
        .WillOnce(Return(1));
    EXPECT_CALL(sdbusMock, sd_bus_message_exit_container(nullptr))
        .WillOnce(Return(0));
}

void expectMalformedLogPathsReply(NiceMock<sdbusplus::SdBusMock>& sdbusMock)
{
    expectCallSuccess(sdbusMock);
    EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(-EINVAL));
}

void expectMalformedAdditionalDataReply(
    NiceMock<sdbusplus::SdBusMock>& sdbusMock)
{
    expectCallSuccess(sdbusMock);
    EXPECT_CALL(sdbusMock, sd_bus_message_verify_type(nullptr, 'v', _))
        .WillOnce(Return(1));
    EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'v', _))
        .WillOnce(Return(0));
    EXPECT_CALL(sdbusMock, sd_bus_message_enter_container(nullptr, 'a', _))
        .WillOnce(Return(-EINVAL));
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

sdbusplus::message::message makeMctpSignal(sdbusplus::bus_t& sender,
                                           const std::string& member,
                                           const std::string& objectPath,
                                           uint8_t eid)
{
    auto msg =
        sender.new_signal(mctpObjMgrPath.data(),
                          "org.freedesktop.DBus.ObjectManager", member.c_str());
    nvidia::software::updater::InterfaceMap interfaces{
        {mctpEndpointIntfName, {{"EID", eid}}}};
    if (member == "InterfacesAdded")
    {
        msg.append(sdbusplus::object_path(objectPath), interfaces);
    }
    else
    {
        msg.append(sdbusplus::object_path(objectPath),
                   std::vector<std::string>{mctpEndpointIntfName});
    }
    return msg;
}

sdbusplus::message::message makeMalformedSignal(sdbusplus::bus_t& sender,
                                                const std::string& signalPath,
                                                const std::string& member)
{
    auto msg =
        sender.new_signal(signalPath.c_str(),
                          "org.freedesktop.DBus.ObjectManager", member.c_str());
    msg.append(std::string("bad-payload"));
    return msg;
}

sdbusplus::message::message
    makeLoggingSignal(sdbusplus::bus_t& sender, const std::string& objectPath,
                      const LoggingInterfaceMap& interfaces)
{
    auto msg =
        sender.new_signal(loggingObjPath, "org.freedesktop.DBus.ObjectManager",
                          "InterfacesAdded");
    msg.append(sdbusplus::object_path(objectPath), interfaces);
    return msg;
}

} // namespace

class CpldResourceTest : public testing::Test
{
  protected:
    void SetUp() override
    {}

    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);
};

TEST_F(CpldResourceTest, Construct)
{
    CpldResource resource(bus, "/xyz/openbmc_project/state/decorator/cpld0", 10,
                          "CPLD_0");

    EXPECT_EQ(resource.getObjectPath(),
              "/xyz/openbmc_project/state/decorator/cpld0");
}

// updateHealth() removed - CpldResource doesn't override
// BaseResource::updateHealth() so it's a no-op; the real logic is in
// checkExistingAuthFail() + monitorLoggingEvents()

TEST_F(CpldResourceTest, HasLoggingEntryPrefix)
{
    // Test the namespace-level helper
    EXPECT_TRUE(hasLoggingEntryPrefix("/xyz/openbmc_project/logging/entry/1"));
    EXPECT_FALSE(hasLoggingEntryPrefix("/xyz/openbmc_project/other/path"));
    EXPECT_FALSE(hasLoggingEntryPrefix(""));
}

TEST_F(CpldResourceTest, ConstructorScansExistingLogsAndSetsAuthFailure)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {"/xyz/openbmc_project/logging/entry/1",
                                    "/xyz/openbmc_project/logging/entry/2"});
    expectAdditionalDataReply(
        sdbusMock, {{"ERROR_ID", "OTHER"}, {"DEVICE_NAME", "CPLD_0"}});
    expectAdditionalDataReply(sdbusMock, {{"ERROR_ID", "HPM-CPLD-AUTH-FAIL"},
                                          {"DEVICE_NAME", "CPLD_0"}});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld0", 10,
                          "CPLD_0");

    EXPECT_TRUE(resource.authFailed);
    EXPECT_EQ(resource.health(), HealthServer::HealthType::Critical);
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);
}

TEST_F(CpldResourceTest, ProcessLogEntryAndLoggingSignalsCoverCallbacks)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld1", 11,
                          "CPLD_1");

    resource.handleLogEntry({{"ERROR_ID", "OTHER"}});
    EXPECT_FALSE(resource.authFailed);

    expectAdditionalDataReply(sdbusMock, {{"ERROR_ID", "HPM-CPLD-AUTH-FAIL"},
                                          {"DEVICE_NAME", "CPLD_1"}});
    EXPECT_TRUE(
        resource.processLogEntry("/xyz/openbmc_project/logging/entry/3"));
    EXPECT_TRUE(resource.authFailed);

    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce(Return(-ENOENT));
    EXPECT_FALSE(
        resource.processLogEntry("/xyz/openbmc_project/logging/entry/4"));

    expectLogPathsReply(sdbusMock, {});
    expectManagedObjectsReplyEmpty(sdbusMock);
    CpldResource callbackResource(bus, "/xyz/openbmc_project/software/cpld2",
                                  12, "CPLD_2");

    auto signalBus = sdbusplus::bus::new_default();

    auto wrongType = makeLoggingSignal(
        signalBus, "/xyz/openbmc_project/logging/entry/5",
        {{loggingEntryIntf, {{additionalDataProperty, std::string("bad")}}}});
    EXPECT_GT(
        dispatchMatchCallback(callbackResource.logEntryAddedMatch, wrongType),
        0);
    EXPECT_FALSE(callbackResource.authFailed);

    auto goodSignal = makeLoggingSignal(
        signalBus, "/xyz/openbmc_project/logging/entry/6",
        {{loggingEntryIntf,
          {{additionalDataProperty,
            LoggingAdditionalData{{errorIDProperty, cpldAuthFailErrorID},
                                  {deviceNameProperty, "CPLD_2"}}}}}});
    EXPECT_GT(
        dispatchMatchCallback(callbackResource.logEntryAddedMatch, goodSignal),
        0);

    EXPECT_TRUE(callbackResource.authFailed);
    EXPECT_EQ(callbackResource.state(),
              OperationalStatusServer::StateType::StandbyOffline);

    auto malformed =
        makeMalformedSignal(signalBus, loggingObjPath, "InterfacesAdded");
    EXPECT_GT(
        dispatchMatchCallback(callbackResource.logEntryAddedMatch, malformed),
        0);
}

TEST_F(CpldResourceTest, SMAEndpointSignalsCoverAddRemoveAndMalformedPaths)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld3", 13,
                          "CPLD_3");

    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);

    auto signalBus = sdbusplus::bus::new_default();
    const std::string endpoint =
        std::string(mctpObjPathPrefix) + std::to_string(13);

    auto add = makeMctpSignal(signalBus, "InterfacesAdded", endpoint, 13);
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, add), 0);
    EXPECT_EQ(resource.smaEndpointObjectPath, endpoint);
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Enabled);

    auto badAdd = makeMalformedSignal(signalBus, mctpObjMgrPath.data(),
                                      "InterfacesAdded");
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, badAdd), 0);

    auto remove = makeMctpSignal(signalBus, "InterfacesRemoved", endpoint, 13);
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointRemovedMatch, remove),
              0);
    EXPECT_TRUE(resource.hasObservedSMAEndpoint);
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::UnavailableOffline);

    auto badRemove = makeMalformedSignal(signalBus, mctpObjMgrPath.data(),
                                         "InterfacesRemoved");
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, badRemove), 0);
}

TEST_F(CpldResourceTest, HandleLogEntryAndLoggingSignalsCoverIgnoredBranches)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld4", 14,
                          "CPLD_4");

    resource.handleLogEntry({{errorIDProperty, cpldAuthFailErrorID}});
    EXPECT_FALSE(resource.authFailed);

    resource.handleLogEntry({{deviceNameProperty, "CPLD_4"}});
    EXPECT_FALSE(resource.authFailed);

    resource.handleLogEntry(
        {{errorIDProperty, "OTHER"}, {deviceNameProperty, "CPLD_4"}});
    EXPECT_FALSE(resource.authFailed);

    resource.handleLogEntry({{errorIDProperty, cpldAuthFailErrorID},
                             {deviceNameProperty, "OTHER"}});
    EXPECT_FALSE(resource.authFailed);

    auto signalBus = sdbusplus::bus::new_default();

    auto unrelatedPath = makeLoggingSignal(
        signalBus, "/xyz/openbmc_project/other/entry/1",
        {{loggingEntryIntf,
          {{additionalDataProperty,
            LoggingAdditionalData{{errorIDProperty, cpldAuthFailErrorID},
                                  {deviceNameProperty, "CPLD_4"}}}}}});
    EXPECT_GT(dispatchMatchCallback(resource.logEntryAddedMatch, unrelatedPath),
              0);
    EXPECT_FALSE(resource.authFailed);

    auto missingInterface = makeLoggingSignal(
        signalBus, "/xyz/openbmc_project/logging/entry/7", {});
    EXPECT_GT(
        dispatchMatchCallback(resource.logEntryAddedMatch, missingInterface),
        0);
    EXPECT_FALSE(resource.authFailed);

    auto missingAdditional =
        makeLoggingSignal(signalBus, "/xyz/openbmc_project/logging/entry/8",
                          {{loggingEntryIntf, {}}});
    EXPECT_GT(
        dispatchMatchCallback(resource.logEntryAddedMatch, missingAdditional),
        0);
    EXPECT_FALSE(resource.authFailed);
}

TEST_F(CpldResourceTest, CheckExistingAuthFailStopsAfterFirstMatch)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {"/xyz/openbmc_project/logging/entry/10",
                                    "/xyz/openbmc_project/logging/entry/11",
                                    "/xyz/openbmc_project/logging/entry/12"});
    expectAdditionalDataReply(sdbusMock, {{"ERROR_ID", "HPM-CPLD-AUTH-FAIL"},
                                          {"DEVICE_NAME", "CPLD_BREAK"}});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld-break", 15,
                          "CPLD_BREAK");

    EXPECT_TRUE(resource.authFailed);
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);
}

TEST_F(CpldResourceTest, CheckExistingAuthFailContinuesAfterFailedLogEntryRead)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {"/xyz/openbmc_project/logging/entry/20",
                                    "/xyz/openbmc_project/logging/entry/21"});
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce(Return(-EIO));
    expectAdditionalDataReply(sdbusMock, {{"ERROR_ID", "HPM-CPLD-AUTH-FAIL"},
                                          {"DEVICE_NAME", "CPLD_CONTINUE"}});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld-continue",
                          18, "CPLD_CONTINUE");

    EXPECT_TRUE(resource.authFailed);
    EXPECT_EQ(resource.state(),
              OperationalStatusServer::StateType::StandbyOffline);
}

TEST_F(CpldResourceTest, SMAEndpointSignalsCoverIgnoredAndMissingEidBranches)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld5", 16,
                          "CPLD_5");

    auto signalBus = sdbusplus::bus::new_default();
    const std::string expectedPath =
        std::string(mctpObjPathPrefix) + std::to_string(16);

    auto missingInterface = signalBus.new_signal(
        mctpObjMgrPath.data(), "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded");
    missingInterface.append(sdbusplus::object_path(expectedPath),
                            nvidia::software::updater::InterfaceMap{});
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, missingInterface),
        0);
    EXPECT_TRUE(resource.smaEndpointObjectPath.empty());

    auto wrongType = signalBus.new_signal(mctpObjMgrPath.data(),
                                          "org.freedesktop.DBus.ObjectManager",
                                          "InterfacesAdded");
    wrongType.append(
        sdbusplus::object_path(expectedPath),
        nvidia::software::updater::InterfaceMap{
            {mctpEndpointIntfName, {{"EID", std::string("bad-eid")}}}});
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, wrongType),
              0);
    EXPECT_TRUE(resource.smaEndpointObjectPath.empty());

    auto missingEid = signalBus.new_signal(mctpObjMgrPath.data(),
                                           "org.freedesktop.DBus.ObjectManager",
                                           "InterfacesAdded");
    missingEid.append(
        sdbusplus::object_path(expectedPath),
        nvidia::software::updater::InterfaceMap{{mctpEndpointIntfName, {}}});
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, missingEid),
              0);
    EXPECT_TRUE(resource.smaEndpointObjectPath.empty());

    auto wrongValue = makeMctpSignal(signalBus, "InterfacesAdded",
                                     expectedPath + "-wrong", 17);
    EXPECT_GT(dispatchMatchCallback(resource.smaEndpointAddedMatch, wrongValue),
              0);
    EXPECT_TRUE(resource.smaEndpointObjectPath.empty());

    auto matchingAdd =
        makeMctpSignal(signalBus, "InterfacesAdded", expectedPath, 16);
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointAddedMatch, matchingAdd), 0);
    EXPECT_EQ(resource.smaEndpointObjectPath, expectedPath);

    auto wrongRemove = makeMctpSignal(signalBus, "InterfacesRemoved",
                                      expectedPath + "-other", 16);
    EXPECT_GT(
        dispatchMatchCallback(resource.smaEndpointRemovedMatch, wrongRemove),
        0);
    EXPECT_EQ(resource.smaEndpointObjectPath, expectedPath);
}

TEST_F(CpldResourceTest, ConstructorHandlesLogScanFailureAndContinues)
{
    testing::InSequence seq;
    EXPECT_CALL(sdbusMock, sd_bus_call(nullptr, nullptr, _, _, _))
        .WillOnce(Return(-EIO));
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld-log-fail",
                          17, "CPLD_LOG_FAIL");

    EXPECT_FALSE(resource.authFailed);
    EXPECT_TRUE(resource.smaEndpointObjectPath.empty());
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
}

TEST_F(CpldResourceTest, ConstructorHandlesMalformedLogPathReplyAndContinues)
{
    testing::InSequence seq;
    expectMalformedLogPathsReply(sdbusMock);
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus,
                          "/xyz/openbmc_project/software/cpld-log-malformed",
                          19, "CPLD_LOG_MALFORMED");

    EXPECT_FALSE(resource.authFailed);
    EXPECT_TRUE(resource.smaEndpointObjectPath.empty());
    EXPECT_EQ(resource.state(), OperationalStatusServer::StateType::Absent);
}

TEST_F(CpldResourceTest, ProcessLogEntryCoversAdditionalDataReadFailure)
{
    testing::InSequence seq;
    expectLogPathsReply(sdbusMock, {});
    expectManagedObjectsReplyEmpty(sdbusMock);

    CpldResource resource(bus, "/xyz/openbmc_project/software/cpld-read-fail",
                          20, "CPLD_READ_FAIL");

    expectMalformedAdditionalDataReply(sdbusMock);
    EXPECT_FALSE(
        resource.processLogEntry("/xyz/openbmc_project/logging/entry/30"));
    EXPECT_FALSE(resource.authFailed);
}
