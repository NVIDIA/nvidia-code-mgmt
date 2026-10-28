// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include "mst_platform.hpp"
#include "mst_policy.hpp"
#include "mst_recovery.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <deque>
#include <format>
#include <mutex>
#include <set>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace mst
{
namespace
{

using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Not;

struct Event
{
    std::string message;
    std::string level;
    std::map<std::string, std::string> data;
};

class FakePlatform : public Platform
{
  public:
    std::map<std::string, std::map<std::string, PropertyMap>> objects;
    std::map<std::string, std::deque<std::optional<std::array<uint8_t, 4>>>>
        i2c;
    std::set<std::string> missingGpio;
    std::set<std::string> failingGpio;
    std::set<std::string> failingHigh;
    std::function<void()> onSleep;
    bool echoOps = false;
    std::set<std::string> devices;
    std::map<std::string, std::string> usb;
    std::deque<std::pair<int, std::string>> mstflint;
    std::deque<Value> bootDone;
    bool haveMstflint = true;
    bool imageReadable = true;
    std::string throwOnStat;
    bool throwOnRun = false;

    std::vector<std::string> ops;
    std::vector<Event> events;
    std::vector<std::string> out;
    std::vector<std::string> err;
    uint64_t sleptMs = 0;
    std::mutex mutex;

    void op(const std::string& s)
    {
        std::lock_guard l(mutex);
        ops.push_back(s);
        if (echoOps)
        {
            std::fprintf(stderr, "op %s\n", s.c_str());
        }
    }

    std::expected<Value, std::string>
        getProperty(const std::string&, const std::string& path,
                    const std::string& iface, const std::string& prop) override
    {
        if (iface == "xyz.openbmc_project.GpioStatus")
        {
            if (bootDone.empty())
            {
                return std::unexpected("Unknown property");
            }
            auto v = bootDone.front();
            if (bootDone.size() > 1)
            {
                bootDone.pop_front();
            }
            return v;
        }
        auto o = objects.find(path);
        if (o != objects.end())
        {
            auto i = o->second.find(iface);
            if (i != o->second.end())
            {
                auto p = i->second.find(prop);
                if (p != i->second.end())
                {
                    return p->second;
                }
            }
        }
        return std::unexpected("Unknown property or interface.");
    }

    std::expected<PropertyMap, std::string>
        getAll(const std::string&, const std::string& path,
               const std::string& iface) override
    {
        auto o = objects.find(path);
        if (o == objects.end() || !o->second.contains(iface))
        {
            return std::unexpected("Unknown object");
        }
        return o->second.at(iface);
    }

    std::expected<std::vector<std::string>, std::string>
        getSubTreePaths(const std::string& root, int32_t,
                        const std::vector<std::string>& ifaces) override
    {
        std::vector<std::string> paths;
        for (const auto& [path, ifs] : objects)
        {
            if (path.starts_with(root) && ifs.contains(ifaces.front()))
            {
                paths.push_back(path);
            }
        }
        return paths;
    }

    void createLog(const std::string& message, const std::string& level,
                   const std::map<std::string, std::string>& data) override
    {
        std::lock_guard l(mutex);
        events.push_back({message, level, data});
    }

    DevInfo statDevice(const std::string& path) override
    {
        if (path == throwOnStat)
        {
            throw std::runtime_error("boom");
        }
        bool present = devices.contains(path);
        return {present, present, present, present};
    }
    bool isReadableFile(const std::string&, bool) override
    {
        return imageReadable;
    }
    std::optional<uint64_t> fileSize(const std::string&) override
    {
        return 4096;
    }

    std::optional<std::array<uint8_t, 4>>
        i2cWriteRead(const std::string& bus, uint16_t addr,
                     const std::vector<uint8_t>&, bool force) override
    {
        op(std::format("i2c {} {} force={}", bus, addr, force ? 1 : 0));
        std::lock_guard l(mutex);
        auto& q = i2c[std::format("{}-{}", bus, addr)];
        if (q.empty())
        {
            return std::nullopt;
        }
        auto v = q.front();
        if (q.size() > 1)
        {
            q.pop_front();
        }
        return v;
    }

    bool gpioFind(const std::string& name) override
    {
        op("find " + name);
        return !missingGpio.contains(name);
    }
    bool gpioSet(const std::string& name, int value) override
    {
        op(std::format("set {}={}", name, value));
        return !failingGpio.contains(name) &&
               (value != 1 || !failingHigh.contains(name));
    }
    bool gpioHold(const std::string& name, int value) override
    {
        op(std::format("hold {}={}", name, value));
        return !failingGpio.contains(name);
    }
    void gpioRelease(const std::string& name) override
    {
        op("release " + name);
    }

    std::optional<std::string> usbAttribute(const std::string& port,
                                            const std::string& attr) override
    {
        auto it = usb.find(port + "/" + attr);
        if (it == usb.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    bool commandExists(const std::string&) override
    {
        return haveMstflint;
    }

    int runProcess(const std::vector<std::string>& argv, OutputMode,
                   const std::function<void(std::string_view)>& cb) override
    {
        if (throwOnRun)
        {
            throw std::runtime_error("boom");
        }
        std::string line;
        for (const auto& a : argv)
        {
            line += (line.empty() ? "" : " ") + a;
        }
        op(line);
        std::pair<int, std::string> r{0, ""};
        {
            std::lock_guard l(mutex);
            if (!mstflint.empty())
            {
                r = mstflint.front();
                if (mstflint.size() > 1)
                {
                    mstflint.pop_front();
                }
            }
        }
        if (cb)
        {
            cb(r.second);
        }
        return r.first;
    }

    void sleepMs(uint64_t ms) override
    {
        {
            std::lock_guard l(mutex);
            sleptMs += ms;
        }
        if (onSleep)
        {
            onSleep();
        }
    }
    int64_t nowSeconds() override
    {
        return 1000;
    }
    void writeOut(std::string_view line) override
    {
        std::lock_guard l(mutex);
        out.emplace_back(line);
    }
    void writeErr(std::string_view line) override
    {
        std::lock_guard l(mutex);
        err.emplace_back(line);
    }
};

constexpr auto root =
    "/xyz/openbmc_project/inventory/system/recovery_config/Devices";
constexpr auto cxIface = "xyz.openbmc_project.Configuration.ConnectXRecovery";
constexpr auto cx8Iface = "xyz.openbmc_project.Configuration.CX8Recovery";
const std::array<uint8_t, 4> healthyBytes{0x20, 0x00, 0x00, 0x19};
const std::array<uint8_t, 4> brokenBytes{0x00, 0x00, 0x00, 0x01};

using S = std::string;
using L = std::vector<std::string>;

// The shipped MSTRecoveryPolicy records, as Entity Manager publishes them
PropertyMap vrRecord(L groups = {"CX_0 CX_2", "CX_1"})
{
    return {{"Name", S("ConnectX_MST_Recovery_Policy")},
            {"Type", S("MSTRecoveryPolicy")},
            {"DeviceType", S("ConnectX")},
            {"RecoveryFlow", S("Mst")},
            {"RecoveryGroups", std::move(groups)},
            {"I2CForce", true},
            {"SMAPresenceCheck", false},
            {"HealthyEvent", false}};
}

PropertyMap hgxrCxRecord(L groups)
{
    auto r = vrRecord(std::move(groups));
    r["I2CForce"] = false;
    r["SMAPresenceCheck"] = true;
    r["HealthyEvent"] = true;
    return r;
}

PropertyMap hgxrRecord(const S& type, L groups)
{
    return {{"Name", S(type + "_MST_Recovery_Policy")},
            {"Type", S("MSTRecoveryPolicy")},
            {"DeviceType", type},
            {"RecoveryGroups", std::move(groups)}};
}

PropertyMap hgxb300Record(L groups)
{
    return {{"Name", S("ConnectX_MST_Recovery_Policy")},
            {"Type", S("MSTRecoveryPolicy")},
            {"DeviceType", S("ConnectX")},
            {"RecoveryFlow", S("Legacy")},
            {"RecoveryGroups", std::move(groups)}};
}

PropertyMap bf4Record()
{
    return {{"Name", S("ConnectX_MST_Recovery_Policy")},
            {"Type", S("MSTRecoveryPolicy")},
            {"DeviceType", S("ConnectX")},
            {"RecoveryFlow", S("Hold")}};
}

void addCx(FakePlatform& p, const std::string& name, uint64_t bus,
           uint64_t addr, bool gpios = true, const char* iface = cxIface)
{
    PropertyMap props{{"ChassisName", std::string("IRoT_" + name)},
                      {"I2CBus", bus},
                      {"I2CAddress", addr},
                      {"SMAEID", uint64_t{60}}};
    if (gpios)
    {
        props["ResetGPIO"] = std::string(name + "_RST");
        props["FlashNotPresentGPIO"] = std::string(name + "_FNP");
    }
    p.objects[std::string(root) + "/" + name][iface] = props;
    p.devices.insert(std::format("/dev/i2c-{}", bus));
}

class MstVrTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        addCx(p, "CX_0", 78, 72);
        addCx(p, "CX_1", 78, 74);
        addCx(p, "CX_2", 79, 72);
    }

    int run(std::vector<std::string> args = {"/tmp/img"},
            const PropertyMap& record = vrRecord(),
            const std::string& board = root)
    {
        p.objects[board + "/Policy"][policyInterface] = record;
        auto policy =
            findPolicy(p, std::get<std::string>(record.at("DeviceType")), 1, 0);
        if (!policy)
        {
            ADD_FAILURE() << policy.error();
            return -1;
        }
        Recovery r(p, *policy, "connectx-recovery.sh");
        return r.run(std::move(args));
    }

    std::vector<std::string> opsFor(const std::string& key)
    {
        std::vector<std::string> v;
        std::ranges::copy_if(p.ops, std::back_inserter(v), [&](const auto& o) {
            return o.find(key) != std::string::npos;
        });
        return v;
    }

    FakePlatform p;
};

TEST_F(MstVrTest, HealthyDevicesSkipRecovery)
{
    p.i2c["78-72"] = {healthyBytes};
    p.i2c["78-74"] = {healthyBytes};
    p.i2c["79-72"] = {healthyBytes};
    EXPECT_EQ(run(), 0);
    EXPECT_TRUE(p.events.empty());
    EXPECT_THAT(p.out,
                Contains("CX_0: booted successfully, skipping recovery"));
    EXPECT_THAT(p.out,
                Contains("recovery_main: starting group 1/2: CX_0 CX_2"));
    EXPECT_THAT(p.out,
                Contains("recovery_main: all 3 device(s) completed in 0s"));
    EXPECT_THAT(p.ops, Not(Contains(HasSubstr("mstflint"))));
}

TEST_F(MstVrTest, RecoverySuccessSequence)
{
    p.i2c["78-72"] = {brokenBytes, brokenBytes, healthyBytes};
    p.i2c["78-74"] = {healthyBytes};
    p.i2c["79-72"] = {healthyBytes};
    p.mstflint = {{0, "Burning\rDone 50%\rDone 100%\n"}};
    EXPECT_EQ(run(), 0);
    EXPECT_THAT(opsFor("CX_0_"),
                ElementsAre("find CX_0_FNP", "find CX_0_RST", "set CX_0_FNP=0",
                            "set CX_0_RST=0", "set CX_0_RST=1", "find CX_0_FNP",
                            "find CX_0_RST", "set CX_0_FNP=1", "set CX_0_RST=0",
                            "set CX_0_RST=1"));
    EXPECT_THAT(p.ops,
                Contains("mstflint --device /dev/i2c-78 --i2c_secondary 72 "
                         "--image /tmp/img --override_cache_replacement "
                         "--no_flash_verify --yes burn"));
    EXPECT_THAT(p.out, Contains("CX_0: Done 50%"));
    EXPECT_THAT(p.out,
                Contains("CX_0: waiting for boot (GIS=0x00000001, 0s/20s)"));
    ASSERT_EQ(p.events.size(), 2U);
    EXPECT_EQ(p.events[0].message, "NvidiaUpdate.1.0.RecoveryStarted");
    EXPECT_EQ(p.events[0].data.at("REDFISH_MESSAGE_ARGS"), "CX_0");
    EXPECT_EQ(p.events[0].data.at("namespace"), "FWUpdate");
    EXPECT_EQ(p.events[1].message, "NvidiaUpdate.1.0.RecoverySuccessful");
    EXPECT_THAT(opsFor("i2c 78 72"), Contains("i2c 78 72 force=1"));
}

TEST_F(MstVrTest, GroupsRunSequentially)
{
    p.i2c["78-72"] = {brokenBytes, healthyBytes};
    p.i2c["78-74"] = {brokenBytes, healthyBytes};
    p.i2c["79-72"] = {brokenBytes, healthyBytes};
    EXPECT_EQ(run(), 0);
    auto first = std::ranges::find(p.ops, "i2c 78 74 force=1");
    ASSERT_NE(first, p.ops.end());
    for (auto it = first; it != p.ops.end(); ++it)
    {
        EXPECT_EQ(it->find("CX_0"), std::string::npos);
        EXPECT_EQ(it->find("CX_2"), std::string::npos);
        EXPECT_EQ(it->find("i2c 78 72"), std::string::npos);
        EXPECT_EQ(it->find("i2c 79 72"), std::string::npos);
    }
}

TEST_F(MstVrTest, MstflintFailureDeassertsAndFails)
{
    p.i2c["78-72"] = {brokenBytes};
    p.i2c["78-74"] = {healthyBytes};
    p.i2c["79-72"] = {healthyBytes};
    p.mstflint = {{1, "-E- burn failed\n"}};
    EXPECT_EQ(run(), 1);
    EXPECT_THAT(
        p.err,
        Contains("<3>CX_0: recovery failed: burn failed, deasserting FNP"));
    EXPECT_THAT(opsFor("CX_0_FNP"), Contains("set CX_0_FNP=1"));
    EXPECT_THAT(p.err, Contains("<3>recovery_main: recovery failed: 1 of 3 "
                                "device(s) failed in 0s: CX_0"));
}

TEST_F(MstVrTest, PollTimeoutAfterElevenReads)
{
    p.i2c["78-72"] = {brokenBytes};
    p.i2c["78-74"] = {healthyBytes};
    p.i2c["79-72"] = {healthyBytes};
    EXPECT_EQ(run(), 1);
    // one pre-burn read plus 11 polls (0, 2, ... 20 s)
    EXPECT_EQ(opsFor("i2c 78 72").size(), 12U);
    EXPECT_THAT(p.err, Contains("<3>CX_0: recovery failed: boot status timeout "
                                "after burn (GIS=0x00000001)"));
}

TEST_F(MstVrTest, I2cRetryBudget)
{
    p.i2c["78-74"] = {healthyBytes};
    p.i2c["79-72"] = {healthyBytes};
    EXPECT_EQ(run(), 1);
    EXPECT_EQ(opsFor("i2c 78 72").size(), 10U);
    EXPECT_THAT(p.err, Contains("<3>CX_0: recovery failed: could not retrieve "
                                "boot status"));
}

TEST_F(MstVrTest, MissingGpioPropertiesRetryThenFail)
{
    addCx(p, "CX_0", 78, 72, false);
    p.i2c["78-74"] = {healthyBytes};
    p.i2c["79-72"] = {healthyBytes};
    EXPECT_EQ(run(), 1);
    EXPECT_EQ(std::ranges::count(
                  p.err, "Failed to get property ResetGPIO on interface "
                         "xyz.openbmc_project.Configuration."
                         "ConnectXRecovery: Unknown property or "
                         "interface."),
              10);
    EXPECT_THAT(p.err, Contains("<3>CX_0: recovery failed: failed to read "
                                "D-Bus properties"));
    EXPECT_GE(p.sleptMs, 9000U);
}

TEST_F(MstVrTest, ForceRecoversHealthyDevice)
{
    p.i2c["78-72"] = {healthyBytes};
    p.i2c["78-74"] = {healthyBytes};
    p.i2c["79-72"] = {healthyBytes};
    EXPECT_EQ(run({"-f", "/tmp/img"}), 0);
    EXPECT_THAT(p.out, Contains("CX_0: booted successfully, forcing recovery"));
    EXPECT_EQ(p.events.size(), 6U);
}

TEST_F(MstVrTest, ArgumentErrors)
{
    EXPECT_EQ(run({}), 1);
    EXPECT_THAT(p.err, Contains("usage: connectx-recovery.sh [-f | --force] "
                                "FW_IMAGE_PATH"));
    p.imageReadable = false;
    EXPECT_EQ(run({"/x"}), 1);
    EXPECT_THAT(p.err, Contains("error: could not open `/x` for reading."));
    p.imageReadable = true;
    p.haveMstflint = false;
    EXPECT_EQ(run({"/x"}), 1);
    EXPECT_THAT(p.err, Contains("<3>mstflint binary not found."));
}

TEST_F(MstVrTest, MissingBusFails)
{
    p.devices.erase("/dev/i2c-79");
    p.i2c["78-72"] = {healthyBytes};
    p.i2c["78-74"] = {healthyBytes};
    EXPECT_EQ(run(), 1);
    EXPECT_THAT(p.err, Contains("<3>CX_2: recovery failed: I2C bus "
                                "/dev/i2c-79 not present"));
}

TEST_F(MstVrTest, SmaIspModeSkips)
{
    auto props = hgxrCxRecord({"CX_0 CX_1"});
    auto& cx0 = p.objects[std::string(root) + "/CX_0"][cxIface];
    cx0["SMAName"] = std::string("SMA_0");
    cx0["SMAUSBPort"] = std::string("1-1.1.1");
    p.devices.erase("/dev/i2c-78");
    p.usb["1-1.1.1/idVendor"] = "0955";
    p.usb["1-1.1.1/idProduct"] = "CF1D";
    EXPECT_EQ(run({"/tmp/img"}, props), 1);
    EXPECT_THAT(p.out, Contains("CX_0: SMA SMA_0 at USB port 1-1.1.1 is "
                                "0955:cf1d"));
    EXPECT_THAT(p.out, Contains("CX_0: SMA SMA_0 is in ISP mode, skipping"));
    EXPECT_THAT(p.out, Contains("CX_1: SMA EID 60 is not present"));
    ASSERT_EQ(p.events.size(), 2U);
    std::ranges::sort(p.events, {}, &Event::message);
    EXPECT_EQ(p.events[0].message, "NvidiaUpdate.1.0.ComponentUpdateSkipped");
    EXPECT_EQ(p.events[1].message, "ResourceEvent.1.0.ResourceErrorsDetected");
    EXPECT_EQ(p.events[1].level,
              "xyz.openbmc_project.Logging.Entry.Level.Critical");
    EXPECT_EQ(
        p.events[1].data.at("xyz.openbmc_project.Logging.Entry.Resolution"),
        "Check device state and try recovery again.");
    EXPECT_THAT(p.out, Not(Contains(HasSubstr("starting group"))));
}

TEST_F(MstVrTest, LegacyStyleIgnoresGpioErrorsAndPostsNoEvents)
{
    addCx(p, "CX_0", 78, 72, true, cx8Iface);
    addCx(p, "CX_1", 78, 74, true, cx8Iface);
    p.missingGpio.insert("CX_0_FNP");
    p.i2c["78-72"] = {brokenBytes, healthyBytes};
    p.i2c["78-74"] = {healthyBytes};
    EXPECT_EQ(run({"/tmp/img"}, hgxb300Record({"CX_0", "CX_1"})), 0);
    EXPECT_TRUE(p.events.empty());
    EXPECT_THAT(p.out,
                Contains("IRoT_CX_0: Boot failure. Beginning recovery."));
    EXPECT_THAT(p.out, Contains("IRoT_CX_0: Recovered successfully."));
    EXPECT_THAT(p.out, Contains("IRoT_CX_1: Booted successfully. Skipping "
                                "recovery."));
    EXPECT_THAT(opsFor("CX_0_"),
                ElementsAre("find CX_0_FNP", "find CX_0_RST", "set CX_0_RST=0",
                            "find CX_0_RST", "set CX_0_RST=1", "find CX_0_FNP",
                            "find CX_0_RST", "set CX_0_RST=0", "find CX_0_RST",
                            "set CX_0_RST=1"));
}

// --- FNPMode HoldDeasserted -------------------------------------------------

constexpr auto bfPath = "/xyz/openbmc_project/inventory/system/recovery_config/"
                        "Lupine_Recovery_Devices/BlueField_FW_NIC_0";

constexpr auto bfBoard = "/xyz/openbmc_project/inventory/system/"
                         "recovery_config/Lupine_Recovery_Devices";

class MstHoldTest : public MstVrTest
{
  protected:
    void SetUp() override
    {
        p.objects[bfPath][cxIface] = {
            {"I2CBus", uint64_t{14}},
            {"I2CAddress", uint64_t{72}},
            {"ResetGPIO", std::string("rst")},
            {"FlashNotPresentGPIO", std::string("fnp")},
            {"FlashNotPresentPolarity", uint64_t{1}},
            {"BootDoneGPIO", std::string("nic_boot_done")}};
        p.devices.insert("/dev/i2c-14");
        p.bootDone = {Value{true}, Value{false}};
    }

    int run(std::vector<std::string> args = {"/tmp/img"})
    {
        return MstVrTest::run(std::move(args), bf4Record(), bfBoard);
    }
};

TEST_F(MstHoldTest, SuccessHoldsFnpAndPostsCompleted)
{
    p.i2c["14-72"] = {std::nullopt, healthyBytes};
    EXPECT_EQ(run(), 0);
    EXPECT_THAT(p.ops,
                ElementsAre("i2c 14 72 force=0", "find fnp", "hold fnp=0",
                            "mstflint --device /dev/i2c-14 --i2c_secondary "
                            "0x48 --image /tmp/img "
                            "--override_cache_replacement --yes "
                            "--no_flash_verify --silent burn",
                            "find rst", "set rst=0", "set rst=1",
                            "i2c 14 72 force=0", "release fnp"));
    ASSERT_EQ(p.events.size(), 2U);
    EXPECT_EQ(p.events[0].data.at("REDFISH_MESSAGE_ID"),
              "OpenBMC.0.1.FirmwareUpdateCompleted");
    EXPECT_FALSE(p.events[0].data.contains("namespace"));
    EXPECT_EQ(p.events[1].data.at("namespace"), "FWUpdate");
    EXPECT_EQ(p.events[1].data.at("REDFISH_MESSAGE_ARGS"),
              "BlueField_FW_NIC_0,unknown");
    EXPECT_THAT(p.err, Contains("[INFO] NIC boot done after 1s"));
    EXPECT_THAT(p.err, Contains("[INFO] Pre-burn GIS read failed - proceeding "
                                "with recovery"));
}

TEST_F(MstHoldTest, HealthySkipsWithNotInRecovery)
{
    p.i2c["14-72"] = {healthyBytes};
    EXPECT_EQ(run(), 0);
    ASSERT_EQ(p.events.size(), 1U);
    EXPECT_EQ(p.events[0].message,
              "Firmware 'BlueField_FW_NIC_0' is not in Recovery.");
    EXPECT_THAT(p.ops, Not(Contains("find fnp")));
}

TEST_F(MstHoldTest, BurnRetriesThenFailsAndReassertsFnp)
{
    p.i2c["14-72"] = {brokenBytes};
    p.mstflint = {{1, "-E- Can't find device id\r\x1b[1mmore\x1b[0m\n"}};
    EXPECT_EQ(run(), 1);
    EXPECT_EQ(opsFor("mstflint").size(), 3U);
    EXPECT_THAT(p.ops, Contains("release fnp"));
    EXPECT_EQ(p.ops.back(), "set fnp=1");
    ASSERT_EQ(p.events.size(), 2U);
    EXPECT_EQ(p.events[0].data.at("REDFISH_MESSAGE_ARGS"),
              "BlueField_FW_NIC_0,unknown,Burn failed after 3 attempt(s): -E- "
              "Can't find device id more ");
    EXPECT_EQ(p.events[1].data.at("REDFISH_MESSAGE_ARGS"),
              "BlueField_FW_NIC_0 recovery,Can't find device id more ");
    EXPECT_EQ(p.events[1].level,
              "xyz.openbmc_project.Logging.Entry.Level.Error");
}

TEST_F(MstHoldTest, BootDoneTimeout)
{
    p.i2c["14-72"] = {brokenBytes};
    p.bootDone = {Value{true}};
    EXPECT_EQ(run(), 1);
    EXPECT_THAT(p.err, Contains("[ERROR] NIC boot-done not detected within "
                                "60s"));
}

TEST_F(MstHoldTest, ConfigErrorsUseDetailedExitCodes)
{
    EXPECT_EQ(run({}), 2);
    EXPECT_EQ(p.events.at(0).data.at("DEVICE"), "unknown");
    p.objects[bfPath][cxIface]["FlashNotPresentPolarity"] = uint64_t{2};
    EXPECT_EQ(run(), 6);
    p.objects.clear();
    EXPECT_EQ(run(), 6);
    EXPECT_THAT(p.err, Contains(HasSubstr("No xyz.openbmc_project."
                                          "Configuration.ConnectXRecovery "
                                          "object found")));
}

TEST_F(MstHoldTest, NonBoolBootDoneIsConfigError)
{
    const std::vector<std::pair<Value, std::string>> cases{
        {Value{std::string("x")}, "s \"x\""},
        {Value{uint8_t{1}}, "y 1"},
        {Value{int16_t{-2}}, "n -2"},
        {Value{uint16_t{3}}, "q 3"},
        {Value{int32_t{-4}}, "i -4"},
        {Value{uint32_t{5}}, "u 5"},
        {Value{int64_t{-6}}, "x -6"},
        {Value{uint64_t{7}}, "t 7"},
        {Value{0.5}, "d 0.5"},
        {Value{std::vector<std::string>{"a", "b"}}, "a b"},
        {Value{std::vector<uint64_t>{1, 2}}, "1 2"},
        {Value{std::vector<int64_t>{-1}}, "-1"},
        {Value{std::vector<double>{1.5}}, "1.5"},
    };
    for (const auto& [value, text] : cases)
    {
        p.err.clear();
        p.i2c["14-72"] = {brokenBytes};
        p.bootDone = {value};
        EXPECT_EQ(run(), 6) << text;
        EXPECT_THAT(p.err, Contains(HasSubstr("Unexpected nic_boot_done D-Bus "
                                              "value '" +
                                              text + "'")))
            << text;
    }
}

TEST_F(MstHoldTest, MissingI2cBusIsInvalidArgumentAndReassertsFnp)
{
    p.devices.clear();
    EXPECT_EQ(run(), 2);
    EXPECT_THAT(p.ops, ElementsAre("find fnp", "set fnp=1"));
}

TEST_F(MstHoldTest, ResetDeassertFailureRetriesDeassertInCleanup)
{
    p.i2c["14-72"] = {brokenBytes};
    p.failingHigh.insert("rst");
    EXPECT_EQ(run(), 6);
    ASSERT_GE(p.ops.size(), 6U);
    std::vector<std::string> tail(p.ops.end() - 6, p.ops.end());
    EXPECT_THAT(tail, ElementsAre("set rst=0", "set rst=1", "set rst=1",
                                  "release fnp", "find fnp", "set fnp=1"));
}

TEST_F(MstHoldTest, CancelDuringResetPulseDeassertsReset)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    p.i2c["14-72"] = {brokenBytes};
    p.echoOps = true;
    p.onSleep = [this]() {
        if (!p.ops.empty() && p.ops.back() == "set rst=0")
        {
            std::raise(SIGTERM);
        }
    };
    EXPECT_EXIT(
        {
            Recovery::installSignalHandlers();
            run();
        },
        testing::KilledBySignal(SIGTERM),
        "op set rst=0\nop set rst=1\nop release fnp\nop find fnp\n"
        "op set fnp=1\n");
}

TEST_F(MstVrTest, WorkerExceptionFailsOnlyThatDevice)
{
    p.i2c["78-72"] = {healthyBytes};
    p.i2c["78-74"] = {healthyBytes};
    p.throwOnStat = "/dev/i2c-79";
    EXPECT_EQ(run(), 1);
    EXPECT_THAT(p.err, Contains("<3>CX_2: recovery failed: boom"));
    EXPECT_THAT(p.out,
                Contains("CX_0: booted successfully, skipping recovery"));
}

TEST_F(MstHoldTest, ExceptionReassertsFnp)
{
    p.i2c["14-72"] = {brokenBytes};
    p.throwOnRun = true;
    EXPECT_EQ(run(), 1);
    EXPECT_THAT(p.err, Contains("<3>mst-recovery: boom"));
    std::vector<std::string> tail(p.ops.end() - 3, p.ops.end());
    EXPECT_THAT(tail, ElementsAre("release fnp", "find fnp", "set fnp=1"));
}

// --- golden: each shipped record and the sequence its script ran -----------

class MstGoldenTest : public MstVrTest
{
  protected:
    void SetUp() override
    {}

    // <prefix>0..n-1 on buses 70+i; only the first one needs recovery
    void addDevices(const std::string& prefix, int n, const char* iface,
                    std::array<uint8_t, 4> ok = healthyBytes)
    {
        for (int i = 0; i < n; ++i)
        {
            addCx(p, prefix + std::to_string(i), 70 + i, 72, true, iface);
            p.i2c[std::format("{}-72", 70 + i)] = {i == 0 ? brokenBytes : ok,
                                                   ok};
        }
    }

    void expectMstSequence(const std::string& dev, bool force)
    {
        EXPECT_THAT(
            opsFor(dev + "_"),
            ElementsAre("find " + dev + "_FNP", "find " + dev + "_RST",
                        "set " + dev + "_FNP=0", "set " + dev + "_RST=0",
                        "set " + dev + "_RST=1", "find " + dev + "_FNP",
                        "find " + dev + "_RST", "set " + dev + "_FNP=1",
                        "set " + dev + "_RST=0", "set " + dev + "_RST=1"));
        EXPECT_THAT(p.ops, Contains("mstflint --device /dev/i2c-70 "
                                    "--i2c_secondary 72 --image /tmp/img "
                                    "--override_cache_replacement "
                                    "--no_flash_verify --yes burn"));
        EXPECT_THAT(opsFor("i2c 70 72"),
                    Each(std::format("i2c 70 72 force={}", force ? 1 : 0)));
    }

    std::vector<std::string> messages()
    {
        std::vector<std::string> m;
        for (const auto& e : p.events)
        {
            m.push_back(e.message);
        }
        std::ranges::sort(m);
        return m;
    }
};

TEST_F(MstGoldenTest, VrConnectX)
{
    addDevices("FW_CX_", 8, cxIface);
    EXPECT_EQ(run({"/tmp/img"}, vrRecord({"FW_CX_0 FW_CX_2 FW_CX_4 FW_CX_6",
                                          "FW_CX_1 FW_CX_3 FW_CX_5 FW_CX_7"})),
              0);
    expectMstSequence("FW_CX_0", true);
    EXPECT_THAT(messages(), ElementsAre("NvidiaUpdate.1.0.RecoveryStarted",
                                        "NvidiaUpdate.1.0.RecoverySuccessful"));
    EXPECT_THAT(p.out, Contains("recovery_main: starting group 2/2: FW_CX_1 "
                                "FW_CX_3 FW_CX_5 FW_CX_7"));
}

TEST_F(MstGoldenTest, HgxrConnectX)
{
    addDevices("HGX_FW_ConnectX_", 8, cxIface);
    EXPECT_EQ(run({"/tmp/img"},
                  hgxrCxRecord({"HGX_FW_ConnectX_0 HGX_FW_ConnectX_1 "
                                "HGX_FW_ConnectX_2 HGX_FW_ConnectX_3 "
                                "HGX_FW_ConnectX_4 HGX_FW_ConnectX_5 "
                                "HGX_FW_ConnectX_6 HGX_FW_ConnectX_7"})),
              0);
    expectMstSequence("HGX_FW_ConnectX_0", false);
    EXPECT_EQ(std::ranges::count(messages(),
                                 "NvidiaUpdate.1.0.FirmwareNotInRecovery"),
              7);
    EXPECT_THAT(p.out,
                Contains("HGX_FW_ConnectX_0: SMA EID 60 is not present"));
    EXPECT_THAT(p.out, Not(Contains(HasSubstr("starting group"))));
}

TEST_F(MstGoldenTest, HgxrNVSwitch)
{
    addDevices("HGX_FW_NVSwitch_", 4,
               "xyz.openbmc_project.Configuration.NVSwitchRecovery",
               {0x00, 0x00, 0x00, 0x5e});
    EXPECT_EQ(run({"/tmp/img"},
                  hgxrRecord("NVSwitch", {"HGX_FW_NVSwitch_0 HGX_FW_NVSwitch_1 "
                                          "HGX_FW_NVSwitch_2 "
                                          "HGX_FW_NVSwitch_3"})),
              0);
    expectMstSequence("HGX_FW_NVSwitch_0", false);
    EXPECT_THAT(p.out,
                Contains("HGX_FW_NVSwitch_0: global_image_status=0x00000001"));
    EXPECT_EQ(std::ranges::count(messages(),
                                 "NvidiaUpdate.1.0.FirmwareNotInRecovery"),
              3);
}

TEST_F(MstGoldenTest, HgxrNVLinkManagementNIC)
{
    addDevices("HGX_FW_NVLinkManagementNIC_", 1,
               "xyz.openbmc_project.Configuration.NVLinkManagementNICRecovery");
    EXPECT_EQ(run({"/tmp/img"}, hgxrRecord("NVLinkManagementNIC",
                                           {"HGX_FW_NVLinkManagementNIC_0"})),
              0);
    expectMstSequence("HGX_FW_NVLinkManagementNIC_0", false);
    EXPECT_THAT(messages(), ElementsAre("NvidiaUpdate.1.0.RecoveryStarted",
                                        "NvidiaUpdate.1.0.RecoverySuccessful"));
}

TEST_F(MstGoldenTest, Hgxb300ConnectX)
{
    addDevices("HGX_FW_ConnectX_", 8, cx8Iface);
    L groups;
    for (int i = 0; i < 8; ++i)
    {
        groups.push_back("HGX_FW_ConnectX_" + std::to_string(i));
    }
    EXPECT_EQ(run({"/tmp/img"}, hgxb300Record(groups)), 0);
    const std::string d = "HGX_FW_ConnectX_0_";
    EXPECT_THAT(opsFor(d),
                ElementsAre("find " + d + "FNP", "set " + d + "FNP=0",
                            "find " + d + "RST", "set " + d + "RST=0",
                            "find " + d + "RST", "set " + d + "RST=1",
                            "find " + d + "FNP", "set " + d + "FNP=1",
                            "find " + d + "RST", "set " + d + "RST=0",
                            "find " + d + "RST", "set " + d + "RST=1"));
    EXPECT_THAT(p.ops, Contains("mstflint --device /dev/i2c-70 "
                                "--i2c_secondary 72 --image /tmp/img "
                                "--override_cache_replacement --yes burn"));
    EXPECT_THAT(opsFor("i2c 70 72"), Each("i2c 70 72 force=0"));
    EXPECT_TRUE(p.events.empty());
    EXPECT_THAT(p.out,
                Contains("IRoT_HGX_FW_ConnectX_0: Recovered successfully."));
}

// The BF4 record: MstHoldTest.SuccessHoldsFnpAndPostsCompleted

// --- policy -----------------------------------------------------------------

TEST(MstPolicyTest, TypeDefaultsAndFlows)
{
    const std::string board =
        "/xyz/openbmc_project/inventory/system/recovery_config/B";
    auto nvs = parsePolicy(board + "/Policy",
                           hgxrRecord("NVSwitch", {"A B", " ", "C"}));
    ASSERT_TRUE(nvs.has_value()) << nvs.error();
    EXPECT_EQ(nvs->board, board);
    EXPECT_EQ(nvs->flow, Flow::Mst);
    EXPECT_EQ(nvs->deviceInterface,
              "xyz.openbmc_project.Configuration.NVSwitchRecovery");
    EXPECT_THAT(nvs->groups, ElementsAre(ElementsAre("A", "B"), L{"C"}));
    EXPECT_TRUE(nvs->smaPresenceCheck);
    EXPECT_TRUE(nvs->healthyEvent);
    EXPECT_FALSE(nvs->i2cForce);
    EXPECT_THAT(nvs->gisRegister, ElementsAre(0x00, 0x15, 0x20, 0x80));
    EXPECT_EQ(nvs->gisHealthyValue, 0x5eU);

    auto legacy = parsePolicy(board + "/Policy", hgxb300Record({"A"}));
    ASSERT_TRUE(legacy.has_value()) << legacy.error();
    EXPECT_EQ(legacy->deviceInterface, cx8Iface);
    auto hold = parsePolicy(board + "/Policy", bf4Record());
    ASSERT_TRUE(hold.has_value()) << hold.error();
    EXPECT_EQ(hold->flow, Flow::Hold);
    EXPECT_EQ(hold->deviceInterface, cxIface);
}

TEST(MstPolicyTest, RejectsIncompleteRecordsAndForeignKeys)
{
    auto error = [](const PropertyMap& props) {
        auto p = parsePolicy("/b/Policy", props);
        return p ? std::string() : p.error();
    };
    auto r = vrRecord();
    r.erase("RecoveryFlow");
    EXPECT_EQ(error(r), "RecoveryFlow is missing");
    r = vrRecord();
    r.erase("I2CForce");
    EXPECT_EQ(error(r), "I2CForce is missing or not a boolean");
    r = hgxrRecord("NVSwitch", {"A"});
    r["HealthyEvent"] = S("yes");
    EXPECT_EQ(error(r), "HealthyEvent is missing or not a boolean");
    r = hgxb300Record({"A"});
    r["I2CForce"] = true;
    EXPECT_EQ(error(r), "I2CForce does not apply to this flow");
    r = bf4Record();
    r["RecoveryGroups"] = L{"A"};
    EXPECT_EQ(error(r), "RecoveryGroups does not apply to this flow");
    EXPECT_EQ(error(vrRecord({" "})), "RecoveryGroups is missing or empty");
    r = vrRecord();
    r["RecoveryFlow"] = S("Fast");
    EXPECT_EQ(error(r), "unsupported RecoveryFlow Fast");
    r["DeviceType"] = S("GPU");
    EXPECT_EQ(error(r), "unsupported DeviceType");
}

TEST(MstPolicyTest, FindPolicyMatchesDeviceType)
{
    FakePlatform p;
    constexpr auto a = "/xyz/openbmc_project/inventory/system/a/Policy";
    constexpr auto b = "/xyz/openbmc_project/inventory/system/b/Policy";
    p.objects[a][policyInterface] = vrRecord();
    p.objects[b][policyInterface] = hgxrRecord("NVSwitch", {"A"});
    auto found = findPolicy(p, "NVSwitch", 1, 0);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->board, "/xyz/openbmc_project/inventory/system/b");
    EXPECT_EQ(findPolicy(p, "Other", 3, 1000).error(),
              "no MSTRecoveryPolicy for Other");
    EXPECT_EQ(p.sleptMs, 2000U);
    p.objects[b][policyInterface] = vrRecord();
    EXPECT_EQ(findPolicy(p, "ConnectX", 1, 0).error(),
              "more than one MSTRecoveryPolicy for ConnectX");
}

