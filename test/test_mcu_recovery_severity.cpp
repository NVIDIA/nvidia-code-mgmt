/*
 * Guards the severity contract of the MCU recovery manager.
 *
 * Recovery errors are written into the shared "FWUpdate" logging namespace,
 * and a Redfish task's status is the roll-up of its most severe message. An
 * entry raised while merely polling or enumerating MCUs therefore fails
 * whatever update happens to be running, even when the device is unrelated
 * to it. fw-status links this manager and calls updateDevInfo() per device
 * on every health refresh, so the blast radius is not limited to the
 * standalone recovery tool.
 *
 * The contract these tests pin:
 *   - enumerating devices reports nothing to Redfish;
 *   - a recovery that actually fails reports deviceRecoveryFailed.
 */
#include "mcu_recovery_manager.hpp"
#include "message_registry.hpp"

#include <sdbusplus/bus.hpp>

#include <map>
#include <memory>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;

namespace mcu_recovery_manager
{

// Reaches the private failure-reporting helper. Declared as a friend by
// MCURecoveryManager, so it must live in the same namespace.
struct MCURecoveryManagerTestAccess
{
    static void handleRecoveryError(MCURecoveryManager& manager,
                                    const std::string& deviceId)
    {
        manager.handleRecoveryError(deviceId);
    }
};

} // namespace mcu_recovery_manager

namespace
{

using mcu_recovery_manager::MCUInfo;
using mcu_recovery_manager::MCURecoveryManager;
using mcu_recovery_manager::MCURecoveryManagerTestAccess;

class MockMessageRegistry : public MessageRegistry
{
  public:
    explicit MockMessageRegistry(sdbusplus::bus::bus& bus) :
        MessageRegistry(bus)
    {}

    MOCK_METHOD(void, createMessageRegistryResourceErrors,
                (const std::string&, const RecoveryProtocol&, const ErrorCode&,
                 const std::string&, Level),
                (const, override));
};

// An I2C MCU on a bus that does not exist, so probing always fails and the
// "device not found" path runs deterministically without hardware.
std::map<std::string, MCUInfo> absentI2cDevice()
{
    MCUInfo info;
    info.device = "FW_IO_Board_SMA_2";
    info.i2cBus = 200;
    info.normalI2cAddress = 0x50;
    info.recoveryI2cAddress = 0x51;
    info.interfaceType = MCUInfo::InterfaceType::I2C;
    return {{"mcu0", info}};
}

} // namespace

// Enumeration is not a recovery request: an MCU that is absent is nothing to
// act on, so no Redfish entry may be raised. Before this contract existed,
// scanning an absent MCU logged noDevicesFound at Critical and turned an
// unrelated firmware update task Critical.
TEST(MCURecoverySeverity, EnumerationEmitsNoRedfishError)
{
    auto bus = sdbusplus::bus::new_default();
    auto registry = std::make_unique<MockMessageRegistry>(bus);
    EXPECT_CALL(*registry, createMessageRegistryResourceErrors(_, _, _, _, _))
        .Times(0);

    MCURecoveryManager manager;
    ASSERT_TRUE(manager.initialize(absentI2cDevice(), std::move(registry),
                                   /*initGpio=*/false));

    manager.updateAllDeviceInfo();
}

// The other half of the contract: a recovery that genuinely fails must still
// report, at Critical, so the task is failed.
TEST(MCURecoverySeverity, RecoveryFailureReportsDeviceRecoveryFailed)
{
    auto bus = sdbusplus::bus::new_default();
    auto registry = std::make_unique<MockMessageRegistry>(bus);
    EXPECT_CALL(*registry,
                createMessageRegistryResourceErrors(
                    resourceErrorsDetected, RecoveryProtocol::MCURecovery,
                    deviceRecoveryFailed, "FW_IO_Board_SMA_2", Level::Critical))
        .Times(1);

    MCURecoveryManager manager;
    ASSERT_TRUE(manager.initialize(absentI2cDevice(), std::move(registry),
                                   /*initGpio=*/false));

    MCURecoveryManagerTestAccess::handleRecoveryError(manager, "mcu0");
}
