#include "em_recovery_config.hpp"

#include <gtest/gtest.h>

using namespace nvidia::software::updater;
namespace rec = nvidia::software::updater::em_recovery;

namespace
{
constexpr auto plat = "/xyz/openbmc_project/inventory/system/platform/P";
constexpr auto policy =
    "/xyz/openbmc_project/inventory/system/platform/P/RecoveryPolicy";
const std::string nv = rec::nvDeviceIface;
const std::string tr = rec::transportIface;
const std::string rc = rec::deviceRecoveryIface;

class EmRecoveryConfigTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        rec::usbBusResolver = [](const std::string&) { return "1"; };
        objects[policy] = {
            {rec::platformIface, {{"NamePrefix", std::string("")}}}};
    }

    void device(const std::string& name, const std::string& kind, uint64_t eid,
                PropertyMap transport = {}, PropertyMap recovery = {},
                bool hasRecovery = true, uint64_t instance = 0)
    {
        InterfaceMap ifaces{{nv,
                             {{"Name", name},
                              {"Kind", kind},
                              {"EID", eid},
                              {"Instance", instance}}}};
        if (!transport.empty())
        {
            ifaces[tr] = std::move(transport);
        }
        if (hasRecovery)
        {
            ifaces[rc] = std::move(recovery);
        }
        objects[std::string(plat) + "/" + name] = std::move(ifaces);
    }

    const PropertyMap* record(const std::string& type, const std::string& name)
    {
        rec::expand(objects);
        auto it =
            objects.find(std::string(plat) + "/" + rec::pathSegment(name));
        if (it == objects.end())
        {
            return nullptr;
        }
        auto r = it->second.find(std::string(rec::cfgPrefix) + type);
        return r == it->second.end() ? nullptr : &r->second;
    }

    ObjectValueTree objects;
};
} // namespace

