#pragma once

#include <string>

namespace test::fw_status_fake_usb_i2c
{

inline int mappedBus = 12;

} // namespace test::fw_status_fake_usb_i2c

namespace recovery_tool::usb_i2c
{

inline int getI2CBusFromUSBPort(const std::string&, bool)
{
    return test::fw_status_fake_usb_i2c::mappedBus;
}

} // namespace recovery_tool::usb_i2c
