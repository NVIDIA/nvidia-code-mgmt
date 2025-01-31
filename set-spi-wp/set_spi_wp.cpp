/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include <fcntl.h>
#include <getopt.h>
#include <linux/spi/spidev.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>

#define BPL_BIT 7
#define WPP_BIT 4
#define BP0_BIT 2

void print_usage(const char* program_name)
{
    std::cout
        << "Usage: " << program_name
        << " -d <SPI_DEVICE> [-a <assert|deassert>] [-r] [-t <num of bytes to read> -o <24-bit hexoffset to read from>]"
        << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout
        << "  -d <SPI_DEVICE> : Specify the SPI device (e.g., /dev/spidev0.0)"
        << std::endl;
    std::cout
        << "  -a <assert|deassert> : Assert or deassert write-protect bits"
        << std::endl;
    std::cout << "  -r : Read the status register only, without modifying it"
              << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program_name << " -d /dev/spidev0.0 -a assert"
              << std::endl;
    std::cout << "  " << program_name << " -d /dev/spidev0.0 -r" << std::endl;
    std::cout << "  " << program_name << " -d /dev/spidev0.0 -t 4 -o 0x00A0A0"
              << std::endl;
}

static int spi_xfer(int fd, unsigned char* txdata, int txlen,
                    unsigned char* rxdata, int rxlen, bool deassert)
{
    struct spi_ioc_transfer spi = {};
    int ret;

    memset(&spi, 0, sizeof(spi));

    spi.tx_buf = (unsigned long)txdata;
    spi.rx_buf = (unsigned long)rxdata;
    spi.len = txlen;
    spi.delay_usecs = rxlen;
    spi.cs_change = deassert;
    spi.bits_per_word = 8;
    ret = ioctl(fd, SPI_IOC_MESSAGE(1), &spi);

    return ret;
}

int read_data(int fd, unsigned char* data, int len, unsigned char* addr)
{
    unsigned char txdata[100] = {0x03, 0x00, 0x00, 0x00};

    memset(&txdata[4], 0x00, 95);

    memcpy(&txdata[1], addr, 3);

    return spi_xfer(fd, txdata, 4 + len, data, len, true);
}

int write_enable(int fd)
{
    uint8_t txdata = 0x6;
    return spi_xfer(fd, &txdata, 1, NULL, 0, true);
}

void write_disable(int fd)
{
    uint8_t txdata = 0x4;
    spi_xfer(fd, &txdata, 1, NULL, 0, true);
    close(fd);
}

int read_status_register(int fd, [[maybe_unused]] uint8_t* statusReg)
{
    uint8_t txdata[3] = {0x05, 0x00, 0x00};

    return spi_xfer(fd, txdata, 3, statusReg, 2, true);
}

int write_status_register(int fd, uint8_t statusReg)
{
    uint8_t cmd[2] = {0x01, statusReg};

    return spi_xfer(fd, cmd, 2, NULL, 0, true);
}

int main(int argc, char* argv[])
{
    const char* spi_device = nullptr;
    std::string wp_action;
    int data_len = 0;
    bool read_only = false;
    bool read_data_only = false;
    int opt;
    unsigned char hex_addr[3] = {0};

    while ((opt = getopt(argc, argv, "d:a:rt:o:")) != -1)
    {
        switch (opt)
        {
            case 'd':
                spi_device = optarg;
                break;
            case 'a':
                wp_action = optarg;
                break;
            case 'r':
                read_only = true;
                break;
            case 't':
                read_data_only = true;
                data_len = std::stoi(optarg);
                if (data_len > 90)
                {
                    std::cerr << "Error: Cannot read more than 90 bytes"
                              << std::endl;
                    return 1;
                }
                std::cout << "Will read " << data_len << " bytes" << std::endl;
                break;
            case 'o':
            {
                unsigned long hex_value = std::strtoul(optarg, nullptr, 16);

                if (hex_value > 0xFFFFFF)
                {
                    std::cerr
                        << "Error: Input must be a 24-bit hex number (maximum 0xFFFFFF)."
                        << std::endl;
                    return 1;
                }

                // Split into 3 bytes and store them in hex_data
                hex_addr[0] = (hex_value >> 16) & 0xFF; // Most significant byte
                hex_addr[1] = (hex_value >> 8) & 0xFF;  // Middle byte
                hex_addr[2] = hex_value & 0xFF; // Least significant byte

                std::cout << "Hex addr parsed as bytes: "
                          << "0x" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<int>(hex_addr[0])
                          << " "
                          << "0x" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<int>(hex_addr[1])
                          << " "
                          << "0x" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<int>(hex_addr[2])
                          << std::endl;
                break;
            }
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    int fd = open(spi_device, O_RDWR);
    if (fd < 0)
    {
        std::cerr << "Failed to open SPI device " << spi_device << ": "
                  << strerror(errno) << std::endl;
        return 1;
    }

    write_enable(fd);
    uint8_t statusReg[2] = {0}; // Array to store the two status register bytes
    if (read_status_register(fd, statusReg) < 0)
    {
        write_disable(fd);
        return 1;
    }

    if (read_only)
    {
        std::cout << "Status Register Byte 1: 0x" << std::hex
                  << static_cast<int>(statusReg[0]) << std::endl;
        std::cout << "Status Register Byte 2: 0x" << std::hex
                  << static_cast<int>(statusReg[1]) << std::endl;
        write_disable(fd);
        return 0;
    }

    if (read_data_only)
    {
        unsigned char data[64];
        read_data(fd, data, data_len, hex_addr);
        for (int i = 0; i < data_len; i++)
        {
            std::cout << "Byte " << i << " is: 0x" << std::hex << std::setw(2)
                      << std::setfill('0')
                      << (static_cast<int>(static_cast<unsigned char>(data[i])))
                      << std::endl;
        }
        write_disable(fd);
        return 0;
    }

    // check if WP is set
    bool assert_wp = (wp_action == "assert");
    if (!(statusReg[0] & 0x10) && !assert_wp)
    {
        std::cout << "WP bit is asserted!!!" << std::endl;
        return -1;
    }

    // Modify the write-protect bits based on user input
    if (assert_wp)
    {
        statusReg[0] |= (1 << BP0_BIT | 1 << BPL_BIT);
    }
    else
    {
        statusReg[0] &= ~(1 << BP0_BIT);
    }

    if (write_status_register(fd, statusReg[0]) < 0)
    {
        write_disable(fd);
        return 1;
    }

    if (read_status_register(fd, statusReg) < 0)
    {
        write_disable(fd);
        return 1;
    }
    write_disable(fd);
    return 0;
}
