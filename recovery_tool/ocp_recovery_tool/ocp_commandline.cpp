#include "config.h"

#include "dbusutils.hpp"
#include "message_registry.hpp"
#include "recovery_commandline.hpp"
#include "recoverytool_utils.hpp"
#include "usb_i2c_mapper.hpp"

#include <CLI/CLI.hpp>
#include <phosphor-logging/lg2.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>

using RecoveryReturnCode = ocp_recovery_commandline::RecoveryReturnCode;
using namespace phosphor::logging;

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto ocpObjInterface =
    "xyz.openbmc_project.Configuration.OCPRecovery";

struct CommandOptions
{
    std::string configPath;
    std::string baseImagePath; // Base path containing component directories
    std::string fspImagePath;
    std::string buildInfoImagePath;
    std::string oobhubImagePath;
    std::string fspRtImagePath;
    bool forceRecovery;
    bool verbose;
    bool emulation;
};

auto& getBus()
{
    static auto bus = sdbusplus::bus::new_default();
    return bus;
}

/**
 * @brief Retrieves the I2C bus number for a given D-Bus object
 *
 * This function queries the D-Bus object to get the I2C bus number property
 * defined in its interface.
 *
 * @param[in] objPath - The D-Bus object path to query
 * @param[in] interface - The interface name containing the I2C bus property
 * @return uint32_t - The I2C bus number
 */
uint32_t getI2CBus(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto i2cBus = dbusUtil.getProperty<uint64_t>(
        entityManagerService, objPath.c_str(), interface.c_str(), "I2CBus");

    return i2cBus;
}

/**
 * @brief Retrieves the I2C address for a given D-Bus object
 *
 * This function queries the D-Bus object to get the I2C address property
 * defined in its interface.
 *
 * @param[in] objPath - The D-Bus object path to query
 * @param[in] interface - The interface name containing the I2C address property
 * @return uint32_t - The I2C address
 */
uint32_t getAddress(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto i2cAddress = dbusUtil.getProperty<uint64_t>(
        entityManagerService, objPath.c_str(), interface.c_str(), "I2CAddress");

    return i2cAddress;
}

/**
 * @brief Retrieves the USB port for a given D-Bus object
 *
 * This function queries the D-Bus object to get the USB port property
 * defined in its interface.
 *
 * @param[in] objPath - The D-Bus object path to query
 * @param[in] interface - The interface name containing the USB port property
 * @return std::string - The USB port identifier
 */
std::string getUSBPort(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto usbPort = dbusUtil.getProperty<std::string>(
        entityManagerService, objPath.c_str(), interface.c_str(), "USBPort");

    return usbPort;
}

/**
 * @brief Checks if a D-Bus object has a USB port property
 *
 * This function queries the D-Bus object to check if it has a USB port property
 * defined in its interface.
 *
 * @param[in] objPath - The D-Bus object path to check
 * @param[in] interface - The interface name to check for the property
 * @return bool - True if the object has a USB port property, false otherwise
 */
bool hasUSBPortProperty(const std::string& objPath,
                        const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    try
    {
        // Try to get the USB port property - if it exists, return true
        dbusUtil.getProperty<std::string>(entityManagerService, objPath.c_str(),
                                          interface.c_str(), "USBPort");
        return true;
    }
    catch (const std::exception& e)
    {
        // If we get an exception, the property doesn't exist
        return false;
    }
}

struct DeviceRecoveryTarget
{
    std::string device;
    uint32_t busAddr;
    uint32_t slaveAddr;
};

static std::vector<DeviceRecoveryTarget> collectRecoveryTargets(
    const nvidia::software::updater::ObjectValueTree& managedObjects,
    bool verbose)
{
    std::vector<DeviceRecoveryTarget> targets;
    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (!interfaces.contains(ocpObjInterface))
        {
            continue;
        }

        const std::string objPathStr = emObjectPath.str;
        lg2::info("Found OCP recovery config Object: {PATH}", "PATH",
                  objPathStr);

        const auto device =
            std::filesystem::path(objPathStr).filename().string();
        uint32_t busAddr = 0;

        if (hasUSBPortProperty(objPathStr, ocpObjInterface))
        {
            const auto usbPort = getUSBPort(objPathStr, ocpObjInterface);
            const auto i2cBus =
                recovery_tool::usb_i2c::getI2CBusFromUSBPort(usbPort, verbose);

            if (i2cBus < 0)
            {
                lg2::error("Failed to get I2C bus from USB port {USBPORT}",
                           "USBPORT", usbPort);
                continue;
            }
            busAddr = i2cBus;
        }
        else
        {
            busAddr = getI2CBus(objPathStr, ocpObjInterface);
        }

        const auto slaveAddr = getAddress(objPathStr, ocpObjInterface);
        targets.push_back({device, busAddr, static_cast<uint32_t>(slaveAddr)});
    }
    return targets;
}

