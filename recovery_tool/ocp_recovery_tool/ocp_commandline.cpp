#include "recoverytool_utils.hpp"
#include "recovery_commandline.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
using BusAddr = int;
using SlaveAddr = int;
using Address = int;
using Device = std::string;
using DeviceAddressConfig = std::map<Device, std::map<std::string, Address>>;
using RecoveryReturnCode = ocp_recovery_commandline::RecoveryReturnCode ;

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

DeviceAddressConfig parseConfig(const std::string& configPath)
{
    std::ifstream fp(configPath);
    if (!fp.is_open())
    {
        std::cerr << "Failed to open JSON file." << std::endl;
        return {};
    }
    json data{};
    try
    {
        data = json::parse(fp);
    }
    catch (json::parse_error& ex)
    {
        std::cerr << "Failed to parse config json due to error at byte "
                  << ex.what() << std::endl;
    }
    return data.get<DeviceAddressConfig>();
}

bool performRecovery(const CommandOptions& opts)
{
    const auto& configData = parseConfig(opts.configPath);
    bool retCode = false;

    for (const auto& [device, addressMap] : configData)
    {
        const auto busAddr = addressMap.at("Bus Address");
        const auto slaveAddr = addressMap.at("Slave Address");
        ocp_recovery_commandline::OCPRecoveryCommandLine ocpRecoveryCommandlineObj(device,
            busAddr, slaveAddr, opts.verbose, opts.emulation);
        // TODO: Enable when force recovery is in place
        // if (opts.forceRecovery)
        // {
        //     ocpRecoveryCommandlineObj.setForceRecovery();
        // }
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

    app.add_option("config", opts.configPath, "Path to config file")
        ->required()
        ->check(CLI::ExistingFile);
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
