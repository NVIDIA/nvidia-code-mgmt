/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "constants.hpp"
#include "prebootdiag_test_fixture.hpp"

#include <format>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nvidia::prebootdiag
{

using namespace constants;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Return;

using PreBootDiagTest = PreBootDiagFixture;

namespace
{

std::string resetGpio(int board)
{
    return std::format(istSysRstGpioFormat, static_cast<int>(board));
}

std::string bootChainGpio(int board)
{
    return std::format(cpuBootChain0GpioFormat, static_cast<int>(board));
}

} // namespace

// Happy path: gates pass, one TID runs to completion, session ends cleanly.
TEST_F(PreBootDiagTest, HappyPathFullSession)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));

    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::TestRunning)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Completed)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);

    EXPECT_CALL(*mockDbus, mockGetSettingsStringProperty("DiagSystemConfig"))
        .WillOnce(Return(R"({"ConfigType":0})"));
    EXPECT_CALL(*mockDbus,
                mockCallNsmConfigSetSystem(14, R"({"ConfigType":0})"))
        .WillOnce(Return(true));

    EXPECT_CALL(*mockDbus, mockGetSettingsStringProperty("DiagConfig"))
        .WillOnce(Return(R"([{"Tid":1}])"));
    EXPECT_CALL(*mockDbus, mockCallNsmConfigSetTID(14, R"([{"Tid":1}])"))
        .WillOnce(Return(true));

    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockGetSettingsStringProperty("DiagResult"))
        .WillOnce(Return("[]"));
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult",
                                                         ::testing::Ne("[]")))
        .Times(1);

    auto diag = createAndStart();
    sendStateUpdate(stateSystemConfigRequested, R"({"ConfigType":0,"Eid":14})");
    sendStateUpdate(stateTIDConfigRequested, R"({"Tid":1,"Eid":14})");
    sendStateUpdate(stateHeartbeatReceived);
    sendStateUpdate(
        stateResultReceived,
        R"({"Tid":1,"Result":0,"ResultMaskSize":0,"ResultMask":[]})");
    sendStateUpdate(stateSessionEnded);

    io.run();
}

// Missing DiagConfig → session aborts with recovery path.
TEST_F(PreBootDiagTest, NoDiagConfigTriggersRecovery)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(false));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr("No DiagConfig"), _))
        .Times(1);

    auto diag = create();
    setEnabled(true);
    io.run();
}

// DiagStatus already InProgress → session refuses to start.
TEST_F(PreBootDiagTest, DiagStatusAlreadyInProgressRejectsStart)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::InProgress));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(_, _)).Times(1);

    auto diag = create();
    setEnabled(true);
    io.run();
}

// Second Enable=true while a session is running is rejected with EBUSY.
TEST_F(PreBootDiagTest, DuplicateEnableRejected)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(_, _)).Times(1);

    auto diag = create();
    setEnabled(true);
    // Session running; second enable is rejected.
    EXPECT_THROW(setEnabled(true), sdbusplus::exception_t);
    io.run();
}

// Update call before the session starts throws.
TEST_F(PreBootDiagTest, UpdateWithoutSessionThrows)
{
    auto diag = create();
    EXPECT_THROW(sendPostCode(postCodePscFmcBootModePrebootDiag),
                 sdbusplus::exception_t);
}

// Unknown state string is rejected with InvalidArgument.
TEST_F(PreBootDiagTest, UpdateRejectsUnknownState)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(_, _)).Times(1);

    auto diag = create();
    diag->setPostCodeGateWaitTimeoutForTest(std::chrono::milliseconds(10));
    setEnabled(true);

    EXPECT_THROW(sendStateUpdate("com.example.Not.A.Real.State"),
                 sdbusplus::exception_t);
    io.run();
}

