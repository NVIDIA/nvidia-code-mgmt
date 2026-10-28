// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

/**
 * Unit tests for the spi-programmer worker: console lines, D-Bus calls with
 * their arguments, poll sleeps and exit codes of the spi-programmer.sh flow
 * it replaces.
 */

#include "spi_programmer_worker.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;
using spi_programmer::Worker;

constexpr auto inProgress =
    "xyz.openbmc_project.Common.Progress.OperationStatus.InProgress";
constexpr auto completed =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Completed";
constexpr auto failedStatus =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Failed";
constexpr auto unreadable = "<error>";

class FakeBus : public spi_programmer::Bus
{
  public:
    std::optional<std::string> startUpdate(const std::string& device,
                                           int imageFd) override
    {
        events.push_back("StartUpdate " + device);
        fds.push_back(imageFd);
        struct stat st{};
        fdInodes.push_back(::fstat(imageFd, &st) == 0 ? st.st_ino : 0);
        fdModes.push_back(::fcntl(imageFd, F_GETFL) & O_ACCMODE);
        std::ifstream layout(layoutPath);
        layoutAtStart.emplace_back(std::istreambuf_iterator<char>(layout),
                                   std::istreambuf_iterator<char>());
        auto it = starts.find(device);
        if (it == starts.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<std::string> getStatus(const std::string& path) override
    {
        events.push_back("Get Status " + path);
        auto& queue = statuses[path];
        if (queue.empty())
        {
            return std::nullopt;
        }
        std::string value = queue.front();
        if (queue.size() > 1)
        {
            queue.pop_front();
        }
        if (value == unreadable)
        {
            return std::nullopt;
        }
        return value;
    }

    void getProgress(const std::string& path) override
    {
        events.push_back("Get Progress " + path);
    }

    void createLog(const std::string& message, const std::string& severity,
                   const std::map<std::string, std::string>& data) override
    {
        std::string event = "Create " + message + " " + severity;
        for (const auto& [key, value] : data)
        {
            event += " " + key + "=" + value;
        }
        events.push_back(event);
    }

    std::string layoutPath;
    std::map<std::string, std::string> starts;
    std::map<std::string, std::deque<std::string>> statuses;
    std::vector<std::string> events;
    std::vector<int> fds;
    std::vector<ino_t> fdInodes;
    std::vector<int> fdModes;
    std::vector<std::string> layoutAtStart;
};

class FakeConsole : public spi_programmer::Console
{
  public:
    void out(std::string_view line) override
    {
        lines.push_back("O " + std::string{line});
    }

    void err(std::string_view line) override
    {
        lines.push_back("E " + std::string{line});
    }

    std::vector<std::string> lines;
};

class SpiProgrammerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root =
            fs::temp_directory_path() /
            ("spi_programmer_test_" + std::to_string(::getpid()) + "_" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        fs::create_directories(root);
        layoutDir = root / "spi_dumps";
        image = (root / "image.bin").string();
        writeImage(image, 0x1234);
        bus.layoutPath = (layoutDir / "sbios_boot_flash.layout").string();
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    static void writeImage(const std::string& path, size_t size)
    {
        std::ofstream file(path, std::ios::binary);
        file << std::string(size, '\xa5');
    }

    int run(const std::vector<std::string>& args)
    {
        spi_programmer::Options options;
        options.programName = "/usr/bin/spi-programmer";
        options.layoutDir = layoutDir;
        options.sleep = [this](std::chrono::seconds duration) {
            bus.events.push_back("sleep " + std::to_string(duration.count()));
        };
        Worker worker(bus, console, options);
        return worker.run(args);
    }

    std::string layoutLine() const
    {
        return "Layout based on '" + image +
               "' (size: 4660 bytes) has been written to '" + bus.layoutPath +
               "'.";
    }

    std::string fd() const
    {
        return std::to_string(bus.fds.at(0));
    }

    std::vector<std::string> prologue(const std::string& devices) const
    {
        return {"O Devices to update: " + devices,
                "O Firmware version: 1.2.3",
                "O Image file: " + image,
                "O Generating SPI layout for file: " + image,
                "O " + layoutLine(),
                "O Opened file descriptor " + fd() +
                    " for image file: " + image};
    }

    static std::string event(const std::string& message,
                             const std::string& level, const std::string& args,
                             const std::string& id)
    {
        return "Create " + message +
               " xyz.openbmc_project.Logging.Entry.Level." + level +
               " REDFISH_MESSAGE_ARGS=" + args + " REDFISH_MESSAGE_ID=" + id +
               " namespace=FWUpdate";
    }

    static std::string transferring()
    {
        return event("Update.1.0.TransferringToComponent", "Informational",
                     "1.2.3,SBIOS", "Update.1.0.TransferringToComponent");
    }

    static std::string successful()
    {
        return event("Update.1.0.UpdateSuccessful", "Informational",
                     "SBIOS,1.2.3", "Update.1.0.UpdateSuccessful");
    }

    static std::string errorsDetected()
    {
        return event("org.open_power.Logging.Error.TestError1", "Warning", ":",
                     "ResourceEvent.1.0.ResourceErrorsDetected");
    }

    std::vector<std::string> exitCleanup() const
    {
        return {"O Cleaning up layout file: " + bus.layoutPath,
                "O Closing file descriptor: " + fd()};
    }

    static void append(std::vector<std::string>& to,
                       const std::vector<std::string>& from)
    {
        to.insert(to.end(), from.begin(), from.end());
    }

    fs::path root;
    fs::path layoutDir;
    std::string image;
    FakeBus bus;
    FakeConsole console;
};

TEST_F(SpiProgrammerTest, usageWhenFewerThanThreeArgs)
{
    for (const auto& args : std::vector<std::vector<std::string>>{
             {}, {"BootSPIFlash_0"}, {"BootSPIFlash_0", "1.2.3"}})
    {
        console.lines.clear();
        EXPECT_EQ(run(args), 1);
        EXPECT_EQ(console.lines,
                  (std::vector<std::string>{
                      "O Usage: /usr/bin/spi-programmer <device_name1> "
                      "[device_name2] ... <fw_version> <file_path>",
                      "O Example: /usr/bin/spi-programmer BootSPIFlash_0 "
                      "BootSPIFlash_1 1.0.0 "
                      "/tmp/images/pldm/SPIProgrammer/F4B94..."}));
    }
    EXPECT_TRUE(bus.events.empty());
}

TEST_F(SpiProgrammerTest, exitTrapRemovesStaleLayoutOnEarlyExit)
{
    fs::create_directories(layoutDir);
    std::ofstream(bus.layoutPath) << "stale\n";
    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3"}), 1);
    EXPECT_EQ(console.lines.back(),
              "O Cleaning up layout file: " + bus.layoutPath);
    EXPECT_FALSE(fs::exists(bus.layoutPath));
}

TEST_F(SpiProgrammerTest, rejectsMissingOrNonRegularImage)
{
    for (const auto& path :
         {(root / "missing.bin").string(), root.string(), std::string{}})
    {
        console.lines.clear();
        EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", path}), 1);
        EXPECT_EQ(
            console.lines,
            (std::vector<std::string>{
                "O Error: File does not exist or path is empty: " + path}));
    }
    EXPECT_TRUE(bus.events.empty());
    EXPECT_FALSE(fs::exists(layoutDir));
}

TEST_F(SpiProgrammerTest, rejectsEmptyVersion)
{
    EXPECT_EQ(run({"BootSPIFlash_0", "", image}), 1);
    EXPECT_EQ(console.lines, (std::vector<std::string>{
                                 "O Error: Firmware version cannot be empty"}));
    EXPECT_TRUE(bus.events.empty());
}

TEST_F(SpiProgrammerTest, rejectsEmptyDeviceName)
{
    EXPECT_EQ(run({"BootSPIFlash_0", "", "1.2.3", image}), 1);
    EXPECT_EQ(console.lines, (std::vector<std::string>{
                                 "O Error: Device name cannot be empty"}));
    EXPECT_TRUE(bus.events.empty());
}

TEST_F(SpiProgrammerTest, singleDeviceSuccessTranscript)
{
    const std::string path = "/xyz/openbmc_project/software/spi_status_0";
    bus.starts["BootSPIFlash_0"] = path;
    bus.statuses[path] = {inProgress, inProgress, completed};

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 0);

    auto expected = prologue("BootSPIFlash_0");
    append(expected,
           {"O Starting SPI programming for 1 device(s)",
            "E Starting SPI programming for device: BootSPIFlash_0 with fd: " +
                fd(),
            "E D-Bus operation path for BootSPIFlash_0: " + path,
            "O Successfully started programming for BootSPIFlash_0",
            "O Monitoring SPI operation status for 1 device(s)...",
            "O   BootSPIFlash_0: Completed successfully",
            "O All devices completed successfully!",
            "O Completed devices: BootSPIFlash_0",
            "O All devices updated successfully"});
    append(expected, exitCleanup());
    EXPECT_EQ(console.lines, expected);

    EXPECT_EQ(bus.events,
              (std::vector<std::string>{
                  "StartUpdate BootSPIFlash_0", transferring(),
                  "Get Status " + path, "Get Progress " + path, "sleep 5",
                  "Get Status " + path, "Get Progress " + path, "sleep 5",
                  "Get Status " + path, successful()}));
    EXPECT_FALSE(fs::exists(bus.layoutPath));
}

TEST_F(SpiProgrammerTest, passesImageReadOnlyAndLayoutBeforeStart)
{
    bus.starts["BootSPIFlash_0"] = "/p0";
    bus.statuses["/p0"] = {completed};

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 0);

