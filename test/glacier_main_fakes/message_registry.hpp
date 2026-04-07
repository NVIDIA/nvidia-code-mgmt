#pragma once

#include "sdbusplus/bus.hpp"

#include <cstdint>
#include <string>
#include <vector>

const std::string firmwareNotInRecovery{
    "NvidiaUpdate.1.0.FirmwareNotInRecovery"};
const std::string recoverySuccessful{"NvidiaUpdate.1.0.RecoverySuccessful"};
const std::string recoveryStarted{"NvidiaUpdate.1.0.RecoveryStarted"};
const std::string resourceErrorsDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

using ErrorCode = std::uint8_t;
inline constexpr ErrorCode deviceRecoveryFailed = 0x70;
inline constexpr ErrorCode deviceNotResponding = 0x71;
inline constexpr ErrorCode noDevicesFound = 0x72;

enum class RecoveryProtocol : std::uint8_t
{
    GlacierRecovery = 0x0
};

struct FakeMessageRegistryCall
{
    bool resourceError = false;
    std::string messageId;
    RecoveryProtocol recoveryProtocol = RecoveryProtocol::GlacierRecovery;
    ErrorCode errorCode = 0;
    std::string deviceName;
};

inline std::vector<FakeMessageRegistryCall> fakeMessageRegistryCalls{};

inline void resetFakeMessageRegistryCalls()
{
    fakeMessageRegistryCalls.clear();
}

class MessageRegistry
{
  public:
    explicit MessageRegistry(sdbusplus::bus::bus&)
    {}

    void createMessageRegistry(const std::string& messageID,
                               const std::string& deviceName) const
    {
        fakeMessageRegistryCalls.push_back({.resourceError = false,
                                            .messageId = messageID,
                                            .deviceName = deviceName});
    }

    void createMessageRegistryResourceErrors(
        const std::string& messageID, const RecoveryProtocol& recoveryProtocol,
        const ErrorCode& errorCode, const std::string& deviceName) const
    {
        fakeMessageRegistryCalls.push_back(
            {.resourceError = true,
             .messageId = messageID,
             .recoveryProtocol = recoveryProtocol,
             .errorCode = errorCode,
             .deviceName = deviceName});
    }
};