// Idempotency: repeating either gate post code is silently ignored.
TEST_F(PreBootDiagTest, UpdateGateIdempotent)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Completed)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);

    auto diag = create();
    setEnabled(true);

    sendPostCode(postCodePscFmcBootModePrebootDiag);
    EXPECT_NO_THROW(sendPostCode(postCodePscFmcBootModePrebootDiag));
    sendPostCode(postCodeMb2CcplexPrebootDiagEntry);
    EXPECT_NO_THROW(sendPostCode(postCodeMb2CcplexPrebootDiagEntry));
    sendStateUpdate(stateSessionEnded);
    io.run();
}

// Combined gate wait times out when no post code arrives — error log names
// the still-missing first post code.
TEST_F(PreBootDiagTest, PostCodeGateWaitTimesOut)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(
                               HasSubstr(postCodePscFmcBootModePrebootDiag), _))
        .Times(1);

    auto diag = create();
    diag->setPostCodeGateWaitTimeoutForTest(std::chrono::milliseconds(10));
    setEnabled(true);
    io.run();
}

// MB2 arriving before PSC-FMC throws EPROTO to the caller AND aborts the
// session via the standard recovery flow (DiagStatus=Abort, DiagMode=false,
// error log carries the out-of-order reason).
TEST_F(PreBootDiagTest, OutOfOrderPostCodeAbortsSession)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr("Out-of-order"), _))
        .Times(1);

    auto diag = create();
    setEnabled(true);

    EXPECT_THROW(sendPostCode(postCodeMb2CcplexPrebootDiagEntry),
                 sdbusplus::exception_t);
    io.run();
}

// Unknown PostCodeName in payload is rejected with EINVAL.
TEST_F(PreBootDiagTest, UnknownPostCodeNameRejected)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(_, _)).Times(1);

    auto diag = create();
    diag->setPostCodeGateWaitTimeoutForTest(std::chrono::milliseconds(10));
    setEnabled(true);

    EXPECT_THROW(sendPostCode("FOO_BAR_UNKNOWN"), sdbusplus::exception_t);
    io.run();
}

// PostCodeReceived without PostCodeName field is rejected with EINVAL.
TEST_F(PreBootDiagTest, PostCodeReceivedMissingPostCodeNameRejected)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(_, _)).Times(1);

    auto diag = create();
    diag->setPostCodeGateWaitTimeoutForTest(std::chrono::milliseconds(10));
    setEnabled(true);

    EXPECT_THROW(sendStateUpdate(statePostCodeReceived, "{}"),
                 sdbusplus::exception_t);
    io.run();
}

// PostCodeReceived with non-JSON payload is rejected with EINVAL.
TEST_F(PreBootDiagTest, PostCodeReceivedInvalidJsonRejected)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(_, _)).Times(1);

    auto diag = create();
    diag->setPostCodeGateWaitTimeoutForTest(std::chrono::milliseconds(10));
    setEnabled(true);

    EXPECT_THROW(sendStateUpdate(statePostCodeReceived, "not json"),
                 sdbusplus::exception_t);
    io.run();
}

// If only PSC-FMC arrives, the wait times out naming MB2 as the missing one.
TEST_F(PreBootDiagTest, OnlyFirstPostCodeTimesOut)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(
                               HasSubstr(postCodeMb2CcplexPrebootDiagEntry), _))
        .Times(1);

    auto diag = create();
    diag->setPostCodeGateWaitTimeoutForTest(std::chrono::milliseconds(50));
    setEnabled(true);
    sendPostCode(postCodePscFmcBootModePrebootDiag);
    io.run();
}

// Enable=false mid-wait aborts the session.
TEST_F(PreBootDiagTest, DisableAbortsSession)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr("aborted"), _))
        .Times(1);

    auto diag = create();
    setEnabled(true);
    setEnabled(false);
    io.run();
}

