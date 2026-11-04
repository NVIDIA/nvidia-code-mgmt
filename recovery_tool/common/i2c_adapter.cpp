// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

#include "i2c_utils.hpp"

namespace recovery_tool::i2c_utils
{

int openI2cAdapter(unsigned int bus)
{
    const auto path = "/dev/i2c-" + std::to_string(bus);
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        const int error = errno;
        throw I2cOpenError(error, path);
    }
    return fd;
}

} // namespace recovery_tool::i2c_utils
