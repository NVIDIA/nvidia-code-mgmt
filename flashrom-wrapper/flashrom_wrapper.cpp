#include "config.h"

#include "dbusutils.hpp"
#include "spi.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/asio/readable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <sdbusplus/exception.hpp>

#include <map>
#include <mutex>
#include <regex>
#include <sstream>

using namespace phosphor::logging;
using namespace nvidia::software::updater;

namespace MatchRules = sdbusplus::bus::match::rules;

// Mapping table for chip types and their expected operation times
const std::vector<ChipTiming> Spi::chipTimingMap = {
    // ChipType, Erase Time, Read Time
    // The current expected erase time is measured with --noverify flag
    // It needs to be updated if the flashrom memory issue is fixed
    {"W25Q64BV/W25Q64CV/W25Q64FV", 30, 30, 60},
    {"MX66U1G45G", 150, 300, 2000}};

// vector of all spi devices
std::vector<std::unique_ptr<Spi>> spiDevices;
std::unique_ptr<sdbusplus::bus::match_t> inventoryObjectMatch;

// Map to track USB port usage (usbPort -> bool)
std::map<std::string, bool> usbPortInUse;
std::mutex usbPortMutex;

std::atomic<uint32_t> objIndex{0};

/**
 * @brief function to get the singleton ASIO connection
 *
 * @return boost::asio::io_context& Reference to the ASIO I/O context
 */
static auto& getAsioConnection()
{
    static boost::asio::io_context io;
    static auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    return conn;
}

/**
 * @brief function to get the singleton D-Bus connection
 *
 * @return sdbusplus::bus::bus& Reference to the D-Bus connection
 */
static auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

/**
 * @brief function to check if the host is powered off
 *
 * @return bool True if the host is powered off, false otherwise
 */
bool isHostPowerOff()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto hostState = dbusUtil.getProperty<std::string>(
        "xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0",
        "xyz.openbmc_project.State.Host", "CurrentHostState");
    return hostState == "xyz.openbmc_project.State.Host.HostState.Off";
}

/**
 * @brief function to check if a USB port is currently in use
 *
 * @param usbPort The USB port string to check
 * @return bool True if the USB port is in use, false otherwise
 */
bool isUsbInUse(const std::string& usbPort)
{
    std::lock_guard<std::mutex> lock(usbPortMutex);
    auto it = usbPortInUse.find(usbPort);
    return (it != usbPortInUse.end()) && it->second;
}

/**
 * @brief function to mark a USB port as in use
 *
 * @param usbPort The USB port string to mark as in use
 * @return bool True if successfully marked as in use, false if already in use
 */
bool markUsbInUse(const std::string& usbPort)
{
    std::lock_guard<std::mutex> lock(usbPortMutex);
    if (usbPortInUse[usbPort])
    {
        return false; // Already in use
    }
    usbPortInUse[usbPort] = true;
    return true;
}

/**
 * @brief function to release a USB port
 *
 * @param usbPort The USB port string to release
 * @return void
 */
void releaseUsb(const std::string& usbPort)
{
    std::lock_guard<std::mutex> lock(usbPortMutex);
    usbPortInUse[usbPort] = false;
}

void Spi::executeFlashrom(std::vector<std::string> args, Operation ops)
{
    try
    {
        auto proc = std::make_shared<boost::process::v2::process>(
            getAsioConnection()->get_io_context(), "/usr/sbin/flashrom", args,
            boost::process::v2::process_stdio{});

        // Store process reference for timeout handling
        currentProcess = proc;

        proc->async_wait([this, proc, ops](const boost::system::error_code& ec,
                                           int exit_code) {
            if (ec)
            {
                if (ec == boost::system::errc::operation_canceled)
                {
                    lg2::info("flashrom process was canceled");
                }
                else
                {
                    lg2::error("Process wait failed: {ERR}", "ERR",
                               ec.message());
                }
                finishSpiOperation(SpiProgress::OperationStatus::Failed);
                return;
            }

            if (exit_code == 0)
            {
                if (ops == Operation::Read)
                {
                    lg2::info("Current dump file name: {FILE}", "FILE",
                              dumpFile);
                    int fd = open(dumpFile.c_str(), O_RDONLY);
                    if (fd < 0)
                    {
                        lg2::error("Failed to open flashrom output file: {ERR}",
                                   "ERR", strerror(errno));
                        finishSpiOperation(
                            SpiProgress::OperationStatus::Failed);
                        return;
                    }
                    dumpFiles.push_back(std::make_pair(dumpFile, fd));

                    if (auto currentProgressObj = getCurrentProgressObj())
                    {
                        sdbusplus::message::unix_fd unixFd(fd);
                        currentProgressObj->spiReadFd(unixFd);
                    }
                }
                finishSpiOperation(SpiProgress::OperationStatus::Completed);
                lg2::info("flashrom completed successfully");
            }
            else
            {
                finishSpiOperation(SpiProgress::OperationStatus::Failed);
                lg2::error("flashrom failed with exit code: {CODE}", "CODE",
                           exit_code);
            }
        });
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to start flashrom process: {ERR}", "ERR", e.what());
    }
}

