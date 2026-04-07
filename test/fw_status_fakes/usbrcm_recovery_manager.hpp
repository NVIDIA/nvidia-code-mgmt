#pragma once

#include "../../fw-status/force_recovery/recovery_mode_manager_base.hpp"

#include <string>
#include <vector>

namespace test::fw_status_fake_usbrcm_mode
{

inline std::vector<std::string> createdPaths{};
inline bool throwOnCreate = false;

inline void reset()
{
    createdPaths.clear();
    throwOnCreate = false;
}

} // namespace test::fw_status_fake_usbrcm_mode

namespace nvidia::recovery
{

class USBRCMRecoveryManager : public RecoveryModeManagerBase
{
  public:
    USBRCMRecoveryManager(sdbusplus::bus_t& bus, std::string chassisName,
                          std::string objPath, std::string) :
        RecoveryModeManagerBase(bus, std::move(chassisName), std::move(objPath))
    {
        if (test::fw_status_fake_usbrcm_mode::throwOnCreate)
        {
            throw std::runtime_error(
                "fake USBRCMRecoveryManager creation failure");
        }
        test::fw_status_fake_usbrcm_mode::createdPaths.push_back(
            getObjectPath());
    }

  protected:
    void performForceRecovery() override
    {}
};

} // namespace nvidia::recovery
