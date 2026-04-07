#pragma once

#include "../../fw-status/force_recovery/recovery_mode_manager_base.hpp"

#include <mcu_recovery_manager.hpp>

#include <memory>
#include <string>
#include <vector>

namespace test::fw_status_fake_mcu_mode
{

inline std::vector<std::string> createdPaths{};
inline bool throwOnCreate = false;

inline void reset()
{
    createdPaths.clear();
    throwOnCreate = false;
}

} // namespace test::fw_status_fake_mcu_mode

namespace nvidia::recovery
{

class MCURecoveryModeManager : public RecoveryModeManagerBase
{
  public:
    MCURecoveryModeManager(
        sdbusplus::bus_t& bus, std::string chassisName, std::string objPath,
        const std::shared_ptr<mcu_recovery_manager::MCURecoveryManager>&,
        std::string) :
        RecoveryModeManagerBase(bus, std::move(chassisName), std::move(objPath))
    {
        if (test::fw_status_fake_mcu_mode::throwOnCreate)
        {
            throw std::runtime_error(
                "fake MCURecoveryModeManager creation failure");
        }
        test::fw_status_fake_mcu_mode::createdPaths.push_back(getObjectPath());
    }

  protected:
    void performForceRecovery() override
    {}
};

} // namespace nvidia::recovery