    struct stat st{};
    ASSERT_EQ(::stat(image.c_str(), &st), 0);
    ASSERT_EQ(bus.fds.size(), 1U);
    EXPECT_EQ(bus.fdInodes[0], st.st_ino);
    EXPECT_EQ(bus.fdModes[0], O_RDONLY);
    EXPECT_EQ(bus.layoutAtStart[0],
              "00000000:00001233 slot0\n00001234:07ffffff unused\n");
    EXPECT_EQ(::fcntl(bus.fds[0], F_GETFD), -1);
}

TEST_F(SpiProgrammerTest, layoutFormat)
{
    EXPECT_EQ(spi_programmer::formatLayout(1),
              "00000000:00000000 slot0\n00000001:07ffffff unused\n");
    EXPECT_EQ(spi_programmer::formatLayout(0x4000000),
              "00000000:03ffffff slot0\n04000000:07ffffff unused\n");
}

TEST_F(SpiProgrammerTest, rejectsEmptyOrOversizedImage)
{
    bus.starts["BootSPIFlash_0"] = "/p0";
    for (auto size :
         {std::uintmax_t{0}, std::uintmax_t{spi_programmer::flashSize + 1}})
    {
        fs::resize_file(image, size);
        console.lines.clear();
        EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 1);
        EXPECT_NE(std::ranges::find(console.lines,
                                    "O Error: Image size " +
                                        std::to_string(size) +
                                        " is not within the 134217728 byte "
                                        "flash"),
                  console.lines.end());
    }
    EXPECT_TRUE(bus.events.empty());
}

