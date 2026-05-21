#pragma once

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

class MessageRegistry;

namespace mcu_recovery_manager
{

struct MCUInfo
{
    std::string device;
    std::string usbPort;
    std::string resetGpioName;
    std::string recoveryGpioName;
    uint8_t i2cBus = 0;
    uint16_t normalI2cAddress = 0;
    uint16_t recoveryI2cAddress = 0;
    uint16_t functionalPid = 0;
    bool resetActiveLow = true;
    bool recoveryActiveLow = true;
    enum class InterfaceType
    {
        USB,
        I2C,
        Unknown
    } interfaceType = InterfaceType::Unknown;
};

inline std::optional<bool> parseActiveLowPolarity(const std::string& s)
{
    if (s == "ActiveLow")
    {
        return true;
    }
    if (s == "ActiveHigh")
    {
        return false;
    }
    return std::nullopt;
}

} // namespace mcu_recovery_manager

namespace test::fw_status_fake_mcu
{

struct DeviceState
{
    bool healthy = false;
    bool inRecovery = false;
};

inline std::map<std::string, DeviceState> devices{};
inline bool initResult = true;
inline bool updateAllCalled = false;
inline bool releaseCalled = false;
inline std::vector<std::string> updatedDevices{};
inline std::vector<std::string> enteredDevices{};
inline bool throwOnEnter = false;
inline std::string initializedCount{};

inline void reset()
{
    devices.clear();
    initResult = true;
    updateAllCalled = false;
    releaseCalled = false;
    updatedDevices.clear();
    enteredDevices.clear();
    throwOnEnter = false;
    initializedCount.clear();
}

} // namespace test::fw_status_fake_mcu

namespace mcu_recovery_manager
{

class MCURecoveryManager
{
  public:
    bool initialize(const std::map<std::string, MCUInfo>& mcuMap,
                    std::unique_ptr<MessageRegistry> = nullptr, bool = false)
    {
        test::fw_status_fake_mcu::initializedCount =
            std::to_string(mcuMap.size());
        return true;
    }

    void updateAllDeviceInfo()
    {
        test::fw_status_fake_mcu::updateAllCalled = true;
    }

    void updateDevInfo(const std::string& deviceId)
    {
        test::fw_status_fake_mcu::updateAllCalled = true;
        test::fw_status_fake_mcu::updatedDevices.push_back(deviceId);
    }

    bool isHealthy(const std::string& deviceId)
    {
        return test::fw_status_fake_mcu::devices[deviceId].healthy;
    }

    bool isInRecoveryMode(const std::string& deviceId)
    {
        return test::fw_status_fake_mcu::devices[deviceId].inRecovery;
    }

    bool initGpioLines()
    {
        return test::fw_status_fake_mcu::initResult;
    }

    void enterRecoveryMode(const std::string& deviceId)
    {
        test::fw_status_fake_mcu::enteredDevices.push_back(deviceId);
        if (test::fw_status_fake_mcu::throwOnEnter)
        {
            throw std::runtime_error("fake enter recovery failure");
        }
    }

    void releaseGpioLines()
    {
        test::fw_status_fake_mcu::releaseCalled = true;
    }
};

} // namespace mcu_recovery_manager