void Spi::onHostPowerStateChanged(sdbusplus::message_t& msg)
{
    // Check if operation is still in progress
    if (!isUsbInUse(usbPort))
    {
        return;
    }

    try
    {
        std::string interface;
        std::map<std::string, std::variant<std::string>> changedProperties;
        msg.read(interface, changedProperties);

        // Check if CurrentHostState property changed
        auto it = changedProperties.find("CurrentHostState");
        if (it == changedProperties.end())
        {
            return;
        }

        auto hostState = std::get<std::string>(it->second);
        lg2::info("[SPI: {NAME}] Host state changed to: {STATE}", "NAME", name,
                  "STATE", hostState);

        // Check if host powered state not Off
        if (hostState != "xyz.openbmc_project.State.Host.HostState.Off")
        {
            lg2::error(
                "[SPI: {NAME}] SPI operation failed due to the host power not remaining off.",
                "NAME", name);

            // Terminate the operation process if running
            if (currentProcess)
            {
                lg2::info(
                    "[SPI: {NAME}] Terminating flashrom process due to host power not remaining off",
                    "NAME", name);
                currentProcess->terminate();
            }

            finishSpiOperation(SpiProgress::OperationStatus::Failed);
        }
    }
    catch (const std::exception& e)
    {
        lg2::warning(
            "[SPI: {NAME}] Failed to parse host power state change: {ERR}",
            "NAME", name, "ERR", e.what());
    }
}

void Spi::updateProgress()
{
    // Check if USB port is still in use (operation in progress) and progress
    // not complete
    if (!isUsbInUse(usbPort) || currentProgress >= 100)
    {
        return;
    }

    // Update progress by 10%
    currentProgress += 10;

    if (auto currentProgressObj = getCurrentProgressObj())
    {
        currentProgressObj->progress(currentProgress);
        lg2::info("[SPI: {NAME}] Progress updated to: {PROGRESS}%", "NAME",
                  name, "PROGRESS", currentProgress);
    }

    // If not at 100%, schedule next update
    if (currentProgress < 100)
    {
        // Calculate interval: 1/10 of expected time
        int intervalMs = (expectedOpTimeSec * 1000) / 10;

        progressTimer->expires_after(std::chrono::milliseconds(intervalMs));
        progressTimer->async_wait([this](const boost::system::error_code& ec) {
            if (!ec)
            {
                updateProgress();
            }
        });
    }
}

int Spi::getExpectedTime(Operation ops) const
{
    // Find matching chip timing in the mapping table
    for (const auto& chipTiming : chipTimingMap)
    {
        if (chipTiming.chipName == chip)
        {
            switch (ops)
            {
                case Operation::Erase:
                    return chipTiming.eraseTimeSeconds;
                case Operation::Read:
                    return chipTiming.readTimeSeconds;
                case Operation::Write:
                    return chipTiming.writeTimeSeconds;
            }
        }
    }
    // Fallback to default timing if chip not found
    switch (ops)
    {
        case Operation::Erase:
            return 450;
        case Operation::Read:
            return 300;
        case Operation::Write:
            return 2000;
        default:
            return 2000;
    }
}

