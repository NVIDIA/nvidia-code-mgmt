# flashrom-wrapper

## Requirements

Reference the requirements from
[DGXOPENBMC-18291](https://jirasw.nvidia.com/browse/DGXOPENBMC-18291)

- The BMC shall support the ability to read, write, erase images in the CPU SPI
  devices (code and data, for both sockets). A special raw CPU firmware image
  should be able to be flashed on standby (S5) power as raw writes to the CPU
  code SPI devices.
- The BMC shall only start this capability (muxing in the selected SPI chip) if
  RUN power is OFF
- If RUN Power is turned ON, whis service must FIRST deactivate and restore the
  SPI muxes to default position so the CPU can boot (make this a dependency to
  run before power on)
- The BMC shall support the ability to erase and read the Vera CPU R/W Variable
  SPI - this replaces the GB200 Redfish OEM Actions "VariableSpiErase" and
  "VariableSpiRead"
- The BMC shall support the GB200 Redfish OEM Actions "VariableSpiErase" and
  "VariableSpiRead" (instead of the HMC)

## Limitation

- Don't prohibit/block reset or power on operations, so we want this to fail out
  if user wants to power on.
