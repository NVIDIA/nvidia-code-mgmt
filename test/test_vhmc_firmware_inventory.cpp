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

    const auto associations = inventory.associations();
    ASSERT_EQ(associations.size(), 1U);
    const auto& association = associations.front();
    EXPECT_EQ(std::get<0>(association), "software_version");
    EXPECT_EQ(std::get<1>(association), "updateable");
    EXPECT_EQ(std::get<2>(association), VHMC_FIRMWARE_INVENTORY_OBJPATH);
}

/** @brief No version purpose describes a vHMC image. */
TEST_F(VHMCFirmwareInventoryTest, KeepsTheOtherVersionPurpose)
{
    VHMCFirmwareInventory inventory(bus, objPath());

    EXPECT_EQ(inventory.purpose(), sdbusplus::xyz::openbmc_project::Software::
                                       server::Version::VersionPurpose::Other);
}

/** @brief Redfish reads Manufacturer off the Asset interface. */
TEST_F(VHMCFirmwareInventoryTest, ReportsTheConfiguredManufacturer)
{
    VHMCFirmwareInventory inventory(bus, objPath());

    EXPECT_EQ(inventory.manufacturer(), VHMC_FIRMWARE_MANUFACTURER);
}

/** @brief Redfish reads Description off the Item interface. */
TEST_F(VHMCFirmwareInventoryTest, ReportsTheConfiguredDescription)
{
    VHMCFirmwareInventory inventory(bus, objPath());

    EXPECT_EQ(inventory.description(), VHMC_FIRMWARE_DESCRIPTION);
}

/** @brief The inventory is present whenever it is published. */
TEST_F(VHMCFirmwareInventoryTest, MarksTheInventoryPresent)
{
    VHMCFirmwareInventory inventory(bus, objPath());

    EXPECT_TRUE(inventory.present());
}

} // namespace