sdbusplus::message::object_path Spi::startUpdate(
    sdbusplus::message::unix_fd image,
    ApplyTimeIntf::RequestedApplyTimes applyTime [[maybe_unused]],
    bool forceUpdate [[maybe_unused]],
    std::vector<sdbusplus::message::object_path> targets [[maybe_unused]])
{
    // Extract file descriptor from unix_fd
    int imageFd = image;

    // Create temporary file path as flashrom only supports file path
    std::string tempFilePath =
        "/tmp/spi_image_" + name + "_" + std::to_string(objIndex) + ".bin";

    // Open temporary file for writing
    int outputFd =
        open(tempFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outputFd < 0)
    {
        lg2::error("[SPI: {NAME}] Failed to create temp file {FILE}: {ERR}",
                   "NAME", name, "FILE", tempFilePath, "ERR", strerror(errno));
        throw sdbusplus::error::xyz::openbmc_project::common::InternalFailure{};
    }

    // Reset file pointer to the beginning
    lseek(imageFd, 0, SEEK_SET);

    // Copy data in chunks using buffer
    char buffer[4096];
    ssize_t bytesRead;
    ssize_t totalBytes = 0;

    while ((bytesRead = read(imageFd, buffer, sizeof(buffer))) > 0)
    {
        ssize_t bytesWritten = write(outputFd, buffer, bytesRead);
        if (bytesWritten != bytesRead)
        {
            lg2::error("[SPI: {NAME}] Failed to write to temp file: {ERR}",
                       "NAME", name, "ERR", strerror(errno));
            close(outputFd);
            unlink(tempFilePath.c_str()); // Delete incomplete file
            throw sdbusplus::error::xyz::openbmc_project::common::
                InternalFailure{};
        }
        totalBytes += bytesWritten;
    }

    close(outputFd);

    if (bytesRead < 0)
    {
        lg2::error("[SPI: {NAME}] Failed to read from image fd: {ERR}", "NAME",
                   name, "ERR", strerror(errno));
        unlink(tempFilePath.c_str()); // Delete incomplete file
        throw sdbusplus::error::xyz::openbmc_project::common::InternalFailure{};
    }

    lg2::info(
        "[SPI: {NAME}] Successfully wrote {SIZE} bytes to temp file {FILE}",
        "NAME", name, "SIZE", totalBytes, "FILE", tempFilePath);

    // Check if operation can be started
    if (!startSpiOperation(Operation::Write))
    {
        lg2::error("[SPI: {NAME}] Cannot start write operation.", "NAME", name);
        unlink(tempFilePath.c_str()); // Delete temporary file
        throw sdbusplus::error::xyz::openbmc_project::common::Unavailable{};
    }

    // Set file path and execute flashrom
    this->filePath = tempFilePath;
    std::vector<std::string> args = prepareArgs(Operation::Write);
    executeFlashrom(args, Operation::Write);

    // Return the path to the progress object
    return sdbusplus::message::object_path(std::string(spiStatusPath) + "_" +
                                           std::to_string(objIndex - 1));
}

sdbusplus::message::object_path Spi::eraseSpi()
{
    if (!startSpiOperation(Operation::Erase))
    {
        lg2::error("[SPI: {NAME}] Cannot start erase operation.", "NAME", name);
        throw sdbusplus::error::xyz::openbmc_project::common::Unavailable{};
    }

    std::vector<std::string> args = prepareArgs(Operation::Erase);
    executeFlashrom(args, Operation::Erase);
    // Return the path to the progress object
    return sdbusplus::message::object_path(std::string(spiStatusPath) + "_" +
                                           std::to_string(objIndex - 1));
}

sdbusplus::message::object_path Spi::readSpi()
{
    if (!startSpiOperation(Operation::Read))
    {
        lg2::error("[SPI: {NAME}] Cannot start read operation.", "NAME", name);
        throw sdbusplus::error::xyz::openbmc_project::common::Unavailable{};
    }

    std::vector<std::string> args = prepareArgs(Operation::Read);
    executeFlashrom(args, Operation::Read);
    // Return the path to the progress object
    return sdbusplus::message::object_path(std::string(spiStatusPath) + "_" +
                                           std::to_string(objIndex - 1));
}

