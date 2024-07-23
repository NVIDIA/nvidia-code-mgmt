#pragma once

namespace mctp_vdm
{

enum class CompletionCodes : uint8_t
{
    Success = 0x00,
    ErrGeneral = 0x01,
    ErrInvalidData = 0x02,
    ErrInvalidLength = 0x03,
    ErrNotReady = 0x04,
    ErrUnsupportedCmd = 0x05,
};

} // namespace mctp_vdm