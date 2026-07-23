#include "force_recovery.hpp"
#include "get_recovery_status.hpp"
#include "perform_rcm_recovery.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>

static RecoveryPinConfig loadPinConfig(const std::string& path)
{
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open config file: " + path);
    auto j = nlohmann::json::parse(f);

    RecoveryPinConfig cfg;
    if (j.contains("ResetPins"))
    {
        cfg.resetPins = j.at("ResetPins").get<std::vector<std::string>>();
    }
    else
    {
        cfg.dbusService = j.at("ResetDbusService").get<std::string>();
        cfg.dbusObject = j.at("ResetDbusObject").get<std::string>();
        cfg.dbusInterface = j.at("ResetDbusInterface").get<std::string>();
        cfg.dbusMethod = j.at("ResetDbusMethod").get<std::string>();
    }
    cfg.strapPins = j.at("StrapPins").get<std::vector<std::string>>();
    cfg.strapActiveValues = j.at("StrapActiveValues").get<std::vector<int>>();
    cfg.strapDefaultValues = j.at("StrapDefaultValues").get<std::vector<int>>();

    if (cfg.strapPins.empty() ||
        cfg.strapActiveValues.size() != cfg.strapPins.size() ||
        cfg.strapDefaultValues.size() != cfg.strapPins.size())
        throw std::runtime_error(
            "StrapPins, StrapActiveValues and StrapDefaultValues must be "
            "non-empty and have the same length");

    if (cfg.resetPins.empty() &&
        (cfg.dbusService.empty() || cfg.dbusObject.empty() ||
         cfg.dbusInterface.empty() || cfg.dbusMethod.empty()))
        throw std::runtime_error(
            "Config must have non-empty ResetPins or all four "
            "ResetDbus* fields");

    return cfg;
}

int main(int argc, char* argv[])
{
    try
    {
        CLI::App app{"USB RCM Recovery Tool for NVIDIA Vera CPU"};
        app.require_subcommand(1);

        bool verbose = false;

        auto getStatusCmd = app.add_subcommand(
            "GetRecoveryStatus",
            "Query recovery status for all Vera devices (outputs JSON array)");
        getStatusCmd->add_flag("-v,--verbose", verbose,
                               "Enable verbose output");

        std::string portPath;
        std::vector<std::string> imagePaths;
        std::string blobPath;
        auto performRecoveryCmd = app.add_subcommand(
            "PerformUSBRecovery",
            "Perform USB RCM recovery by sending images to device");
        performRecoveryCmd
            ->add_option("-p,--port-path", portPath,
                         "USB port path (e.g., '1-1.3', '2-4.1') - required")
            ->required();
        performRecoveryCmd
            ->add_option(
                "-i,--images", imagePaths,
                "Recovery image file paths (e.g., image1.bin image2.bin)")
            ->required();
        performRecoveryCmd->add_option(
            "-b,--blob", blobPath,
            "DOT blob file path (optional, error if required but missing)");
        performRecoveryCmd->add_flag("-v,--verbose", verbose,
                                     "Enable verbose diagnostic output");

        std::string configFile;
        auto forceRecoveryCmd = app.add_subcommand(
            "SetForceRecovery", "Force device(s) into USB RCM recovery mode");
        forceRecoveryCmd
            ->add_option(
                "-c,--config-file", configFile,
                "Path to JSON pin config file (ResetPins/ResetDbus* + StrapPins"
                " + StrapActiveValues + StrapDefaultValues)")
            ->required();

        CLI11_PARSE(app, argc, argv);

        if (getStatusCmd->parsed())
        {
            nlohmann::json jsonOutput;
            if (!getRecoveryStatus(jsonOutput, verbose))
            {
                // Output the error JSON (formatted)
                std::cout << jsonOutput.dump(2) << '\n';
                return EXIT_FAILURE;
            }

            // Output the success JSON (formatted)
            std::cout << jsonOutput.dump(2) << '\n';
        }
        else if (performRecoveryCmd->parsed())
        {
            nlohmann::json jsonOutput;

            if (!performUsbRecovery(portPath, imagePaths, blobPath, jsonOutput,
                                    verbose))
            {
                // Output the error JSON (formatted)
                std::cout << jsonOutput.dump(2) << '\n';
                return EXIT_FAILURE;
            }

            // Output the success JSON (formatted)
            std::cout << jsonOutput.dump(2) << '\n';
        }
        else if (forceRecoveryCmd->parsed())
        {
            nlohmann::json jsonOutput;
            forceRecoveryMode(loadPinConfig(configFile), jsonOutput);

            // Output the result JSON (formatted)
            std::cout << jsonOutput.dump(2) << '\n';

            // Check top-level Status field for exit code
            if (jsonOutput.contains("Status") &&
                jsonOutput["Status"] == "Failed")
            {
                return EXIT_FAILURE;
            }
        }

        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