TEST_F(SpiProgrammerTest, symlinkImageUsesTargetSize)
{
    const auto link = (root / "link.bin").string();
    fs::create_symlink(image, link);
    bus.starts["BootSPIFlash_0"] = "/p0";
    bus.statuses["/p0"] = {completed};

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", link}), 0);
    EXPECT_EQ(bus.layoutAtStart[0], spi_programmer::formatLayout(0x1234));
}

TEST_F(SpiProgrammerTest, layoutOpenFailureStopsBeforeStart)
{
    std::ofstream(layoutDir.string()) << "not a directory";
    bus.starts["BootSPIFlash_0"] = "/p0";
    bus.statuses["/p0"] = {completed};

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 1);
    EXPECT_TRUE(bus.events.empty());
    EXPECT_NE(std::ranges::find(console.lines,
                                "E " + bus.layoutPath + ": Not a directory"),
              console.lines.end());
    EXPECT_NE(std::ranges::find(console.lines,
                                "O Error: Failed to generate SPI layout"),
              console.lines.end());
    EXPECT_EQ(std::ranges::find(console.lines, "O " + layoutLine()),
              console.lines.end());
}

TEST_F(SpiProgrammerTest, layoutWriteFailureStopsBeforeStart)
{
    if (!fs::exists("/dev/full"))
    {
        GTEST_SKIP() << "/dev/full not available";
    }
    fs::create_directories(layoutDir);
    fs::create_symlink("/dev/full", bus.layoutPath);
    bus.starts["BootSPIFlash_0"] = "/p0";

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 1);
    EXPECT_TRUE(bus.events.empty());
    EXPECT_NE(std::ranges::find(console.lines, "E " + bus.layoutPath +
                                                   ": No space left on device"),
              console.lines.end());
    EXPECT_NE(std::ranges::find(console.lines,
                                "O Error: Failed to generate SPI layout"),
              console.lines.end());
}