std::vector<std::string> Spi::prepareArgs(Operation ops)
{
    std::vector<std::string> args;
    lg2::info("[SPI: {NAME}] USB Bus: {BUS}, Device: {DEV}", "NAME", name,
              "BUS", usbBusNum, "DEV", usbDevNum);

    // use chip select, bus, devnum to indicate the spi chip
    std::string programmerWithDevInfo = programmer + ":cs=" + chipSelect +
                                        ",bus=" + usbBusNum +
                                        ",devnum=" + usbDevNum;
    switch (ops)
    {
        case Operation::Erase:
        {
            // Add --noverify to skip verification of the erase operation due to
            // the flashrom memory issue
            args = {"-p",      programmerWithDevInfo, "-c", chip,
                    "--erase", "--noverify"};
            break;
        }
        case Operation::Read:
        {
            dumpFile = std::string(spiDumpDir) + "/spi_dump_" +
                       std::to_string(objIndex - 1) + ".bin";
            args = {"-p",    programmerWithDevInfo, "-c", chip, "--read",
                    dumpFile};
            break;
        }
        case Operation::Write:
        {
            args = {
                "-p",
                programmerWithDevInfo,
                "-c",
                chip,
                "--write",
                "-l",
                "/var/emmc/user-logs/logging/spi_dumps/sbios_boot_flash.layout",
                "-i",
                "slot0:" + filePath};
            lg2::info("[SPI: {NAME}] Writing to SPI flash memory: {FILE}",
                      "NAME", name, "FILE", filePath);
#ifdef FLASHROM_WRITE_USE_REF_FILE
            std::cout << "[SPI: " << name
                      << "] Using reference file: " << FLASHROM_WRITE_REF_FILE
                      << std::endl;
            args.push_back("--flash-contents");
            args.push_back(FLASHROM_WRITE_REF_FILE);
#endif
#ifdef FLASHROM_WRITE_SKIP_VERIFY
            lg2::info("[SPI: {NAME}] Skipping write verify", "NAME", name);
            args.push_back("--noverify");
#endif
            break;
        }
        default:
            throw std::invalid_argument("Invalid operation");
    }
    return args;
}

bool Spi::getUsbBusNum()
{
    // Extract bus number (first number before '-')
    size_t dashPos = usbPort.find('-');
    if (dashPos == std::string::npos)
    {
        lg2::error("[SPI: {NAME}] No '-' found in USB port string: {PORT}",
                   "NAME", name, "PORT", usbPort);
        return false;
    }

    try
    {
        usbBusNum = usbPort.substr(0, dashPos);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "[SPI: {NAME}] Failed to convert bus number to integer: {ERR}",
            "NAME", name, "ERR", e.what());
        return false;
    }
    return true;
}

bool Spi::getUsbDevNum()
{
    // Read device number from sysfs
    std::string usbDevicePath = "/sys/bus/usb/devices/" + usbPort;
    std::string devnumPath = usbDevicePath + "/devnum";
    std::ifstream file(devnumPath);

    if (!file.is_open())
    {
        lg2::error("[SPI: {NAME}] Failed to open file: {PATH}", "NAME", name,
                   "PATH", devnumPath);
        return false;
    }

    std::string line;
    if (!std::getline(file, line))
    {
        lg2::error("[SPI: {NAME}] Failed to read line from file: {PATH}",
                   "NAME", name, "PATH", devnumPath);
        return false;
    }

    try
    {
        usbDevNum = line;
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "[SPI: {NAME}] Failed to convert device number to integer: {ERR}",
            "NAME", name, "ERR", e.what());
        return false;
    }
    return true;
}

