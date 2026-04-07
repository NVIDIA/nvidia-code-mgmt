#pragma once

namespace recovery_tool
{

enum class DeviceStatus : int
{
    StatusPending = 0x0,
    DeviceHealthy = 0x1,
    DeviceError = 0x2,
    RecoveryMode = 0x3,
    RecoveryPending = 0x4,
    RecoveryImgRunning = 0x5,
    BootFailure = 0xE,
    FatalError = 0xF
};

} // namespace recovery_tool