// Missing TID config triggers session abort with appropriate log.
TEST_F(PreBootDiagTest, MissingTidConfigTriggersRecovery)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockGetSettingsStringProperty("DiagConfig"))
        .WillOnce(Return(R"([{"Tid":1}])"));
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr("TID=99"), _)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr("failed"), _)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);

    auto diag = createAndStart();
    sendStateUpdate(stateTIDConfigRequested, R"({"Tid":99,"Eid":14})");
    io.run();
}

// Happy path drives the three-phase GPIO sandwich (assert reset →
// program boot-chain → release reset) on enable, then the symmetric
// clear sequence at session end. Ordering matters: BootROMs must be in
// reset whenever the boot-chain strap is changing.
TEST_F(PreBootDiagTest, BootChainGpiosDrivenOnEnable)
{
    testing::Sequence gpioSeq;
    // Init Phase 1: pulse reset asserted on every board (transient).
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 0)).InSequence(gpioSeq);
    // Init Phase 2: hold boot-chain straps high for the session.
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(1), 1)).InSequence(gpioSeq);
    // Init Phase 3: pulse reset deasserted (transient).
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 1)).InSequence(gpioSeq);
    // Cleanup Phase 1: drive held straps low, then release.
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(1), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio,
                releasePins(ElementsAre(bootChainGpio(0), bootChainGpio(1))))
        .InSequence(gpioSeq);
    // Cleanup Phase 2: pulse reset asserted.
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 0)).InSequence(gpioSeq);
    // Cleanup Phase 3: pulse reset deasserted so CPUs re-enter normal boot.
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 1)).InSequence(gpioSeq);

    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Completed)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);

    auto diag = createAndStart();
    sendStateUpdate(stateSessionEnded);
    io.run();
}

// If BRD1 is absent, the diagnostic session still runs and every GPIO phase
// uses only BRD0 lines.
TEST_F(PreBootDiagTest, SingleBoardGpiosUsedWhenBrd1Missing)
{
    EXPECT_CALL(*mockGpio, isPinAvailable(resetGpio(1)))
        .WillOnce(Return(false));
    EXPECT_CALL(*mockGpio, isPinAvailable(bootChainGpio(1)))
        .WillOnce(Return(false));
    EXPECT_CALL(*mockGpio, setPin(HasSubstr("BRD1_"), _)).Times(0);
    EXPECT_CALL(*mockGpio, holdPin(HasSubstr("BRD1_"), _)).Times(0);

    testing::Sequence gpioSeq;
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, releasePins(ElementsAre(bootChainGpio(0))))
        .InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 1)).InSequence(gpioSeq);

    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Completed)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);

    auto diag = createAndStart();
    sendStateUpdate(stateSessionEnded);
    io.run();
}

// BRD0 is mandatory; if any required BRD0 line is missing, the session aborts
// before GPIO init and records the missing pin in the standard error log path.
TEST_F(PreBootDiagTest, MissingBrd0GpioAbortsSession)
{
    EXPECT_CALL(*mockGpio, isPinAvailable(resetGpio(0)))
        .WillOnce(Return(false));
    EXPECT_CALL(*mockGpio, setPin(_, _)).Times(0);
    EXPECT_CALL(*mockGpio, holdPin(_, _)).Times(0);
    EXPECT_CALL(*mockGpio, releasePins(ElementsAre())).Times(1);

    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr(resetGpio(0)), _))
        .Times(1);

    auto diag = create();
    setEnabled(true);
    io.run();
}

// Partial BRD1 availability is a board-detection fault. The session aborts
// before GPIO init and cleanup remains a no-op because no GPIOs were touched.
TEST_F(PreBootDiagTest, PartialBrd1GpioAvailabilityAbortsSession)
{
    EXPECT_CALL(*mockGpio, isPinAvailable(bootChainGpio(1)))
        .WillOnce(Return(false));
    EXPECT_CALL(*mockGpio, setPin(_, _)).Times(0);
    EXPECT_CALL(*mockGpio, holdPin(_, _)).Times(0);
    EXPECT_CALL(*mockGpio, releasePins(ElementsAre())).Times(1);

    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr(bootChainGpio(1)), _))
        .Times(1);

    auto diag = create();
    setEnabled(true);
    io.run();
}

