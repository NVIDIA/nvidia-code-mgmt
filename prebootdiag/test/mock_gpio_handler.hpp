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
#pragma once

#include "gpio_handler.hpp"

#include <gmock/gmock.h>

namespace nvidia::prebootdiag
{

class MockGpioHandler : public GpioHandlerInterface
{
  public:
    MOCK_METHOD(std::error_code, setPin,
                (const std::string& lineName, int value), (override));
    MOCK_METHOD(std::error_code, holdPin,
                (const std::string& lineName, int value), (override));
    MOCK_METHOD(void, releaseAllPins, (), (override));
    MOCK_METHOD(void, releasePins, (std::initializer_list<const char*> names),
                (override));
};

} // namespace nvidia::prebootdiag
