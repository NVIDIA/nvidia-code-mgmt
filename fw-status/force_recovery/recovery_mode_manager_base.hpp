/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <com/nvidia/SetRecoveryMode/server.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <string>

namespace nvidia::recovery
{

using SetRecoveryModeInherit = sdbusplus::server::object_t<
    sdbusplus::server::com::nvidia::SetRecoveryMode>;

class RecoveryModeManagerBase : public SetRecoveryModeInherit
{
  public:
    RecoveryModeManagerBase() = delete;
    RecoveryModeManagerBase(const RecoveryModeManagerBase&) = delete;
    RecoveryModeManagerBase& operator=(const RecoveryModeManagerBase&) = delete;
    RecoveryModeManagerBase(RecoveryModeManagerBase&&) = delete;
    RecoveryModeManagerBase& operator=(RecoveryModeManagerBase&&) = delete;
    ~RecoveryModeManagerBase() override = default;

    RecoveryModeManagerBase(sdbusplus::bus_t& bus, std::string chassisName,
                            std::string objPath) :
        SetRecoveryModeInherit(bus, objPath.c_str()),
        chassisName(std::move(chassisName)), objectPath(std::move(objPath))
    {}

    [[nodiscard]] std::string getObjectPath() const
    {
        return objectPath;
    }

    void setRecoveryMode() override
    {
        lg2::info("Performing force recovery for {CHASSIS}", "CHASSIS",
                  chassisName);
        try
        {
            performForceRecovery();
        }
        catch (const sdbusplus::exception_t&)
        {
            throw;
        }
        catch (const std::exception& e)
        {
            lg2::error("Force recovery failed for {CHASSIS}: {ERR}", "CHASSIS",
                       chassisName, "ERR", e.what());
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InternalFailure();
        }
    }

  protected:
    virtual void performForceRecovery() = 0;
    std::string chassisName;
    std::string objectPath;
};

} // namespace nvidia::recovery