// First reset-assert failing in initGpioSequence aborts before any
// strap is touched. Cleanup still runs best-effort (all six writes,
// none of which touch the boot-chain-to-1 lines).
TEST_F(PreBootDiagTest, GpioSetFailureAbortsSession)
{
    testing::Sequence gpioSeq;
    // Init Phase 1 first call fails → initGpioSequence throws here.
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0))
        .InSequence(gpioSeq)
        .WillOnce(Return(std::make_error_code(std::errc::io_error)));
    // Cleanup Phase 1: drive boot-chain straps low, then release.
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(1), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio,
                releasePins(ElementsAre(bootChainGpio(0), bootChainGpio(1))))
        .InSequence(gpioSeq);
    // Cleanup Phase 2: pulse reset asserted on both boards.
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 0)).InSequence(gpioSeq);
    // Cleanup Phase 3: pulse reset deasserted.
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 1)).InSequence(gpioSeq);
    // Init Phase 2 never runs — boot-chain is never driven to 1.
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 1)).Times(0);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(1), 1)).Times(0);

    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr("Failed to assert"), _))
        .Times(1);

    auto diag = create();
    setEnabled(true);
    io.run();
}

// An out-of-order post code (after GPIOs are asserted) drives the session
// into the failure-recovery branch, which clears both boot-chain GPIOs
// via the three-phase symmetric cleanup.
TEST_F(PreBootDiagTest, BootChainGpiosClearedOnAbort)
{
    testing::Sequence gpioSeq;
    // Init: pulse reset, hold straps high, pulse reset deasserted.
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(1), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 1)).InSequence(gpioSeq);
    // Cleanup: drive straps low + releasePins FIRST so prebootdiag drops
    // ownership of the boot chain, then pulse IST_SYS_RST 0→1 to bring
    // CPUs back into normal boot. Clearing straps before any reset edge
    // prevents an early/glitched BootROM sample from re-latching diag.
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, holdPin(bootChainGpio(1), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio,
                releasePins(ElementsAre(bootChainGpio(0), bootChainGpio(1))))
        .InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 0)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(0), 1)).InSequence(gpioSeq);
    EXPECT_CALL(*mockGpio, setPin(resetGpio(1), 1)).InSequence(gpioSeq);

    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Abort)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);
    EXPECT_CALL(*mockDbus, mockCreateErrorLog(HasSubstr("Out-of-order"), _))
        .Times(1);

    auto diag = create();
    setEnabled(true);

    EXPECT_THROW(sendPostCode(postCodeMb2CcplexPrebootDiagEntry),
                 sdbusplus::exception_t);
    io.run();
}

// Per-TID result accumulation over multiple TIDs.
TEST_F(PreBootDiagTest, ResultAccumulationMultipleTids)
{
    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Completed)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);

    // Each handleResultReceived reads existing DiagResult then writes updated
    EXPECT_CALL(*mockDbus, mockGetSettingsStringProperty("DiagResult"))
        .WillOnce(Return("[]"))
        .WillOnce(Return(R"([{"Tid":1,"Result":0}])"))
        .WillOnce(Return(R"([{"Tid":1,"Result":0},{"Tid":2,"Result":0}])"));
    EXPECT_CALL(*mockDbus,
                mockSetSettingsStringProperty("DiagResult", HasSubstr("Tid")))
        .Times(3);

    auto diag = createAndStart();
    sendStateUpdate(stateResultReceived, R"({"Tid":1,"Result":0})");
    sendStateUpdate(stateResultReceived, R"({"Tid":2,"Result":0})");
    sendStateUpdate(stateResultReceived, R"({"Tid":3,"Result":0})");
    sendStateUpdate(stateSessionEnded);
    io.run();
}

} // namespace nvidia::prebootdiag