TEST_F(SpiProgrammerTest, createsLayoutDirectory)
{
    layoutDir = root / "a" / "b";
    bus.layoutPath = (layoutDir / "sbios_boot_flash.layout").string();
    bus.starts["BootSPIFlash_0"] = "/p0";
    bus.statuses["/p0"] = {completed};

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 0);
    EXPECT_TRUE(fs::is_directory(layoutDir));
    EXPECT_FALSE(bus.layoutAtStart[0].empty());
}

TEST_F(SpiProgrammerTest, pollsDevicesInArgumentOrder)
{
    bus.starts = {{"SPI_A", "/pa"}, {"Flash", "/pf"}};
    bus.statuses["/pa"] = {inProgress, completed};
    bus.statuses["/pf"] = {completed};

    EXPECT_EQ(run({"SPI_A", "Flash", "1.2.3", image}), 0);
    EXPECT_EQ(bus.events,
              (std::vector<std::string>{
                  "StartUpdate SPI_A", "StartUpdate Flash", transferring(),
                  "Get Status /pa", "Get Progress /pa", "Get Status /pf",
                  "sleep 5", "Get Status /pa", successful()}));
    EXPECT_NE(
        std::ranges::find(console.lines, "O Completed devices: Flash SPI_A"),
        console.lines.end());
}

// The script forgets the devices that failed to start (sh:212)
TEST_F(SpiProgrammerTest, partialStartFailureSucceedsLikeTheScript)
{
    bus.starts["BootSPIFlash_1"] = "/p1";
    bus.statuses["/p1"] = {completed};

    EXPECT_EQ(run({"BootSPIFlash_0", "BootSPIFlash_1", "1.2.3", image}), 0);

    auto expected = prologue("BootSPIFlash_0 BootSPIFlash_1");
    append(expected,
           {"O Starting SPI programming for 2 device(s)",
            "E Starting SPI programming for device: BootSPIFlash_0 with fd: " +
                fd(),
            "E SPI programming command failed for device: BootSPIFlash_0",
            "O Failed to start programming for BootSPIFlash_0",
            "E Starting SPI programming for device: BootSPIFlash_1 with fd: " +
                fd(),
            "E D-Bus operation path for BootSPIFlash_1: /p1",
            "O Successfully started programming for BootSPIFlash_1",
            "O Warning: Failed to start programming for devices: "
            "BootSPIFlash_0",
            "O Monitoring SPI operation status for 1 device(s)...",
            "O   BootSPIFlash_1: Completed successfully",
            "O All devices completed successfully!",
            "O Completed devices: BootSPIFlash_1",
            "O All devices updated successfully"});
    append(expected, exitCleanup());
    EXPECT_EQ(console.lines, expected);
    EXPECT_EQ(bus.events.back(), successful());
}

TEST_F(SpiProgrammerTest, allStartsFailExitsWithoutEvents)
{
    EXPECT_EQ(run({"BootSPIFlash_0", "BootSPIFlash_1", "1.2.3", image}), 1);

    auto expected = prologue("BootSPIFlash_0 BootSPIFlash_1");
    append(expected,
           {"O Starting SPI programming for 2 device(s)",
            "E Starting SPI programming for device: BootSPIFlash_0 with fd: " +
                fd(),
            "E SPI programming command failed for device: BootSPIFlash_0",
            "O Failed to start programming for BootSPIFlash_0",
            "E Starting SPI programming for device: BootSPIFlash_1 with fd: " +
                fd(),
            "E SPI programming command failed for device: BootSPIFlash_1",
            "O Failed to start programming for BootSPIFlash_1",
            "O Warning: Failed to start programming for devices: "
            "BootSPIFlash_0 BootSPIFlash_1",
            "O Error: No devices started successfully"});
    append(expected, exitCleanup());
    EXPECT_EQ(console.lines, expected);
    EXPECT_EQ(bus.events,
              (std::vector<std::string>{"StartUpdate BootSPIFlash_0",
                                        "StartUpdate BootSPIFlash_1"}));
}

