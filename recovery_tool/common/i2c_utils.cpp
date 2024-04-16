/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "i2c_utils.hpp"

namespace recovery_tool
{

namespace i2c_utils
{
bool sendI2cCmdForRead(int fd, uint16_t slaveId, std::vector<uint8_t>& commandData,
                       std::vector<uint8_t>& readData, bool verbose)
{

    if (readData.empty())
    {
        if (verbose)
        {
            std::cerr << "sendI2cCmdForRead: readData is empty \n";
        }
        return false;
    }

    if (commandData.empty())
    {
        if (verbose)
        {
            std::cerr << "sendI2cCmdForRead: commandData is empty \n";
        }
        return false;
    }

    struct i2c_rdwr_ioctl_data rdwrMsg
    {};
    struct i2c_msg msg[2]{};
    int ret = -1;

    msg[0].addr = slaveId;
    msg[0].flags = 0;
    msg[0].len = static_cast<uint16_t>(commandData.size());
    msg[0].buf = commandData.data();

    msg[1].addr = slaveId;
    msg[1].flags = I2C_M_RD;
    msg[1].len = static_cast<uint16_t>(readData.size());
    msg[1].buf = readData.data();

    rdwrMsg.msgs = msg;
    rdwrMsg.nmsgs = 2;
    if ((ret = ioctl(fd, I2C_RDWR, &rdwrMsg)) < 0)
    {
        if (verbose)
        {
            std::cerr << "ret:" << ret << "  error " << std::strerror(errno)
                      << "\n";
        }
        return false;
    }

    return true;
}

bool sendI2cCmdForWrite(int fd, uint16_t slaveId,
                        std::vector<uint8_t>& writeData, bool verbose)
{
    if (writeData.empty())
    {
        if (verbose)
        {
            std::cerr << "sendI2cCmdForWrite, writeData is empty \n";
        }
        return false;
    }

    struct i2c_rdwr_ioctl_data rdwrMsg
    {};
    struct i2c_msg msg[1]{};
    int ret = -1;

    msg[0].addr = slaveId;
    msg[0].flags = 0;
    msg[0].len = static_cast<uint16_t>(writeData.size());
    msg[0].buf = writeData.data();

    rdwrMsg.msgs = msg;
    rdwrMsg.nmsgs = 1;

    if ((ret = ioctl(fd, I2C_RDWR, &rdwrMsg)) < 0)
    {
        if (verbose)
        {
            std::cerr << "ret:" << ret << "  error " << std::strerror(errno)
                      << "\n";
        }
        return false;
    }

    return true;
}

} // namespace i2c_utils
} // namespace recovery_tool