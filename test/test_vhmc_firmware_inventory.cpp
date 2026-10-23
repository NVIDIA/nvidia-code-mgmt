/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#include "../src/vhmc_updater.hpp"

#include <string>
#include <tuple>

using ::testing::NiceMock;
using namespace nvidia::software::updater;

namespace
{

/** @brief Publishes the vHMC firmware inventory against a mocked bus. */
class VHMCFirmwareInventoryTest : public ::testing::Test
{
  protected:
    NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&sdbusMock);

    /** @brief D-Bus path the inventory is published at. */
    std::string objPath()
    {
        return std::string(VHMC_FIRMWARE_INVENTORY_OBJPATH) + "/" +
               VHMC_FIRMWARE_INVENTORY_ID;
    }
};

/** @brief Only this association makes the mapper create the updateable path. */
TEST_F(VHMCFirmwareInventoryTest, AdvertisesTheInventoryAsUpdateable)
{
    VHMCFirmwareInventory inventory(bus, objPath());

    inventory.associations(
        {{"software_version", "updateable", VHMC_FIRMWARE_INVENTORY_OBJPATH}});

    ASSERT_EQ(inventory.associations().size(), 1U);
    const auto& association = inventory.associations().front();
    EXPECT_EQ(std::get<0>(association), "software_version");
    EXPECT_EQ(std::get<1>(association), "updateable");
    EXPECT_EQ(std::get<2>(association), VHMC_FIRMWARE_INVENTORY_OBJPATH);
}

/** @brief The purpose drives the Redfish Description, so it must stay Other. */
TEST_F(VHMCFirmwareInventoryTest, KeepsTheOtherVersionPurpose)
{
    VHMCFirmwareInventory inventory(bus, objPath());

    EXPECT_EQ(inventory.purpose(), sdbusplus::xyz::openbmc_project::Software::
                                       server::Version::VersionPurpose::Other);
}

} // namespace
