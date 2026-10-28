// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

/**
 * spi-programmer: worker started by spi-programmer@.service as
 *   spi-programmer <device>... <fw_version> <image_path>
 * Writes the flashrom layout read by flashrom-wrapper, starts the update on
 * every SPI device through com.Nvidia.FlashromWrapper, polls the Progress
 * objects and posts the Redfish events. The exit code is the unit result.
 */

#include "spi_programmer_worker.hpp"

#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <atomic>
#include <csignal>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{

constexpr auto flashromService = "com.Nvidia.FlashromWrapper";
constexpr auto updateInterface = "xyz.openbmc_project.Software.Update";
constexpr auto progressInterface = "xyz.openbmc_project.Common.Progress";
constexpr auto propertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto loggingService = "xyz.openbmc_project.Logging";
constexpr auto loggingPath = "/xyz/openbmc_project/logging";
constexpr auto loggingInterface = "xyz.openbmc_project.Logging.Create";
constexpr auto applyTimeImmediate =
    "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.Immediate";

// The error carries the D-Bus error name and message
void logError(const char* call, const std::string& target,
              const std::exception& e)
{
    lg2::error("{CALL} {TARGET} failed: {ERROR}", "CALL", call, "TARGET",
               target, "ERROR", e.what());
}

class SystemBus : public spi_programmer::Bus
{
  public:
    std::optional<std::string> startUpdate(const std::string& device,
                                           int imageFd) override
    {
        try
        {
            auto& conn = connection();
            auto method = conn.new_method_call(
                flashromService,
                ("/xyz/openbmc_project/inventory/system/" + device + "/SPI")
                    .c_str(),
                updateInterface, "StartUpdate");
            method.append(sdbusplus::message::unix_fd{imageFd},
                          std::string{applyTimeImmediate}, false,
                          std::vector<sdbusplus::object_path>{});
            auto reply = conn.call(method);
            sdbusplus::object_path path;
            reply.read(path);
            return path.str;
        }
        catch (const std::exception& e)
        {
            logError("StartUpdate", device, e);
            return std::nullopt;
        }
    }

    std::optional<std::string> getStatus(const std::string& path) override
    {
        try
        {
            auto value = getProperty(path, "Status");
            if (auto* status = std::get_if<std::string>(&value))
            {
                return *status;
            }
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            logError("Get Status", path, e);
            return std::nullopt;
        }
    }

    void getProgress(const std::string& path) override
    {
        try
        {
            getProperty(path, "Progress");
        }
        catch (const std::exception& e)
        {
            logError("Get Progress", path, e);
        }
    }

    void createLog(const std::string& message, const std::string& severity,
                   const std::map<std::string, std::string>& data) override
    {
        try
        {
            auto& conn = connection();
            auto method = conn.new_method_call(loggingService, loggingPath,
                                               loggingInterface, "Create");
            method.append(message, severity, data);
            conn.call_noreply(method);
        }
        catch (const std::exception& e)
        {
            logError("Logging.Create", message, e);
        }
    }

  private:
    using Value = std::variant<std::string, uint8_t>;

    Value getProperty(const std::string& path, const char* property)
    {
        auto& conn = connection();
        auto method = conn.new_method_call(flashromService, path.c_str(),
                                           propertiesInterface, "Get");
        method.append(progressInterface, property);
        auto reply = conn.call(method);
        Value value;
        reply.read(value);
        return value;
    }

    sdbusplus::bus_t& connection()
    {
        if (!bus)
        {
            bus.emplace(sdbusplus::bus::new_system());
        }
        return *bus;
    }

    std::optional<sdbusplus::bus_t> bus;
};

class StdConsole : public spi_programmer::Console
{
  public:
    void out(std::string_view line) override
    {
        write(STDOUT_FILENO, line);
    }

    void err(std::string_view line) override
    {
        write(STDERR_FILENO, line);
    }

  private:
    static void write(int fd, std::string_view line)
    {
        std::string text{line};
        text += '\n';
        const char* data = text.data();
        size_t size = text.size();
        while (size > 0)
        {
            ssize_t n = ::write(fd, data, size);
            if (n <= 0)
            {
                return;
            }
            data += n;
            size -= static_cast<size_t>(n);
        }
    }
};

std::atomic<spi_programmer::Worker*> activeWorker = nullptr;

extern "C" void onTerminate(int sig)
{
    if (auto* worker = activeWorker.load(); worker != nullptr)
    {
        worker->cleanupFromSignal(STDOUT_FILENO);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

int main(int argc, char** argv)
{
    SystemBus bus;
    StdConsole console;
    spi_programmer::Options options;
    options.programName = argc > 0 ? argv[0] : "spi-programmer";
    spi_programmer::Worker worker(bus, console, options);

    activeWorker = &worker;
    struct sigaction action{};
    action.sa_handler = onTerminate;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGHUP})
    {
        sigaction(sig, &action, nullptr);
    }

    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    int rc = worker.run(args);
    activeWorker = nullptr;
    return rc;
}
