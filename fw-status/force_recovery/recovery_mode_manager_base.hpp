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

#include <functional>
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

    /** @brief Register a callback invoked after a force recovery completes
     *  successfully. Used to refresh dependent resource health so Redfish
     *  reflects the new in-recovery state immediately: a device that was
     *  already enumerated before recovery transitions into recovery via a udev
     *  'change' (which the monitor ignores) rather than an 'add', so nothing
     *  else re-triggers the resource's updateHealth().
     */
    void setRecoveryCompleteCallback(std::function<void()> cb)
    {
        onRecoveryComplete = std::move(cb);
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

        // Recovery confirmed. Refresh dependent resource health now so the
        // firmware inventory reflects the in-recovery state without waiting for
        // an external event that may never arrive.
        if (onRecoveryComplete)
        {
            try
            {
                onRecoveryComplete();
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Recovery-complete callback failed for {CHASSIS}: {ERR}",
                    "CHASSIS", chassisName, "ERR", e.what());
            }
        }
    }

  protected:
    virtual void performForceRecovery() = 0;
    std::string chassisName;
    std::string objectPath;
    std::function<void()> onRecoveryComplete;
};

} // namespace nvidia::recovery
