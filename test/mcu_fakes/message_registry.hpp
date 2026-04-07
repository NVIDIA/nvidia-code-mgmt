#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

const std::string firmwareNotInRecovery{
    "NvidiaUpdate.1.0.FirmwareNotInRecovery"};
const std::string recoverySuccessful{"NvidiaUpdate.1.0.RecoverySuccessful"};
const std::string recoveryStarted{"NvidiaUpdate.1.0.RecoveryStarted"};
const std::string enterDOTRecovery{"NvidiaUpdate.1.0.EnterDOTRecovery"};
const std::string resourceErrorsDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

using ErrorCode = std::uint8_t;

enum class Level : std::uint8_t
{
    Critical,
    Warning,
    Informational
};

constexpr ErrorCode deviceRecoveryFailed = 0x70;
constexpr ErrorCode deviceNotResponding = 0x71;
constexpr ErrorCode noDevicesFound = 0x72;

enum class RecoveryProtocol : uint8_t
{
    MCURecovery = 0x5
};

enum class MCURecoveryErrorCode : uint8_t
{
    CmdExecFailed = 0x20,
    DevNotProvisioned = 0x21,
    GetSecurityStateFailed = 0x22,
    NotSecureDevice = 0x23,
    ReadMemoryFailed = 0x24,
    EncryptKeyNotSet = 0x25,
    InvalidSBFile = 0x26
};

class MessageRegistry
{
  public:
    void createMessageRegistry(const std::string&, const std::string&) const
    {}

    std::optional<std::tuple<std::string, std::string>>
        getMessage(const RecoveryProtocol&, const ErrorCode&) const
    {
        return std::nullopt;
    }

    void createMessageRegistryResourceErrors(const std::string&,
                                             const RecoveryProtocol&,
                                             const ErrorCode&,
                                             const std::string&,
                                             Level = Level::Critical) const
    {}
};