TEST(MstPolicyTest, ValueTextAllTypes)
{
    EXPECT_EQ(toText(Value{uint8_t{1}}), "1");
    EXPECT_EQ(toText(Value{int16_t{-2}}), "-2");
    EXPECT_EQ(toText(Value{uint16_t{3}}), "3");
    EXPECT_EQ(toText(Value{int32_t{-4}}), "-4");
    EXPECT_EQ(toText(Value{uint32_t{5}}), "5");
    EXPECT_EQ(toText(Value{int64_t{-6}}), "-6");
    EXPECT_EQ(toText(Value{false}), "false");
    EXPECT_EQ(toText(Value{0.25}), "0.25");
    EXPECT_EQ(toText(Value{std::vector<std::string>{"a", "b"}}), "a b");
    EXPECT_EQ(toText(Value{std::vector<uint64_t>{1, 2}}), "1 2");
    EXPECT_EQ(toText(Value{std::vector<int64_t>{-1, 3}}), "-1 3");
    EXPECT_EQ(toText(Value{std::vector<double>{1.5, 2}}), "1.5 2");
    EXPECT_EQ(toText(Value{std::vector<std::string>{}}), "");
}

TEST(MstRecoverySignals, InstallSignalHandlers)
{
    struct sigaction oldTerm{};
    struct sigaction oldInt{};
    sigaction(SIGTERM, nullptr, &oldTerm);
    sigaction(SIGINT, nullptr, &oldInt);

    Recovery::installSignalHandlers();

    struct sigaction now{};
    sigaction(SIGTERM, nullptr, &now);
    EXPECT_NE(now.sa_handler, oldTerm.sa_handler);
    sigaction(SIGINT, nullptr, &now);
    EXPECT_NE(now.sa_handler, oldInt.sa_handler);

    sigaction(SIGTERM, &oldTerm, nullptr);
    sigaction(SIGINT, &oldInt, nullptr);
}

TEST(MstPolicyTest, ValueText)
{
    EXPECT_EQ(toText(Value{uint64_t{72}}), "72");
    EXPECT_EQ(toText(Value{72.0}), "72");
    EXPECT_EQ(toText(Value{true}), "true");
    EXPECT_EQ(toText(Value{std::string("x")}), "x");
}

} // namespace
} // namespace mst