TEST_F(EmRecoveryConfigTest, McuUsbDerivesFromKindAndTransport)
{
    device(
        "IO_Board_SMA_0", "cx_sma", 60,
        {{"Medium", std::string("usb")},
         {"Port", std::string("1.2.1.4.1")},
         {"RootHubPath", std::string("/hub")}},
        {{"RecoveryGpio", std::string("R")}, {"ResetGpio", std::string("S")}});
    auto* r = record("MCURecovery", "FW_IO_Board_SMA_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("Target")), "CX9");
    EXPECT_EQ(std::get<std::string>(r->at("USBPort")), "1-1.2.1.4.1");
    EXPECT_EQ(std::get<std::string>(r->at("Interface")), "USB");
    EXPECT_EQ(std::get<uint64_t>(r->at("ProductId")), 0xCF11U);
    EXPECT_EQ(std::get<uint64_t>(r->at("MctpEID")), 60U);
    EXPECT_FALSE(r->contains("ResetGpioPolarity"));
}

TEST_F(EmRecoveryConfigTest, McuTargetAndPolarityOverrides)
{
    device("MGX_NVSwitch_SMA_0", "nvswitch_sma", 72,
           {{"Medium", std::string("usb")}, {"Port", std::string("1.4.1")}},
           {{"RecoveryGpio", std::string("R")},
            {"ResetGpio", std::string("S")},
            {"Target", std::string("QM4")},
            {"ResetGpioPolarity", std::string("ActiveHigh")}});
    auto* r = record("MCURecovery", "FW_MGX_NVSwitch_SMA_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("Target")), "QM4");
    EXPECT_EQ(std::get<std::string>(r->at("ResetGpioPolarity")), "ActiveHigh");
}

TEST_F(EmRecoveryConfigTest, McuI2cUsesTransportBusAndAddress)
{
    device(
        "HPM_SMA_0", "hmc_hpm_sma", 80,
        {{"Medium", std::string("i2c")},
         {"Bus", uint64_t(3)},
         {"Address", uint64_t(82)}},
        {{"RecoveryGpio", std::string("R")}, {"ResetGpio", std::string("S")}});
    auto* r = record("MCURecovery", "FW_HPM_SMA_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("Interface")), "I2C");
    EXPECT_EQ(std::get<uint64_t>(r->at("I2CBus")), 3U);
    EXPECT_EQ(std::get<uint64_t>(r->at("NormalI2CAddress")), 82U);
    EXPECT_EQ(std::get<uint64_t>(r->at("RecoveryI2CAddress")), 0x10U);
    EXPECT_EQ(std::get<std::string>(r->at("Target")), "HPM");
}

TEST_F(EmRecoveryConfigTest, ConnectXForcesTheIRoTUnlessThePlatformSaysNot)
{
    device("CX_0", "cx9_io", 61,
           {{"Medium", std::string("bridge")}, {"BridgeEID", uint64_t(60)}},
           {{"I2CBus", uint64_t(78)}, {"I2CAddress", uint64_t(72)}});
    auto* r = record("ConnectXRecovery", "FW_CX_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("ChassisName")), "IRoT_CX_0");
    EXPECT_EQ(std::get<std::string>(r->at("ForceRecoveryChassisObject")),
              "IRoT_CX_0");
    EXPECT_EQ(std::get<uint64_t>(r->at("SMAEID")), 60U);
    EXPECT_EQ(std::get<uint64_t>(r->at("EID")), 61U);
    EXPECT_TRUE(std::get<bool>(r->at("hasChassisPowerSource")));
}

TEST_F(EmRecoveryConfigTest, PlatformPolicyAndPrefix)
{
    objects[policy][rec::platformIface]["NamePrefix"] = std::string("HGX_");
    objects[policy][rec::platformIface].merge(
        PropertyMap{{"ConnectXForceRecoveryOnIRoT", false},
                    {"OcpInfoRom", false},
                    {"OcpSmaEid", false}});
    device("HGX_ConnectX_0", "cx9_io", 17,
           {{"Medium", std::string("bridge")}, {"BridgeEID", uint64_t(16)}},
           {{"I2CBus", uint64_t(110)}});
    device("HGX_GPU_0", "gpu", 49,
           {{"Medium", std::string("bridge")},
            {"Bridge", std::string("HGX_SXM_SMA_0")},
            {"BridgeEID", uint64_t(48)},
            {"BridgePort", std::string("1.2.1")}},
           {{"OcpRecoveryI2cAddress", uint64_t(105)}});
    auto* cx = record("ConnectXRecovery", "HGX_FW_ConnectX_0");
    ASSERT_NE(cx, nullptr);
    EXPECT_EQ(std::get<std::string>(cx->at("ChassisName")),
              "HGX_IRoT_ConnectX_0");
    EXPECT_EQ(std::get<std::string>(cx->at("ForceRecoveryChassisObject")),
              "HGX_ConnectX_0");
    auto* gpu = record("OCPRecovery", "HGX_FW_GPU_0");
    ASSERT_NE(gpu, nullptr);
    EXPECT_FALSE(gpu->contains("InfoRomName"));
    EXPECT_FALSE(gpu->contains("SMAEID"));
    EXPECT_EQ(std::get<std::string>(gpu->at("USBPort")), "1-1.2.1");
}

TEST_F(EmRecoveryConfigTest, ImplicitKindsNeedNoRecoveryObject)
{
    device("ERoT_BMC_0", "erot", 50, {}, {}, false);
    device("HGX_CPU_0", "cpu", 14,
           {{"Medium", std::string("usb")}, {"Port", std::string("1.1.1.2")}},
           {}, false, 1);
    auto* e = record("GlacierCrisisRecovery", "FW_ERoT_BMC_0");
    ASSERT_NE(e, nullptr);
    EXPECT_FALSE(std::get<bool>(e->at("isRecoverable")));
    auto* c = record("USBRCMRecovery", "FW_HGX_CPU_0");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(std::get<std::string>(c->at("FMCComponentName")), "SBIOS_FMC_1");
}

TEST_F(EmRecoveryConfigTest, KindWithoutRecoveryObjectIsNotARecoveryDevice)
{
    device("NVSwitch_0", "nvswitch", 79, {}, {}, false);
    EXPECT_EQ(record("NVSwitchRecovery", "FW_NVSwitch_0"), nullptr);
}

TEST_F(EmRecoveryConfigTest, GpioErotNamesComeFromTheRecoveryObject)
{
    device("HMC_ERoT_BMC_0", "hmc_erot", 51, {},
           {{"Method", std::string("GPIOERoT")},
            {"SoftwareName", std::string("FW_RECOVERY_HGX_FW_ERoT_BMC_0")},
            {"ApName", std::string("FW_RECOVERY_HGX_FW_BMC_0")},
            {"FatalErrorGpio", std::string("G")},
            {"FatalErrorPolarity", std::string("ActiveLow")},
            {"I2CBus", uint64_t(6)},
            {"I2CAddress", uint64_t(82)},
            {"Target", std::string("enable-hmc-recovery.service")}});
    auto* r = record("GPIOERoTRecovery", "FW_RECOVERY_HGX_FW_ERoT_BMC_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("MonitorMode")), "Polling");
    EXPECT_EQ(std::get<std::string>(r->at("GPIO")), "G");
    EXPECT_EQ(std::get<uint64_t>(r->at("MctpEID")), 51U);
    EXPECT_TRUE(std::get<bool>(r->at("HideWhenHealthy")));
}

TEST_F(EmRecoveryConfigTest, ForceRecoveryRecordFromPlatformPolicy)
{
    objects[policy][rec::platformIface]["TopChassis"] =
        std::string("Chassis_0");
    objects[policy][rec::platformIface].merge(
        PropertyMap{{"CpuRecoveryConfig", std::string("c2")},
                    {"CpuRecoveryChassis", std::string("BMC_0")}});
    auto* r = record("USBRCMForceRecovery", "Chassis_0_CPU_Recovery");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("ConfigType")), "c2");
    EXPECT_EQ(std::get<std::string>(r->at("ForceRecoveryChassisObject")),
              "BMC_0");
}

TEST_F(EmRecoveryConfigTest,
       SnapshotWithoutTopologyIsUntouchedAndExpandIsIdempotent)
{
    ObjectValueTree legacy{{"/a/b",
                            {{"xyz.openbmc_project.Configuration.MCURecovery",
                              {{"Name", std::string("FW_X")}}}}}};
    auto copy = legacy;
    rec::expand(legacy);
    EXPECT_EQ(legacy, copy);

    device("ERoT_BMC_0", "erot", 50, {}, {}, false);
    rec::expand(objects);
    const auto size = objects.size();
    rec::expand(objects);
    EXPECT_EQ(objects.size(), size);
}

TEST_F(EmRecoveryConfigTest, WaitsForTheTransportInterface)
{
    // The Transport interface is published after the NvDevice and Recovery ones
    device(
        "IO_Board_SMA_0", "cx_sma", 60, {},
        {{"RecoveryGpio", std::string("R")}, {"ResetGpio", std::string("S")}});
    EXPECT_EQ(record("MCURecovery", "FW_IO_Board_SMA_0"), nullptr);
    objects[std::string(plat) + "/IO_Board_SMA_0"][tr] = {
        {"Medium", std::string("usb")}, {"Port", std::string("1.2")}};
    EXPECT_NE(record("MCURecovery", "FW_IO_Board_SMA_0"), nullptr);
}

TEST(EmRecoveryConfigSignal, TriggersOnTopologyObjectsOnly)
{
    InterfaceMap nvOnly{{nv, {{"Kind", std::string("cx_sma")}}}};
    EXPECT_FALSE(rec::isConfigObject(nvOnly));
    InterfaceMap withRecovery{{nv, {{"Kind", std::string("cx_sma")}}},
                              {rc, {}}};
    EXPECT_TRUE(rec::isConfigObject(withRecovery));
    InterfaceMap implicit{{nv, {{"Kind", std::string("erot")}}}};
    EXPECT_TRUE(rec::isConfigObject(implicit));
    InterfaceMap platformObj{{rec::platformIface, {}}};
    EXPECT_TRUE(rec::isConfigObject(platformObj));
}

namespace
{
constexpr auto spiPath =
    "/xyz/openbmc_project/inventory/system/module/BootFlash_0/BootSPI_0";

InterfaceMap spiRecord()
{
    return {{rec::spiIface,
             {{"Name", std::string("BootSPI_0")},
              {"Type", std::string("SPI")},
              {"Device", std::string("SMA_0")},
              {"MuxGpio", std::string("MUX0")},
              {"ChipSelect", uint64_t{0}}}}};
}
} // namespace

TEST_F(EmRecoveryConfigTest, SpiIsTranslatedWithoutThePolicyRecord)
{
    objects.erase(policy);
    device("SMA_0", "cx_sma", 60,
           {{"Medium", std::string("usb")},
            {"Port", std::string("1.1.1.1")},
            {"RootHubPath", std::string("/hub")}},
           {}, false);
    objects[spiPath] = spiRecord();
    rec::expand(objects);
    const auto& spi = objects[spiPath];
    ASSERT_TRUE(spi.contains(rec::spiIface));
    EXPECT_EQ(std::get<std::string>(spi.at(rec::spiIface).at("USBPort")),
              "1-1.1.1.1");
    EXPECT_EQ(std::get<std::string>(spi.at(rec::spiIface).at("Programmer")),
              "nv_sma_spi");
    EXPECT_TRUE(spi.contains(std::string(rec::spiIface) + ".Gpio0"));
}

TEST_F(EmRecoveryConfigTest, SpiIsWithheldUntilItsDeviceIsPublished)
{
    objects[spiPath] = spiRecord();
    objects[spiPath][std::string(rec::spiIface) + ".Gpio0"] = {};
    rec::expand(objects);
    EXPECT_FALSE(objects[spiPath].contains(rec::spiIface));
    EXPECT_FALSE(
        objects[spiPath].contains(std::string(rec::spiIface) + ".Gpio0"));
    // The device without its transport is still not enough.
    device("SMA_0", "cx_sma", 60, {}, {}, false);
    objects[spiPath] = spiRecord();
    rec::expand(objects);
    EXPECT_FALSE(objects[spiPath].contains(rec::spiIface));
}

TEST(EmRecoveryConfigSpiTrigger, TransportCompletesAWithheldSpiRecord)
{
    InterfaceMap transport{{rec::transportIface, {}}};
    EXPECT_TRUE(rec::completesSpi(transport));
    InterfaceMap other{{rec::nvDeviceIface, {}}};
    EXPECT_FALSE(rec::completesSpi(other));
}

TEST_F(EmRecoveryConfigTest, McuChassisNameFollowsThePlatformPolicy)
{
    device(
        "IO_Board_SMA_0", "cx_sma", 60,
        {{"Medium", std::string("usb")},
         {"Port", std::string("1.2.1.4.1")},
         {"RootHubPath", std::string("/hub")}},
        {{"RecoveryGpio", std::string("R")}, {"ResetGpio", std::string("S")}});
    auto* r = record("MCURecovery", "FW_IO_Board_SMA_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("ChassisName")), "IO_Board_SMA_0");

    objects[policy][rec::platformIface]["McuChassisName"] = false;
    objects.erase(std::string(plat) + "/FW_IO_Board_SMA_0");
    rec::derivedRecords().clear();
    r = record("MCURecovery", "FW_IO_Board_SMA_0");
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(r->contains("ChassisName"));
}
