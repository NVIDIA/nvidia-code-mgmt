// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include "spi_programmer_worker.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <ranges>
#include <system_error>
#include <thread>

namespace spi_programmer
{

namespace
{

constexpr auto statusInProgress =
    "xyz.openbmc_project.Common.Progress.OperationStatus.InProgress";
constexpr auto statusCompleted =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Completed";
constexpr auto levelInformational =
    "xyz.openbmc_project.Logging.Entry.Level.Informational";
constexpr auto levelWarning = "xyz.openbmc_project.Logging.Entry.Level.Warning";
constexpr int progressReportLoops = 12;

std::string join(const std::vector<std::string>& items)
{
    std::string joined;
    for (const auto& item : items)
    {
        if (&item != &items.front())
        {
            joined += ' ';
        }
        joined += item;
    }
    return joined;
}

bool isRegularFile(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::map<std::string, std::string> eventData(const std::string& args,
                                             const std::string& messageId)
{
    return {{"REDFISH_MESSAGE_ARGS", args},
            {"REDFISH_MESSAGE_ID", messageId},
            {"namespace", "FWUpdate"}};
}

bool writeAll(int fd, const char* data, size_t size) noexcept
{
    while (size > 0)
    {
        ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            if (n == 0)
            {
                errno = EIO;
            }
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

std::string formatLayout(std::int64_t imageSize)
{
    return std::format("{:08x}:{:08x} slot0\n{:08x}:{:08x} unused\n", 0,
                       imageSize - 1, imageSize, flashSize - 1);
}

Worker::Worker(Bus& bus, Console& console, Options options) :
    bus(bus), console(console), options(std::move(options)),
    layoutPath((this->options.layoutDir / layoutFileName).string()),
    cleanupMessage("Cleaning up layout file: " + layoutPath + "\n")
{
    if (!this->options.sleep)
    {
        this->options.sleep = [](std::chrono::seconds duration) {
            std::this_thread::sleep_for(duration);
        };
    }
}

Worker::~Worker()
{
    int fd = imageFd.exchange(-1);
    if (fd >= 0 && !imageFdClosed.exchange(true))
    {
        ::close(fd);
    }
}

int Worker::run(const std::vector<std::string>& args)
{
    int rc = execute(args);
    cleanup();
    return rc;
}

int Worker::execute(const std::vector<std::string>& args)
{
    const auto& prog = options.programName;
    if (args.size() < 3)
    {
        console.out("Usage: " + prog +
                    " <device_name1> [device_name2] ... <fw_version> "
                    "<file_path>");
        console.out("Example: " + prog +
                    " BootSPIFlash_0 BootSPIFlash_1 1.0.0 "
                    "/tmp/images/pldm/SPIProgrammer/F4B94...");
        return 1;
    }

    const std::string& filePath = args.back();
    const std::string& version = args[args.size() - 2];
    std::vector<std::string> devices(args.begin(), args.end() - 2);

    if (!isRegularFile(filePath))
    {
        console.out("Error: File does not exist or path is empty: " + filePath);
        return 1;
    }
    if (version.empty())
    {
        console.out("Error: Firmware version cannot be empty");
        return 1;
    }
    if (std::ranges::any_of(devices, &std::string::empty))
    {
        console.out("Error: Device name cannot be empty");
        return 1;
    }

    console.out("Devices to update: " + join(devices));
    console.out("Firmware version: " + version);
    console.out("Image file: " + filePath);

    std::error_code ec;
    std::filesystem::create_directories(options.layoutDir, ec);
    if (ec)
    {
        console.err("mkdir: cannot create directory '" +
                    options.layoutDir.string() + "': " + ec.message());
    }

    auto size = openImage(filePath);
    if (size < 0)
    {
        console.out("Error: Failed to open file descriptor for " + filePath);
        return 1;
    }
    console.out("Generating SPI layout for file: " + filePath);
    if (!generateLayout(filePath, size))
    {
        console.out("Error: Failed to generate SPI layout");
        return 1;
    }
    const int fd = imageFd.load();
    console.out("Opened file descriptor " + std::to_string(fd) +
                " for image file: " + filePath);

    console.out("Starting SPI programming for " +
                std::to_string(devices.size()) + " device(s)");
    // A repeated device keeps its slot with the latest path
    std::vector<std::pair<std::string, std::string>> objects;
    std::vector<std::string> failedToStart;
    for (const auto& device : devices)
    {
        console.err("Starting SPI programming for device: " + device +
                    " with fd: " + std::to_string(fd));
        auto path = bus.startUpdate(device, fd);
        if (!path || path->empty())
        {
            console.err("SPI programming command failed for device: " + device);
            failedToStart.push_back(device);
            console.out("Failed to start programming for " + device);
            continue;
        }
        console.err("D-Bus operation path for " + device + ": " + *path);
        auto it = std::ranges::find(
            objects, device, &std::pair<std::string, std::string>::first);
        if (it != objects.end())
        {
            it->second = *path;
        }
        else
        {
            objects.emplace_back(device, *path);
        }
        console.out("Successfully started programming for " + device);
    }

    if (!failedToStart.empty())
    {
        console.out("Warning: Failed to start programming for devices: " +
                    join(failedToStart));
        if (objects.empty())
        {
            console.out("Error: No devices started successfully");
            return 1;
        }
    }

    return poll(objects, version);
}

int Worker::poll(
    const std::vector<std::pair<std::string, std::string>>& objects,
    const std::string& version)
{
    console.out("Monitoring SPI operation status for " +
                std::to_string(objects.size()) + " device(s)...");

    bus.createLog(
        "Update.1.0.TransferringToComponent", levelInformational,
        eventData(version + ",SBIOS", "Update.1.0.TransferringToComponent"));

    std::vector<std::string> completed;
    std::vector<std::string> failed;
    unsigned int counter = 0;
    while (true)
    {
        for (const auto& [device, path] : objects)
        {
            if (std::ranges::contains(completed, device) ||
                std::ranges::contains(failed, device))
            {
                continue;
            }
            auto status = bus.getStatus(path).value_or("");
            if (status == statusInProgress)
            {
                bus.getProgress(path);
            }
            else if (status == statusCompleted)
            {
                console.out("  " + device + ": Completed successfully");
                completed.push_back(device);
            }
            else
            {
                console.out("  " + device + ": Failed with status: '" + status +
                            "'");
                failed.push_back(device);
            }
        }

        if (completed.size() == objects.size())
        {
            console.out("All devices completed successfully!");
            break;
        }
        if (!failed.empty())
        {
            console.out("Some devices failed: " + join(failed));
            break;
        }

        ++counter;
        if (counter % progressReportLoops == 0)
        {
            console.out("Updating firmware in progress... (" +
                        std::to_string(completed.size()) + "/" +
                        std::to_string(objects.size()) + " completed)");
        }
        options.sleep(options.pollInterval);
    }

    console.out("Completed devices: " + join(completed));
    // Like the script, a device that failed to start is not counted here
    if (!failed.empty())
    {
        console.out("Failed devices: " + join(failed));
        console.out("Firmware update completed with errors");
        // The script expands two unset variables here, so the args are ":"
        bus.createLog(
            "org.open_power.Logging.Error.TestError1", levelWarning,
            eventData(":", "ResourceEvent.1.0.ResourceErrorsDetected"));
        return 1;
    }
    bus.createLog("Update.1.0.UpdateSuccessful", levelInformational,
                  eventData("SBIOS," + version, "Update.1.0.UpdateSuccessful"));
    console.out("All devices updated successfully");
    return 0;
}

bool Worker::generateLayout(const std::string& filePath, std::int64_t size)
{
    if (size <= 0 || size > flashSize)
    {
        console.out("Error: Image size " + std::to_string(size) +
                    " is not within the " + std::to_string(flashSize) +
                    " byte flash");
        return false;
    }
    const std::string layout = formatLayout(size);
    int fd = ::open(layoutPath.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
    {
        console.err(layoutPath + ": " + std::strerror(errno));
        return false;
    }
    bool written = writeAll(fd, layout.data(), layout.size());
    int writeErr = errno;
    if (::close(fd) != 0 && written)
    {
        written = false;
        writeErr = errno;
    }
    if (!written)
    {
        console.err(layoutPath + ": " + std::strerror(writeErr));
        return false;
    }

    console.out("Layout based on '" + filePath +
                "' (size: " + std::to_string(size) +
                " bytes) has been written to '" + layoutPath + "'.");
    return true;
}

std::int64_t Worker::openImage(const std::string& filePath)
{
    // flashrom-wrapper only seeks and reads the descriptor
    int fd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0)
    {
        console.err(filePath + ": " + std::strerror(errno));
        if (fd >= 0)
        {
            ::close(fd);
        }
        return -1;
    }
    closeMessage = "Closing file descriptor: " + std::to_string(fd) + "\n";
    imageFd.store(fd);
    return st.st_size;
}

void Worker::cleanup()
{
    if (isRegularFile(layoutPath))
    {
        console.out("Cleaning up layout file: " + layoutPath);
        std::error_code ec;
        std::filesystem::remove(layoutPath, ec);
    }

    int fd = imageFd.load();
    if (fd >= 0)
    {
        console.out("Closing file descriptor: " + std::to_string(fd));
        if (!imageFdClosed.exchange(true))
        {
            ::close(fd);
        }
    }
}

void Worker::cleanupFromSignal(int outFd) noexcept
{
    struct stat st{};
    if (::stat(layoutPath.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    {
        writeAll(outFd, cleanupMessage.data(), cleanupMessage.size());
        ::unlink(layoutPath.c_str());
    }
    int fd = imageFd.load();
    if (fd >= 0)
    {
        writeAll(outFd, closeMessage.data(), closeMessage.size());
        if (!imageFdClosed.exchange(true))
        {
            ::close(fd);
        }
    }
}

} // namespace spi_programmer
