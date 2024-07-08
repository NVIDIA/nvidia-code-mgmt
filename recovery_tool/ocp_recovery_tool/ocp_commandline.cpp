#include "recoverytool_utils.hpp"
#include "recovery_commandline.hpp"
#include "message_registry.hpp"
#include "dbusutils.hpp"

#include <CLI/CLI.hpp>
#include <phosphor-logging/lg2.hpp>

using RecoveryReturnCode = ocp_recovery_commandline::RecoveryReturnCode ;
using namespace phosphor::logging;

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto entityManagerObjManager = "/xyz/openbmc_project/inventory";
constexpr auto ocpObjInterface = "xyz.openbmc_project.Configuration.OCPRecovery";

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

std::pair<uint32_t, uint32_t> getI2CBusAndAddress(const std::string& objPath, const std::string& interface)
{
    auto dbusUtil = nvidia::software::updater::DBUSUtils(getBus());
    auto i2cBus = dbusUtil.getProperty<uint64_t>(entityManagerService, objPath.c_str(),
            interface.c_str(), "I2CBus");
    auto i2cAddress = dbusUtil.getProperty<uint64_t>(entityManagerService, objPath.c_str(),
            interface.c_str(), "I2CAddress");

    return {i2cBus, i2cAddress};
}

bool performRecovery(const CommandOptions& opts)
{
    auto& bus = getBus();
    auto dbusUtil = nvidia::software::updater::DBUSUtils(bus);
    const auto managedObjects = dbusUtil.getManagedObjects(entityManagerService, entityManagerObjManager);
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

        lg2::info("Found OCP recovery config Object: {PATH}", "PATH", emObjectPath);
        const auto [busAddr, slaveAddr] = getI2CBusAndAddress(emObjectPath, ocpObjInterface);
        const auto& device = emObjectPath.filename();
        ocp_recovery_commandline::OCPRecoveryCommandLine ocpRecoveryCommandlineObj(device,
            busAddr, slaveAddr, opts.verbose, opts.emulation);
        auto status = ocpRecoveryCommandlineObj.performRecovery({opts.fspImagePath, opts.oobhubImagePath});
        if (status == RecoveryReturnCode::FAILURE)
        {
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
    app.add_flag("-e,--emulation", opts.emulation, "Enable for emulation setup");

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
