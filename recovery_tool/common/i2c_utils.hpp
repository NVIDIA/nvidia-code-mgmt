#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace recovery_tool
{

namespace i2c_utils
{
/**
 * @brief Sends an I2C read command to the specified slave device.
 *
 * @param fd The file descriptor for the I2C bus communication.
 * @param slaveId The 16-bit address of the slave device on the I2C bus
 * @param commandData command data written to the device before reading from the
 * device.
 * @param readData Buffer to store the data read from the slave device.
 * @param verbose Flag indicating whether to display verbose logging or not.
 * @return true if the operation was successful, false otherwise.
 */
bool sendI2cCmdForRead(int fd, uint16_t slaveId,
                       std::vector<uint8_t>& commandData,
                       std::vector<uint8_t>& readData, bool verbose);
/**
 * @brief Sends an I2C write command to the specified slave device.
 *
 * @param fd The file descriptor for the I2C bus communication.
 * @param slaveId The 16-bit address of the slave device on the I2C bus.
 * @param writeData A vector containing the data bytes to be written to the
 * slave device.
 * @param verbose Flag indicating whether to display verbose logging or not.
 * @return true if the operation was successful, false otherwise.
 */
bool sendI2cCmdForWrite(int fd, uint16_t slaveId,
                        std::vector<uint8_t>& writeData, bool verbose);

} // namespace i2c_utils
} // namespace recovery_tool