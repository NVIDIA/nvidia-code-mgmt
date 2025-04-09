#include "dbusutils.hpp"
#include "message_registry.hpp"
#include "recovery_commandline.hpp"
#include "recoverytool_utils.hpp"
#include "usb_i2c_mapper.hpp"

#include <CLI/CLI.hpp>
#include <phosphor-logging/lg2.hpp>

using RecoveryReturnCode = ocp_recovery_commandline::RecoveryReturnCode;
using namespace phosphor::logging;

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto ocpObjInterface =
    "xyz.openbmc_project.Configuration.OCPRecovery";

struct CommandOptions
{
    std::string configPath;
    std::string fspImagePath;
    std::string oobhubImagePath;
    std::vector<std::string> imagePaths;
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
        retCode = true;
    }

    for (const auto& [emObjectPath, interfaces] : managedObjects)
    {
        if (!interfaces.contains(ocpObjInterface))
        {
            continue;
        }

        lg2::info("Found OCP recovery config Object: {PATH}", "PATH",
                  emObjectPath);

        const auto& device = emObjectPath.filename();

        uint32_t busAddr = 0;
        if (hasUSBPortProperty(emObjectPath, ocpObjInterface))
        {
            const auto usbPort = getUSBPort(emObjectPath, ocpObjInterface);
            const auto i2cBus = recovery_tool::usb_i2c::getI2CBusFromUSBPort(
                usbPort, opts.verbose);

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
            busAddr = getI2CBus(emObjectPath, ocpObjInterface);
        }

        const auto slaveAddr = getAddress(emObjectPath, ocpObjInterface);

        ocp_recovery_commandline::OCPRecoveryCommandLine
            ocpRecoveryCommandlineObj(device, busAddr, slaveAddr, opts.verbose,
                                      opts.emulation);
        try
        {
            auto status = ocpRecoveryCommandlineObj.performRecovery(
                {opts.fspImagePath, opts.oobhubImagePath});
            if (status == RecoveryReturnCode::FAILURE)
            {
                retCode = true;
            }
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            lg2::error("Recovery failed due to exception {EXCEPTION}",
                       "EXCEPTION", e.what());
            retCode = true;
        }
    }
    return retCode;
}

int main(int argc, char** argv)
{
    CLI::App app{"Command line interface for OCP recovery"};
    CommandOptions opts{};

    app.add_option("oobhub_image", opts.oobhubImagePath, "Path to oobhub Image")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("fsp_image", opts.fspImagePath, "Path to FSP Image")
        ->required()
        ->check(CLI::ExistingFile);
    // app.add_flag("force", opts.forceRecovery, "Slave address");
    app.add_flag("-v,--verbose", opts.verbose, "Verbose output");
    app.add_flag("-e,--emulation", opts.emulation,
                 "Enable for emulation setup");

    try
    {
        CLI11_PARSE(app, argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return performRecovery(opts);
}