bool Spi::startSpiOperation(Operation ops)
{
    // Check if USB port is already in use by another SPI device
    if (isUsbInUse(usbPort))
    {
        lg2::error(
            "[SPI: {NAME}] USB port {PORT} is already in use by another SPI device.",
            "NAME", name, "PORT", usbPort);
        return false;
    }

    if (!isHostPowerOff())
    {
        lg2::error("[SPI: {NAME}] Host is not powered off.", "NAME", name);
        return false;
    }
    lg2::info("[SPI: {NAME}] Host is powered off.", "NAME", name);
    if (!getUsbDevNum())
    {
        lg2::error("[SPI: {NAME}] Failed to get Device number", "NAME", name);
        return false;
    }

    // Check and create /var/emmc/spi_dump directory if it doesn't exist
    if (ops == Operation::Read && !checkDumpFiles())
    {
        lg2::error("[SPI: {NAME}] Failed to create directory {DIR}", "NAME",
                   name, "DIR", spiDumpDir);
        return false;
    }

    setSpiMux();

    // Mark USB port as in use
    if (!markUsbInUse(usbPort))
    {
        lg2::error("[SPI: {NAME}] Failed to mark USB port {PORT} as in use.",
                   "NAME", name, "PORT", usbPort);
        resetSpiMux();
        return false;
    }

    // Dynamically detect chip model
    std::string detectedChip = detectChipModel();
    if (detectedChip.empty())
    {
        lg2::error("[SPI: {NAME}] Failed to detect chip model", "NAME", name);
        releaseUsb(usbPort);
        resetSpiMux();
        return false;
    }

    // Update chip member variable with detected model
    chip = detectedChip;
    lg2::info("[SPI: {NAME}] Using detected chip model: {CHIP}", "NAME", name,
              "CHIP", chip);

    // Remove oldest entry if we've reached max size
    if (progressHistory.size() >= maxProgressHistory)
    {
        progressHistory.pop_front();
    }

    std::string progressInterface =
        std::string(spiStatusPath) + "_" + std::to_string(objIndex++);
    auto newProgress =
        std::make_unique<SpiProgress>(getBus(), progressInterface.c_str());
    newProgress->startTime(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    newProgress->status(SpiProgress::OperationStatus::InProgress);
    newProgress->progress(0);
    newProgress->spiReadFd(sdbusplus::message::unix_fd(-1));

    progressHistory.push_back(std::move(newProgress));

    // Set up progress timer
    expectedOpTimeSec = getExpectedTime(ops);
    currentProgress = 0;
    lg2::info("[SPI: {NAME}] Expected time: {TIME} seconds", "NAME", name,
              "TIME", expectedOpTimeSec);
    // Create timer to update progress every 1/10 of expected time as we don't
    // know the actual progress
    progressTimer = std::make_shared<boost::asio::steady_timer>(
        getAsioConnection()->get_io_context());

    // Create timeout timer, we use 1.5x expected time as timeout
    timeoutTimer = std::make_shared<boost::asio::steady_timer>(
        getAsioConnection()->get_io_context());
    int timeoutSeconds = static_cast<int>(expectedOpTimeSec * 1.5);
    lg2::info("[SPI: {NAME}] Timeout set to: {TIMEOUT} seconds", "NAME", name,
              "TIMEOUT", timeoutSeconds);

    timeoutTimer->expires_after(std::chrono::seconds(timeoutSeconds));
    timeoutTimer->async_wait([this](const boost::system::error_code& ec) {
        if (!ec)
        {
            lg2::error(
                "[SPI: {NAME}] Operation timed out after 1.5x expected time",
                "NAME", name);
            if (currentProcess)
            {
                lg2::info(
                    "[SPI: {NAME}] Terminating flashrom process due to timeout",
                    "NAME", name);
                currentProcess->terminate();
            }
            finishSpiOperation(SpiProgress::OperationStatus::Failed);
        }
    });

    // Create a dbus match to monitor host power state changes
    hostPowerStateMatch = std::make_unique<sdbusplus::bus::match_t>(
        getBus(),
        MatchRules::propertiesChanged("/xyz/openbmc_project/state/host0",
                                      "xyz.openbmc_project.State.Host"),
        [this](sdbusplus::message_t& msg) { onHostPowerStateChanged(msg); });
    lg2::info("[SPI: {NAME}] Started monitoring host power state", "NAME",
              name);

    updateProgress();

    return true;
}

void Spi::finishSpiOperation(SpiProgress::OperationStatus opStatus)
{
    if (auto currentProgressObj = getCurrentProgressObj())
    {
        currentProgressObj->status(opStatus);
        currentProgressObj->progress(100);
        currentProgressObj->completedTime(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // Clean up temporary file for Write operation
    if (!filePath.empty())
    {
        if (unlink(filePath.c_str()) == 0)
        {
            lg2::info("[SPI: {NAME}] Deleted temporary file {FILE}", "NAME",
                      name, "FILE", filePath);
        }
        else
        {
            lg2::warning(
                "[SPI: {NAME}] Failed to delete temporary file {FILE}: {ERR}",
                "NAME", name, "FILE", filePath, "ERR", strerror(errno));
        }
        filePath.clear();
    }

    // Release USB port
    releaseUsb(usbPort);
    lg2::info("[SPI: {NAME}] Released USB port {PORT}", "NAME", name, "PORT",
              usbPort);

    resetSpiMux();

    // Cancel the progress timer when operation finishes
    if (progressTimer)
    {
        progressTimer->cancel();
        progressTimer.reset();
    }

    // Cancel the timeout timer when operation finishes
    if (timeoutTimer)
    {
        timeoutTimer->cancel();
        timeoutTimer.reset();
    }

    // Remove the host power state dbus match when operation finish
    if (hostPowerStateMatch)
    {
        hostPowerStateMatch.reset();
    }

    currentProcess.reset();
}

void Spi::setSpiMux()
{
    lg2::info("[SPI: {NAME}] Setting SPI MUX...", "NAME", name);
    for (auto& [gpioLine, value] : activeGpios)
    {
        gpiod::line gpio;
        gpio = gpiod::find_line(gpioLine);
        if (!gpio)
        {
            lg2::error("Failed to find the GPIO line: {GPIO}", "GPIO",
                       gpioLine);
            continue;
        }
        gpio.request(
            {"flashrom-wrapper", gpiod::line_request::DIRECTION_OUTPUT, {}});
        gpio.set_value(value);
        gpio.release();
    }
}

void Spi::resetSpiMux()
{
    lg2::info("[SPI: {NAME}] Resetting SPI MUX...", "NAME", name);
    for (auto& [gpioLine, value] : deactiveGpios)
    {
        gpiod::line gpio;
        gpio = gpiod::find_line(gpioLine);
        if (!gpio)
        {
            std::cout << "Failed to find the GPIO line: " << gpioLine
                      << std::endl;
            continue;
        }
        gpio.request(
            {"flashrom-wrapper", gpiod::line_request::DIRECTION_OUTPUT, {}});
        gpio.set_value(value);
        gpio.release();
    }
}

bool Spi::checkDumpFiles()
{
    // Check if we need to clean up old dump files to avoid space issues
    if (dumpFiles.size() >= maxSpiDumps)
    {
        lg2::info(
            "[SPI: {NAME}] Cleaning up old dump file, current count: {COUNT}, max: {MAX}",
            "NAME", name, "COUNT", dumpFiles.size(), "MAX", maxSpiDumps);

        auto [oldFile, oldFd] = dumpFiles.front();

        // Close the file descriptor first to ensure disk space is released
        if (oldFd >= 0)
        {
            if (close(oldFd) == 0)
            {
                lg2::info("[SPI: {NAME}] Closed file descriptor for: {FILE}",
                          "NAME", name, "FILE", oldFile);
            }
            else
            {
                lg2::warning(
                    "[SPI: {NAME}] Failed to close file descriptor for: {FILE}, error: {ERR}",
                    "NAME", name, "FILE", oldFile, "ERR", strerror(errno));
            }
        }

        if (std::remove(oldFile.c_str()) == 0)
        {
            lg2::info("[SPI: {NAME}] Removed old dump file: {FILE}", "NAME",
                      name, "FILE", oldFile);
        }
        else
        {
            lg2::warning(
                "[SPI: {NAME}] Failed to remove old dump file: {FILE}, error: {ERR}",
                "NAME", name, "FILE", oldFile, "ERR", strerror(errno));
        }
        dumpFiles.pop_front();
    }

    return true;
}

std::string Spi::detectChipModel()
{
    lg2::info("[SPI: {NAME}] Detecting chip model...", "NAME", name);

    // Build flashrom command without -c parameter to auto-detect chip
    std::string programmerWithDevInfo = programmer + ":cs=" + chipSelect +
                                        ",bus=" + usbBusNum +
                                        ",devnum=" + usbDevNum;

    std::vector<std::string> args = {"-p", programmerWithDevInfo};

    lg2::info("[SPI: {NAME}] Running detection with programmer: {PROG}", "NAME",
              name, "PROG", programmerWithDevInfo);

    try
    {
        // Create pipe for capturing stdout
        boost::asio::readable_pipe stdoutPipe(
            getAsioConnection()->get_io_context());

        // Execute flashrom process with output capture
        boost::process::v2::process proc(
            getAsioConnection()->get_io_context(), "/usr/sbin/flashrom", args,
            boost::process::v2::process_stdio{
                .in = nullptr, .out = stdoutPipe, .err = nullptr});

        // Create timeout timer
        constexpr int detectionTimeoutSec = 10;
        boost::asio::steady_timer timeoutTimer(
            getAsioConnection()->get_io_context());
        timeoutTimer.expires_after(std::chrono::seconds(detectionTimeoutSec));

        bool timedOut = false;
        timeoutTimer.async_wait([&proc, &timedOut, detectionTimeoutSec,
                                 this](const boost::system::error_code& ec) {
            if (!ec)
            {
                lg2::error(
                    "[SPI: {NAME}] Chip detection timed out after {TIMEOUT} seconds",
                    "NAME", name, "TIMEOUT", detectionTimeoutSec);
                timedOut = true;
                proc.terminate();
            }
        });

        // Read output synchronously
        std::string output;
        std::array<char, 1024> buffer;
        boost::system::error_code ec;

        while (true)
        {
            std::size_t n =
                stdoutPipe.read_some(boost::asio::buffer(buffer), ec);
            if (ec)
            {
                if (ec == boost::asio::error::eof ||
                    ec == boost::asio::error::broken_pipe)
                {
                    break; // End of data
                }
                lg2::error("[SPI: {NAME}] Error reading stdout: {ERR}", "NAME",
                           name, "ERR", ec.message());
                break;
            }
            if (n > 0)
            {
                output.append(buffer.data(), n);
            }
        }

        // Wait for process to complete
        proc.wait();

        // Cancel timeout timer if process completed successfully
        timeoutTimer.cancel();

        // Check if we timed out
        if (timedOut)
        {
            lg2::error(
                "[SPI: {NAME}] Chip detection was terminated due to timeout",
                "NAME", name);
            return "";
        }

        lg2::info("[SPI: {NAME}] Flashrom detection output: {OUTPUT}", "NAME",
                  name, "OUTPUT", output);

        // Parse output to find chip model
        // Looking for pattern: Found ... flash chip "CHIP_MODEL"
        std::regex chipPattern(R"(Found .* flash chip \"([^\"]+)\")");
        std::smatch matches;

        if (std::regex_search(output, matches, chipPattern))
        {
            std::string detectedChip = matches[1].str();
            lg2::info("[SPI: {NAME}] Detected chip model: {CHIP}", "NAME", name,
                      "CHIP", detectedChip);
            return detectedChip;
        }

        lg2::error(
            "[SPI: {NAME}] Failed to parse chip model from flashrom output",
            "NAME", name);
        return "";
    }
    catch (const std::exception& e)
    {
        lg2::error("[SPI: {NAME}] Exception during chip detection: {ERR}",
                   "NAME", name, "ERR", e.what());
        return "";
    }
}

/**
 * @brief function to initialize dump folder globally
 *
 * @return bool True if initialization successful, false otherwise
 */
bool initDumpFolder()
{
    struct stat st;

    if (stat(spiDumpDir, &st) != 0)
    {
        // Directory doesn't exist, create it
        if (mkdir(spiDumpDir, 0755) != 0)
        {
            lg2::error("Failed to create dump directory {DIR}: {ERR}", "DIR",
                       spiDumpDir, "ERR", strerror(errno));
            return false;
        }
        lg2::info("Created dump directory: {DIR}", "DIR", spiDumpDir);
    }
    else if (!S_ISDIR(st.st_mode))
    {
        lg2::error("{DIR} exists but is not a directory", "DIR", spiDumpDir);
        return false;
    }
    else
    {
        lg2::info("Dump directory already exists: {DIR}", "DIR", spiDumpDir);

        // Clean all files in the directory
        DIR* dir = opendir(spiDumpDir);
        if (dir == nullptr)
        {
            lg2::error("Failed to open directory {DIR}: {ERR}", "DIR",
                       spiDumpDir, "ERR", strerror(errno));
            return false;
        }

        struct dirent* entry;

        while ((entry = readdir(dir)) != nullptr)
        {
            // Skip . and .. entries
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }

            std::string filePath =
                std::string(spiDumpDir) + "/" + entry->d_name;
            if (std::remove(filePath.c_str()) == 0)
            {
                lg2::info("Removed existing file: {FILE}", "FILE", filePath);
            }
            else
            {
                lg2::warning("Failed to remove file: {FILE}, error: {ERR}",
                             "FILE", filePath, "ERR", strerror(errno));
            }
        }

        closedir(dir);
    }
    return true;
}

/**
 * @brief function to get string property from D-Bus interface map
 *
 * @param interfaces The interface map containing properties
 * @param interface The interface name to look up
 * @param property The property name to retrieve
 * @return std::string The property value, empty string if not found
 */
std::string getString(const InterfaceMap& interfaces,
                      const Interface& interface, const Property& property)
{
    try
    {
        return std::get<std::string>(interfaces.at(interface).at(property));
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to get property {NAME}. {ERR}", "NAME", property,
                   "ERR", e.what());
        return {};
    }
}

/**
 * @brief function to get uint64_t property from D-Bus interface map
 *
 * @param interfaces The interface map containing properties
 * @param interface The interface name to look up
 * @param property The property name to retrieve
 * @return uint64_t The property value, 0 if not found
 */
std::uint64_t getUint64(const InterfaceMap& interfaces,
                        const Interface& interface, const Property& property)
{
    try
    {
        return std::get<uint64_t>(interfaces.at(interface).at(property));
    }
    catch (std::exception& e)
    {
        lg2::error("Failed to get property {NAME}. {ERR}", "NAME", property,
                   "ERR", e.what());
        return {};
    }
}

/**
 * @brief function to populate SPI objects from Entity Manager configuration
 *
 * @return void
 */
void populateSpiObjects()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    const auto managedObjects =
        dbusUtil.getManagedObjects(entityManagerService, inventoryRootPath);

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(spiObjectInterfaces))
        {
            const auto usbPort =
                getString(interfaces, spiObjectInterfaces, "USBPort");
            const auto name =
                getString(interfaces, spiObjectInterfaces, "Name");
            const auto programmer =
                getString(interfaces, spiObjectInterfaces, "Programmer");
            const auto type =
                getString(interfaces, spiObjectInterfaces, "Type");
            const auto chipSelect = std::to_string(
                getUint64(interfaces, spiObjectInterfaces, "ChipSelect"));
            const auto inventoryObjPath =
                getString(interfaces, spiObjectInterfaces, "InventoryObjPath");

            auto activeGpios = std::vector<std::pair<std::string, bool>>();
            auto deactiveGpios = std::vector<std::pair<std::string, bool>>();
            for (auto i = 0; i < maxGpios; i++)
            {
                auto gpioCfgInterface = std::string(spiObjectInterfaces) +
                                        ".Gpio" + std::to_string(i);
                if (interfaces.contains(gpioCfgInterface))
                {
                    auto gpioLine =
                        getString(interfaces, gpioCfgInterface, "GpioLine");
                    auto valueStr =
                        getString(interfaces, gpioCfgInterface, "ActiveValue");
                    bool value = (valueStr == "high") ? true : false;
                    activeGpios.push_back(std::make_pair(gpioLine, value));

                    valueStr = getString(interfaces, gpioCfgInterface,
                                         "DeactiveValue");
                    value = (valueStr == "high") ? true : false;
                    deactiveGpios.push_back(std::make_pair(gpioLine, value));
                }
                else
                {
                    break;
                }
            }

            auto chassisName = emObjectPath.parent_path().filename();
            auto objPath = inventoryObjPath + "/" + chassisName + "/SPI";
            lg2::info("[SPI: {NAME}] Creating SPI object: {OBJ_PATH}", "NAME",
                      chassisName, "OBJ_PATH", objPath);
            spiDevices.push_back(std::make_unique<Spi>(
                getBus(), objPath, usbPort, chassisName, programmer, type,
                chipSelect, activeGpios, deactiveGpios));
        }
    }
}

