#include "em_recovery_config.hpp"

#include <gtest/gtest.h>

using namespace nvidia::software::updater;
namespace rec = nvidia::software::updater::em_recovery;

namespace
{
constexpr auto plat = "/xyz/openbmc_project/inventory/system/platform/P";
const std::string nv = rec::nvDeviceIface;
const std::string tr = rec::transportIface;
const std::string rc = rec::deviceRecoveryIface;

class EmRecoveryConfigTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        rec::usbBusResolver = [](const std::string&) { return "1"; };
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
    device("IO_Board_SMA_0", "cx_sma", 60,
           {{"Medium", std::string("usb")},
            {"Port", std::string("1.2.1.4.1")},
            {"RootHubPath", std::string("/hub")}},
           {{"RecoveryType", std::string("MCU")},
            {"RecoveryGpio", std::string("R")},
            {"ResetGpio", std::string("S")}});
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
           {{"RecoveryType", std::string("MCU")},
            {"RecoveryGpio", std::string("R")},
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
    device("HPM_SMA_0", "hmc_hpm_sma", 80,
           {{"Medium", std::string("i2c")},
            {"Bus", uint64_t(3)},
            {"Address", uint64_t(82)}},
           {{"RecoveryType", std::string("MCU")},
            {"RecoveryGpio", std::string("R")},
            {"ResetGpio", std::string("S")}});
    auto* r = record("MCURecovery", "FW_HPM_SMA_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("Interface")), "I2C");
    EXPECT_EQ(std::get<uint64_t>(r->at("I2CBus")), 3U);
    EXPECT_EQ(std::get<uint64_t>(r->at("NormalI2CAddress")), 82U);
    EXPECT_EQ(std::get<uint64_t>(r->at("RecoveryI2CAddress")), 0x10U);
    EXPECT_EQ(std::get<std::string>(r->at("Target")), "HPM");
}

TEST_F(EmRecoveryConfigTest, ConnectXForcesTheDeviceUnlessItNamesTheIRoT)
{
    device("CX_0", "cx9_io", 61,
           {{"Medium", std::string("bridge")}, {"BridgeEID", uint64_t(60)}},
           {{"RecoveryType", std::string("ConnectX")},
            {"I2CBus", uint64_t(78)},
            {"I2CAddress", uint64_t(72)}});
    device("CX_1", "cx9_io", 63,
           {{"Medium", std::string("bridge")}, {"BridgeEID", uint64_t(60)}},
           {{"RecoveryType", std::string("ConnectX")},
            {"ForceRecoveryOnIRoT", true},
            {"I2CBus", uint64_t(78)},
            {"I2CAddress", uint64_t(74)}});
    auto* r = record("ConnectXRecovery", "FW_CX_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("ChassisName")), "IRoT_CX_0");
    EXPECT_EQ(std::get<std::string>(r->at("ForceRecoveryChassisObject")),
              "CX_0");
    EXPECT_EQ(std::get<uint64_t>(r->at("SMAEID")), 60U);
    EXPECT_EQ(std::get<uint64_t>(r->at("EID")), 61U);
    EXPECT_TRUE(std::get<bool>(r->at("hasChassisPowerSource")));
    auto* irot = record("ConnectXRecovery", "FW_CX_1");
    ASSERT_NE(irot, nullptr);
    EXPECT_EQ(std::get<std::string>(irot->at("ForceRecoveryChassisObject")),
              "IRoT_CX_1");
}

TEST_F(EmRecoveryConfigTest, ConnectXNamesItsUsbSmaButNVSwitchDoesNot)
{
    device("SMA_0", "cx_sma", 16,
           {{"Medium", std::string("usb")},
            {"Port", std::string("1.1.1")},
            {"RootHubPath", std::string("/hub")}});
    for (const char* type : {"ConnectX", "NVSwitch"})
    {
        device(std::string(type) + "_0", "x", 17,
               {{"Medium", std::string("bridge")},
                {"Bridge", std::string("SMA_0")},
                {"BridgeEID", uint64_t(16)}},
               {{"RecoveryType", std::string(type)}});
    }
    auto* cx = record("ConnectXRecovery", "FW_ConnectX_0");
    ASSERT_NE(cx, nullptr);
    EXPECT_EQ(std::get<std::string>(cx->at("SMAName")), "SMA_0");
    EXPECT_EQ(std::get<std::string>(cx->at("SMAUSBPort")), "1-1.1.1");
    auto* sw = record("NVSwitchRecovery", "FW_NVSwitch_0");
    ASSERT_NE(sw, nullptr);
    EXPECT_FALSE(sw->contains("SMAName"));
    EXPECT_FALSE(sw->contains("SMAUSBPort"));
}

TEST_F(EmRecoveryConfigTest, ExplicitNamesReplaceAPlatformPrefix)
{
    device("HGX_ConnectX_0", "cx9_io", 17,
           {{"Medium", std::string("bridge")}, {"BridgeEID", uint64_t(16)}},
           {{"RecoveryType", std::string("ConnectX")},
            {"SoftwareName", std::string("HGX_FW_ConnectX_0")},
            {"ChassisName", std::string("HGX_IRoT_ConnectX_0")},
            {"I2CBus", uint64_t(110)}});
    device("HGX_GPU_0", "gpu", 49,
           {{"Medium", std::string("bridge")},
            {"Bridge", std::string("HGX_SXM_SMA_0")},
            {"BridgeEID", uint64_t(48)},
            {"BridgePort", std::string("1.2.1")}},
           {{"RecoveryType", std::string("OCP")},
            {"SoftwareName", std::string("HGX_FW_GPU_0")},
            {"ChassisName", std::string("HGX_IRoT_GPU_0")},
            {"OcpInfoRom", false},
            {"OcpSmaEid", false},
            {"OcpRecoveryI2cAddress", uint64_t(105)}});
    device("HGX_GPU_1", "gpu", 50,
           {{"Medium", std::string("bridge")},
            {"Bridge", std::string("HGX_SXM_SMA_0")},
            {"BridgeEID", uint64_t(48)},
            {"BridgePort", std::string("1.2.2")}},
           {{"RecoveryType", std::string("OCP")},
            {"SoftwareName", std::string("HGX_FW_GPU_1")},
            {"ChassisName", std::string("HGX_IRoT_GPU_1")},
            {"InfoRomName", std::string("HGX_InfoROM_GPU_1")},
            {"OcpRecoveryI2cAddress", uint64_t(105)}});
    auto* cx = record("ConnectXRecovery", "HGX_FW_ConnectX_0");
    ASSERT_NE(cx, nullptr);
    EXPECT_EQ(std::get<std::string>(cx->at("ChassisName")),
              "HGX_IRoT_ConnectX_0");
    EXPECT_EQ(std::get<std::string>(cx->at("ForceRecoveryChassisObject")),
              "HGX_ConnectX_0");
    auto* gpu = record("OCPRecovery", "HGX_FW_GPU_0");
    ASSERT_NE(gpu, nullptr);
    EXPECT_EQ(std::get<std::string>(gpu->at("ChassisName")), "HGX_IRoT_GPU_0");
    EXPECT_FALSE(gpu->contains("InfoRomName"));
    EXPECT_FALSE(gpu->contains("SMAEID"));
    EXPECT_EQ(std::get<std::string>(gpu->at("USBPort")), "1-1.2.1");
    auto* gpu1 = record("OCPRecovery", "HGX_FW_GPU_1");
    ASSERT_NE(gpu1, nullptr);
    EXPECT_EQ(std::get<std::string>(gpu1->at("InfoRomName")),
              "HGX_InfoROM_GPU_1");
    EXPECT_EQ(std::get<uint64_t>(gpu1->at("SMAEID")), 48U);
}

TEST_F(EmRecoveryConfigTest, UsbRcmAndOcpNamesDefaultToTheDevice)
{
    device("CPU_0", "cpu", 14,
           {{"Medium", std::string("usb")}, {"Port", std::string("1.1.1.2")}},
           {{"RecoveryType", std::string("USBRCM")}}, true, 1);
    auto* c = record("USBRCMRecovery", "FW_CPU_0");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(std::get<std::string>(c->at("FMCComponentName")), "SBIOS_FMC_1");
    EXPECT_EQ(std::get<std::string>(c->at("FWSComponentName")), "SBIOS_FW_1");
}

TEST_F(EmRecoveryConfigTest, RecoveryTypeAloneMakesARecoveryDevice)
{
    device("ERoT_BMC_0", "erot", 50, {},
           {{"RecoveryType", std::string("GlacierCrisis")}});
    device("HGX_CPU_0", "cpu", 14,
           {{"Medium", std::string("usb")}, {"Port", std::string("1.1.1.2")}},
           {{"RecoveryType", std::string("USBRCM")}}, true, 1);
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
    device("ERoT_BMC_0", "erot", 50, {}, {}, false);
    EXPECT_EQ(record("NVSwitchRecovery", "FW_NVSwitch_0"), nullptr);
    EXPECT_EQ(record("GlacierCrisisRecovery", "FW_ERoT_BMC_0"), nullptr);
}

TEST_F(EmRecoveryConfigTest, RecoveryObjectWithoutRecoveryTypeIsNotARecovery)
{
    device("NVSwitch_0", "nvswitch", 79,
           {{"Medium", std::string("bridge")}, {"BridgeEID", uint64_t(78)}},
           {{"I2CBus", uint64_t(5)}});
    EXPECT_EQ(record("NVSwitchRecovery", "FW_NVSwitch_0"), nullptr);
}

TEST_F(EmRecoveryConfigTest, GpioErotNamesComeFromTheRecoveryObject)
{
    device("HMC_ERoT_BMC_0", "hmc_erot", 51, {},
           {{"RecoveryType", std::string("GPIOERoT")},
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

PropertyMap cpuStraps(const std::string& board, const std::string& chassis)
{
    return {{"ActionRedfishChassis", chassis},
            {"SystemResetGpio", board + "_IST_SYS_RST_L-O"},
            {"ForcedRecoveryGpio", board + "_CPU_FORCED_RECOVERY_L-O"},
            {"BootDevSel0Gpio", board + "_CPU_BOOT_DEV_SEL0-O"},
            {"BootDevSel1Gpio", board + "_CPU_BOOT_DEV_SEL1-O"},
            {"BootDevSel2Gpio", board + "_CPU_BOOT_DEV_SEL2-O"},
            {"RecoveryType0Gpio", board + "_CPU_RECOVERY_TYPE0-O"},
            {"RecoveryType1Gpio", board + "_CPU_RECOVERY_TYPE1-O"}};
}

TEST_F(EmRecoveryConfigTest, ForceRecoveryRecordListsEveryBoardOfAChassis)
{
    device("HGX_CPU_0", "cpu", 14, {}, cpuStraps("BRD0", "HGX_Chassis_0"));
    device("HGX_CPU_1", "cpu", 24, {}, cpuStraps("BRD1", "HGX_Chassis_0"), true,
           1);
    auto* r = record("USBRCMForceRecovery", "HGX_Chassis_0_CPU_Recovery");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("ForceRecoveryChassisObject")),
              "HGX_Chassis_0");
    const std::vector<std::string> resets{"BRD0_IST_SYS_RST_L-O",
                                          "BRD1_IST_SYS_RST_L-O"};
    EXPECT_EQ(std::get<std::vector<std::string>>(r->at("ResetPins")), resets);
    const auto straps = std::get<std::vector<std::string>>(r->at("StrapPins"));
    ASSERT_EQ(straps.size(), 12U);
    EXPECT_EQ(straps[0], "BRD0_CPU_FORCED_RECOVERY_L-O");
    EXPECT_EQ(straps[5], "BRD0_CPU_RECOVERY_TYPE1-O");
    EXPECT_EQ(straps[6], "BRD1_CPU_FORCED_RECOVERY_L-O");
    const std::vector<double> active{0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    const std::vector<double> normal{1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1};
    EXPECT_EQ(std::get<std::vector<double>>(r->at("StrapActiveValues")),
              active);
    EXPECT_EQ(std::get<std::vector<double>>(r->at("StrapDefaultValues")),
              normal);
    EXPECT_FALSE(r->contains("ConfigType"));
    EXPECT_FALSE(r->contains("ActionRedfishChassis"));
}

TEST_F(EmRecoveryConfigTest, CpusOfOneBoardAreListedOnce)
{
    device("CPU_0", "cpu", 16, {}, cpuStraps("B0_M1", "BMC_0"));
    device("CPU_1", "cpu", 17, {}, cpuStraps("B0_M1", "BMC_0"), true, 1);
    auto* r = record("USBRCMForceRecovery", "BMC_0_CPU_Recovery");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::vector<std::string>>(r->at("ResetPins")).size(),
              1U);
    EXPECT_EQ(std::get<std::vector<std::string>>(r->at("StrapPins")).size(),
              6U);
}

TEST_F(EmRecoveryConfigTest, CpuWithoutActionChassisHasNoForceRecovery)
{
    device("HGX_CPU_0", "cpu", 14, {}, cpuStraps("BRD0", ""));
    EXPECT_EQ(record("USBRCMForceRecovery", "_CPU_Recovery"), nullptr);
    EXPECT_EQ(record("USBRCMForceRecovery", "HGX_Chassis_0_CPU_Recovery"),
              nullptr);
}

TEST_F(EmRecoveryConfigTest,
       SnapshotWithoutTopologyIsUntouchedAndExpandIsIdempotent)
{
    ObjectValueTree legacy{{sdbusplus::object_path("/a/b"),
                            {{"xyz.openbmc_project.Configuration.MCURecovery",
                              {{"Name", std::string("FW_X")}}}}}};
    auto copy = legacy;
    rec::expand(legacy);
    EXPECT_EQ(legacy, copy);

    device("ERoT_BMC_0", "erot", 50, {},
           {{"RecoveryType", std::string("GlacierCrisis")}});
    rec::expand(objects);
    const auto size = objects.size();
    rec::expand(objects);
    EXPECT_EQ(objects.size(), size);
}

TEST_F(EmRecoveryConfigTest, WaitsForTheTransportInterface)
{
    // The Transport interface is published after the NvDevice and Recovery ones
    device("IO_Board_SMA_0", "cx_sma", 60, {},
           {{"RecoveryType", std::string("MCU")},
            {"RecoveryGpio", std::string("R")},
            {"ResetGpio", std::string("S")}});
    EXPECT_EQ(record("MCURecovery", "FW_IO_Board_SMA_0"), nullptr);
    objects[std::string(plat) + "/IO_Board_SMA_0"][tr] = {
        {"Medium", std::string("usb")}, {"Port", std::string("1.2")}};
    EXPECT_NE(record("MCURecovery", "FW_IO_Board_SMA_0"), nullptr);
}

TEST(EmRecoveryConfigSignal, SingleSignalsThatCompleteADevice)
{
    // entity-manager sends NvDevice, Recovery and Transport in separate
    // signals.
    InterfaceMap nvOnly{{nv, {{"Kind", std::string("cx_sma")}}}};
    EXPECT_FALSE(rec::touchesConfig(nvOnly));
    InterfaceMap recoveryOnly{{rc, {}}};
    EXPECT_FALSE(rec::isConfigObject(recoveryOnly));
    EXPECT_TRUE(rec::touchesConfig(recoveryOnly));
    InterfaceMap transportOnly{{rec::transportIface, {}}};
    EXPECT_TRUE(rec::touchesConfig(transportOnly));
    InterfaceMap dfu{{rec::usbDfuIface, {}}};
    EXPECT_TRUE(rec::touchesConfig(dfu));
    InterfaceMap unrelated{{"xyz.openbmc_project.Unrelated", {}}};
    EXPECT_FALSE(rec::touchesConfig(unrelated));
}

TEST(EmRecoveryConfigSignal, TriggersOnTopologyObjectsOnly)
{
    InterfaceMap nvOnly{{nv, {{"Kind", std::string("cx_sma")}}}};
    EXPECT_FALSE(rec::isConfigObject(nvOnly));
    InterfaceMap withRecovery{{nv, {{"Kind", std::string("cx_sma")}}},
                              {rc, {}}};
    EXPECT_TRUE(rec::isConfigObject(withRecovery));
    InterfaceMap erotOnly{{nv, {{"Kind", std::string("erot")}}}};
    EXPECT_FALSE(rec::isConfigObject(erotOnly));
    InterfaceMap dfu{{rec::usbDfuIface, {}}};
    EXPECT_TRUE(rec::isConfigObject(dfu));
}

namespace
{
const sdbusplus::object_path spiPath{
    "/xyz/openbmc_project/inventory/system/module/BootFlash_0/BootSPI_0"};

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

TEST_F(EmRecoveryConfigTest, SpiIsTranslatedWithoutARecoveryDevice)
{
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

TEST_F(EmRecoveryConfigTest, McuRecordCarriesChassisName)
{
    device("IO_Board_SMA_0", "cx_sma", 60,
           {{"Medium", std::string("usb")},
            {"Port", std::string("1.2.1.4.1")},
            {"RootHubPath", std::string("/hub")}},
           {{"RecoveryType", std::string("MCU")},
            {"RecoveryGpio", std::string("R")},
            {"ResetGpio", std::string("S")}});
    auto* r = record("MCURecovery", "FW_IO_Board_SMA_0");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(std::get<std::string>(r->at("ChassisName")), "IO_Board_SMA_0");
}
