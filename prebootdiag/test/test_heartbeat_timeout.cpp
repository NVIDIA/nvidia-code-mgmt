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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nvidia::prebootdiag
{

using namespace constants;
using ::testing::_;
using ::testing::Ne;
using ::testing::Return;

using HeartbeatTest = PreBootDiagFixture;

// Multiple heartbeats before a result — all accepted, DiagStatus transitions
// to TestRunning on the first one and stays there until Result arrives.
TEST_F(HeartbeatTest, MultipleHeartbeatsBeforeResult)
{
    EXPECT_CALL(*mockGpio, setPin(_, _))
        .WillRepeatedly(Return(std::error_code{}));

    EXPECT_CALL(*mockDbus, mockGetDiagStatus())
        .WillOnce(Return(DiagStatus::NotStarted));
    EXPECT_CALL(*mockDbus, mockHasDiagConfig()).WillOnce(Return(true));

    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::InProgress)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(true)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetSettingsStringProperty("DiagResult", "[]"))
        .Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::TestRunning)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::Completed)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagStatus(DiagStatus::NotStarted)).Times(1);
    EXPECT_CALL(*mockDbus, mockSetDiagMode(false)).Times(1);

    EXPECT_CALL(*mockDbus, mockGetSettingsStringProperty("DiagResult"))
        .WillOnce(Return("[]"));
    EXPECT_CALL(*mockDbus,
                mockSetSettingsStringProperty("DiagResult", Ne("[]")))
        .Times(1);

    auto diag = createAndStart();
    sendStateUpdate(stateHeartbeatReceived);
    sendStateUpdate(stateHeartbeatReceived);
    sendStateUpdate(stateHeartbeatReceived);
    sendStateUpdate(
        stateResultReceived,
        R"({"Tid":1,"Result":0,"ResultMaskSize":0,"ResultMask":[]})");
    sendStateUpdate(stateSessionEnded);
    io.run();
}

} // namespace nvidia::prebootdiag