/**
 * @brief function to check if SPI objects are present in Entity Manager
 *
 * @return bool True if SPI objects are found, false otherwise
 */
bool isSpiObjectPresent()
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    const auto managedObjects =
        dbusUtil.getManagedObjects(entityManagerService, inventoryRootPath);
    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (interfaces.contains(spiObjectInterfaces))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief function to try populating SPI objects, with fallback to D-Bus match
 * rule if no SPI objects are present
 *
 * @return void
 */
void tryPopulateSpiObjects()
{
    // If no SPI objects are present, add a match rule to populate the objects
    if (!isSpiObjectPresent())
    {
        inventoryObjectMatch = std::make_unique<sdbusplus::bus::match_t>(
            getBus(), MatchRules::interfacesAdded(inventoryRootPath),
            []([[maybe_unused]] sdbusplus::message::message& msg) {
                sdbusplus::message::object_path objPath;
                std::map<std::string, std::map<std::string, Value>> interfaces;
                msg.read(objPath, interfaces);

                for (const auto& [interfaceName, properties] : interfaces)
                {
                    if (interfaceName == spiObjectInterfaces)
                    {
                        populateSpiObjects();
                        inventoryObjectMatch.reset();
                        break;
                    }
                }
            });
    }
    else
    {
        populateSpiObjects();
    }
}

/**
 * @brief main function to start the flashrom-wrapper service
 *
 * @return int Exit code (0 for success, EXIT_FAILURE for error)
 */
int main()
{
    lg2::info("Starting flashrom-wrapper service...");

    try
    {
        auto& conn = getAsioConnection();
        sdbusplus::asio::object_server objServer(conn);

        auto& bus = getBus();
        bus.request_name("com.Nvidia.FlashromWrapper");

        if (!initDumpFolder())
        {
            lg2::error("Failed to initialize dump folder");
            return EXIT_FAILURE;
        }

        tryPopulateSpiObjects();
        conn->get_io_context().run();

        return 0;
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception: {ERR}", "ERR", e.what());
        return EXIT_FAILURE;
    }
}