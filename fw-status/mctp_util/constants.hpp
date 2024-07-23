#pragma once

#include "stddef.h"
#include "stdint.h"

#include <string_view>

namespace mapper
{
constexpr auto Service = "xyz.openbmc_project.ObjectMapper";
constexpr auto Path = "/xyz/openbmc_project/object_mapper";
constexpr auto Interface = "xyz.openbmc_project.ObjectMapper";
} // namespace mapper

namespace mctp
{
constexpr auto UUIDInterface{"xyz.openbmc_project.Common.UUID"};
}

namespace mctp_vdm
{
// Message Type for Vendor defined - IANA
constexpr uint8_t MessageType = 0x7F;

// MCTP VDM header
constexpr uint32_t nvidiaIANA = 5703;
constexpr uint8_t nvidiaMsgType = 1;
constexpr uint8_t nvidiaMsgVersion = 1;

/* Add external timestamp*/
constexpr uint8_t addExtTimestamp = 0x13;
constexpr size_t addExtTimestampReqBytes = 8;
constexpr size_t addExtTimestampRespBytes = 1;

constexpr uint8_t numCommandRetries = 3;
} // namespace mctp_vdm

namespace pldm
{
constexpr auto Service = "xyz.openbmc_project.PLDM";
constexpr auto Path = "/";
} // namespace pldm