static bool performRecoveryForDevice(const DeviceRecoveryTarget& target,
                                     const CommandOptions& opts)
{
    try
    {
        ocp_recovery_commandline::OCPRecoveryCommandLine
            ocpRecoveryCommandlineObj(target.device, target.busAddr,
                                      target.slaveAddr, opts.verbose,
                                      opts.emulation);

        auto status = ocpRecoveryCommandlineObj.performRecovery(
            {opts.fspImagePath, opts.buildInfoImagePath, opts.oobhubImagePath,
             opts.fspRtImagePath});

        return status == RecoveryReturnCode::FAILURE;
    }
    catch (const std::exception& e)
    {
        lg2::error("Recovery failed due to exception {EXCEPTION}", "EXCEPTION",
                   e.what());
        return true;
    }
}

bool performRecovery(const CommandOptions& opts)
{
    auto& bus = getBus();
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    const auto managedObjects = dbusUtil.getManagedObjects(
        entityManagerService, entityManagerObjManager);
    bool retCode = false;
    std::unique_ptr<MessageRegistry> messageRegistry =
        std::make_unique<MessageRegistry>(bus);

    if (managedObjects.empty())
    {
        lg2::error("No Devices found to recover");
        messageRegistry->createMessageRegistryResourceErrors(
            resourceErrorsDetected, RecoveryProtocol::OCPRecoveryProtocolError,
            static_cast<ErrorCode>(noDevicesFound), "OCPRecovery");
        return true;
    }

    auto targets = collectRecoveryTargets(managedObjects, opts.verbose);
    if (targets.empty())
    {
        return retCode;
    }

    std::vector<std::future<bool>> futures;
    futures.reserve(targets.size());
    for (const auto& target : targets)
    {
        futures.emplace_back(std::async(std::launch::async, [&opts, target]() {
            return performRecoveryForDevice(target, opts);
        }));
    }

    for (auto& future : futures)
    {
        if (future.get())
        {
            retCode = true;
        }
    }
    return retCode;
}

/**
 * @brief Helper to get the recovery image path from the directory path
 * @param dirPath The directory path to search for the recovery image. Expected
 * structure: baseImagePath/<CompID>
 * @return The recovery image path if found, empty string otherwise
 */
std::string getRecoveryImagePath(const std::string& dirPath)
{
    namespace fs = std::filesystem;
    try
    {
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath))
        {
            return "";
        }
        for (const auto& entry : fs::directory_iterator(dirPath))
        {
            if (fs::is_regular_file(entry.path()))
            {
                return entry.path().string();
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Error finding file in directory: {ERROR}", "ERROR",
                   e.what());
    }
    return "";
}

int main(int argc, char** argv)
try
{
    CLI::App app{"Command line interface for OCP recovery"};
    CommandOptions opts{};

    app.add_option("base_image_path", opts.baseImagePath,
                   "Base directory containing component image subdirectories")
        ->required()
        ->check(CLI::ExistingPath);

    app.add_flag("-v,--verbose", opts.verbose, "Verbose output");
    app.add_flag("-e,--emulation", opts.emulation,
                 "Enable for emulation setup");

    try
    {
        CLI11_PARSE(app, argc, argv);

        // Construct image paths from base directory using component IDs
        // Expected structure: baseImagePath/<CompID>/<image_file>
        opts.fspImagePath = getRecoveryImagePath(opts.baseImagePath + "/" +
                                                 GPU_OCP_FSP_COMP_ID);
        opts.buildInfoImagePath = getRecoveryImagePath(
            opts.baseImagePath + "/" + GPU_OCP_BUILD_INFO_COMP_ID);
        opts.oobhubImagePath = getRecoveryImagePath(opts.baseImagePath + "/" +
                                                    GPU_OCP_OOBHUB_COMP_ID);
        opts.fspRtImagePath = getRecoveryImagePath(opts.baseImagePath + "/" +
                                                   GPU_OCP_FSP_RT_COMP_ID);

        if (opts.fspImagePath.empty() || opts.buildInfoImagePath.empty() ||
            opts.oobhubImagePath.empty() || opts.fspRtImagePath.empty())
        {
            lg2::error(
                "Failed to find required images in base directory: {PATH}",
                "PATH", opts.baseImagePath);
            lg2::error(
                "FSP: {FSP}, BUILD_INFO: {BUILD_INFO}, OOB: {OOB}, FSP_RT: {FSP_RT}",
                "FSP", opts.fspImagePath, "BUILD_INFO", opts.buildInfoImagePath,
                "OOB", opts.oobhubImagePath, "FSP_RT", opts.fspRtImagePath);
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return performRecovery(opts);
}
catch (const std::exception&)
{
    return EXIT_FAILURE;
}
catch (...)
{
    return EXIT_FAILURE;
}