TEST_F(SpiProgrammerTest, firstFailureStopsPollingAfterThePass)
{
    bus.starts = {{"BootSPIFlash_0", "/p0"}, {"BootSPIFlash_1", "/p1"}};
    bus.statuses["/p0"] = {failedStatus};
    bus.statuses["/p1"] = {inProgress};

    EXPECT_EQ(run({"BootSPIFlash_0", "BootSPIFlash_1", "1.2.3", image}), 1);

    EXPECT_EQ(bus.events,
              (std::vector<std::string>{
                  "StartUpdate BootSPIFlash_0", "StartUpdate BootSPIFlash_1",
                  transferring(), "Get Status /p0", "Get Status /p1",
                  "Get Progress /p1", errorsDetected()}));
    std::vector<std::string> expected{
        "O   BootSPIFlash_0: Failed with status: '" +
            std::string{failedStatus} + "'",
        "O Some devices failed: BootSPIFlash_0",
        "O Completed devices: ", "O Failed devices: BootSPIFlash_0",
        "O Firmware update completed with errors"};
    append(expected, exitCleanup());
    ASSERT_GE(console.lines.size(), expected.size());
    EXPECT_EQ(std::vector<std::string>(console.lines.end() - expected.size(),
                                       console.lines.end()),
              expected);
}

TEST_F(SpiProgrammerTest, unreadableStatusIsAFailureWithEmptyValue)
{
    bus.starts["BootSPIFlash_0"] = "/p0";
    bus.statuses["/p0"] = {inProgress, unreadable};

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 1);
    EXPECT_NE(std::ranges::find(console.lines,
                                "O   BootSPIFlash_0: Failed with status: ''"),
              console.lines.end());
    EXPECT_EQ(bus.events, (std::vector<std::string>{
                              "StartUpdate BootSPIFlash_0", transferring(),
                              "Get Status /p0", "Get Progress /p0", "sleep 5",
                              "Get Status /p0", errorsDetected()}));
}

TEST_F(SpiProgrammerTest, reportsProgressEveryTwelvePollsWithoutTimeout)
{
    bus.starts["BootSPIFlash_0"] = "/p0";
    std::deque<std::string> statuses(25, inProgress);
    statuses.push_back(completed);
    bus.statuses["/p0"] = statuses;

    EXPECT_EQ(run({"BootSPIFlash_0", "1.2.3", image}), 0);

    EXPECT_EQ(std::ranges::count(bus.events, std::string{"sleep 5"}), 25);
    EXPECT_EQ(
        std::ranges::count(
            console.lines,
            std::string{"O Updating firmware in progress... (0/1 completed)"}),
        2);
}

TEST_F(SpiProgrammerTest, duplicateDeviceKeepsOneSlotWithLatestPath)
{
    bus.starts["BootSPIFlash_0"] = "/p0";
    bus.statuses["/p0"] = {completed};

    EXPECT_EQ(run({"BootSPIFlash_0", "BootSPIFlash_0", "1.2.3", image}), 0);
    EXPECT_NE(std::ranges::find(console.lines,
                                "O Starting SPI programming for 2 device(s)"),
              console.lines.end());
    EXPECT_NE(std::ranges::find(
                  console.lines,
                  "O Monitoring SPI operation status for 1 device(s)..."),
              console.lines.end());
    EXPECT_EQ(std::ranges::count(bus.events, std::string{"Get Status /p0"}), 1);
}

TEST_F(SpiProgrammerTest, signalCleanupRemovesLayoutAndClosesFd)
{
    bus.starts["BootSPIFlash_0"] = "/p0";
    bus.statuses["/p0"] = {inProgress};

    spi_programmer::Options options;
    options.layoutDir = layoutDir;
    int pipeFds[2];
    ASSERT_EQ(::pipe(pipeFds), 0);
    Worker* self = nullptr;
    options.sleep = [&self, &pipeFds](std::chrono::seconds) {
        self->cleanupFromSignal(pipeFds[1]);
        throw std::runtime_error("terminated");
    };
    Worker worker(bus, console, options);
    self = &worker;
    EXPECT_THROW(worker.run({"BootSPIFlash_0", "1.2.3", image}),
                 std::runtime_error);
    ::close(pipeFds[1]);

    std::string written(256, '\0');
    auto n = ::read(pipeFds[0], written.data(), written.size());
    ::close(pipeFds[0]);
    written.resize(n > 0 ? static_cast<size_t>(n) : 0);
    EXPECT_EQ(written, "Cleaning up layout file: " + bus.layoutPath +
                           "\nClosing file descriptor: " + fd() + "\n");
    EXPECT_FALSE(fs::exists(bus.layoutPath));
    EXPECT_EQ(::fcntl(bus.fds.at(0), F_GETFD), -1);
}

} // namespace
