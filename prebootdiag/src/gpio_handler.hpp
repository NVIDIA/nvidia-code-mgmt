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

#include <gpiod.hpp>

#include <initializer_list>
#include <map>
#include <string>
#include <system_error>

namespace nvidia::prebootdiag
{

class GpioHandlerInterface
{
  public:
    virtual ~GpioHandlerInterface() = default;
    // Transient write: request the line, drive `value`, release. The
    // kernel reclaims the line as soon as this returns. Use for IST_SYS_RST
    // pulses where prebootdiag must not retain ownership of the reset
    // signal between writes.
    virtual std::error_code setPin(const std::string& lineName, int value) = 0;
    // Sticky write: request the line on first call, drive `value`, and
    // KEEP the line requested. Subsequent holdPin() calls on the same
    // name reuse the cached request and just update the value. Ownership
    // is returned only via releaseAllPins() or handler destruction. Use for
    // boot-chain straps that must remain driven across reset transitions.
    virtual std::error_code holdPin(const std::string& lineName, int value) = 0;
    // Releases every line currently held by holdPin(). Reserved for
    // safety-net cleanup (e.g. handler destruction); explicit code paths
    // should prefer releasePins() to declare which lines they own.
    virtual void releaseAllPins() = 0;
    // Release a specific subset of held lines by name. Names not currently
    // held are silently skipped — best-effort cleanup.
    virtual void releasePins(std::initializer_list<const char*> names) = 0;

    // Batch sugar: apply `value` to every line in `names`. Continues
    // through the list on error so best-effort callers can discard the
    // return; returns the first non-success error_code encountered, or
    // success if all writes succeeded.
    std::error_code setPins(std::initializer_list<const char*> names,
                            int value);
    std::error_code holdPins(std::initializer_list<const char*> names,
                             int value);
};

class LibGpioHandler : public GpioHandlerInterface
{
  public:
    ~LibGpioHandler() override;
    std::error_code setPin(const std::string& lineName, int value) override;
    std::error_code holdPin(const std::string& lineName, int value) override;
    void releaseAllPins() override;
    void releasePins(std::initializer_list<const char*> names) override;

  private:
    std::map<std::string, gpiod::line> heldLines;
};

} // namespace nvidia::prebootdiag
