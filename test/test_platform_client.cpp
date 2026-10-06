/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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
#include "../nvidia_igx/igx/src/platform_client.hpp"

#include <gtest/gtest.h>

TEST(PlatformClient, FormatsBestAvailableSmcuVersion)
{
    EXPECT_EQ(nvidia::platform::formatSmcuVersion("NFW", "2.00.02"),
              "NFW 2.00.02");
    EXPECT_EQ(nvidia::platform::formatSmcuVersion("NA", "2.00.02"), "2.00.02");
    EXPECT_EQ(nvidia::platform::formatSmcuVersion("NFW", "NA"), "NFW");
    EXPECT_EQ(nvidia::platform::formatSmcuVersion("NA", "NA"), "--");
}

TEST(PlatformClient, ResolvesAtomicPlatformSnapshot)
{
    using nvidia::platform::PlatformProperties;
    using nvidia::platform::platformShortFromProperties;

    EXPECT_EQ(platformShortFromProperties(PlatformProperties{
                  {"IdentityReady", true}, {"Name", std::string{"IGX Thor"}}}),
              "Thor");
    EXPECT_EQ(platformShortFromProperties(PlatformProperties{
                  {"IdentityReady", true}, {"Name", std::string{"IGX Orin"}}}),
              "Orin");
    EXPECT_EQ(platformShortFromProperties(PlatformProperties{
                  {"IdentityReady", false}, {"Name", std::string{"IGX Thor"}}}),
              "");
    EXPECT_EQ(platformShortFromProperties(
                  PlatformProperties{{"Name", std::string{"IGX Thor"}}}),
              "");
    EXPECT_EQ(platformShortFromProperties(
                  PlatformProperties{{"IdentityReady", true}}),
              "Host");
    EXPECT_EQ(platformShortFromProperties(PlatformProperties{}), "");
}
