#include "force_recovery.hpp"
#include "get_recovery_status.hpp"
#include "perform_rcm_recovery.hpp"
#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>

int main(int argc, char *argv[]) {
  CLI::App app{"USB RCM Recovery Tool for NVIDIA Vera CPU"};
  app.require_subcommand(1);

  bool verbose = false;

  auto getStatusCmd = app.add_subcommand(
      "GetRecoveryStatus",
      "Query recovery status for all Vera devices (outputs JSON array)");
  getStatusCmd->add_flag("-v,--verbose", verbose, "Enable verbose output");

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
      ->add_option("-i,--images", imagePaths,
                   "Recovery image file paths (e.g., image1.bin image2.bin)")
      ->required();
  performRecoveryCmd
      ->add_option("-b,--blob", blobPath,
                   "DOT blob file path (optional, error if required but missing)");
  performRecoveryCmd->add_flag("-v,--verbose", verbose,
                               "Enable verbose diagnostic output");

  std::string configType;
  auto forceRecoveryCmd = app.add_subcommand(
      "SetForceRecovery", "Force device(s) into USB RCM recovery mode");
  forceRecoveryCmd
      ->add_option("-c,--config", configType,
                   "Config type:\n"
                   "  c2   - E5010, E5020, Vera C2 MGX aka P5035\n"
                   "  c1g2 - PG558 aka Strata single board config\n"
                   "  c2g4 - PG558 aka Strata dual board config")
      ->required();

  CLI11_PARSE(app, argc, argv);

  try {
    if (getStatusCmd->parsed()) {
      nlohmann::json jsonOutput;
      if (!getRecoveryStatus(jsonOutput, verbose)) {
        // Output the error JSON (formatted)
        std::cout << jsonOutput.dump(2) << '\n';
        return EXIT_FAILURE;
      }

      // Output the success JSON (formatted)
      std::cout << jsonOutput.dump(2) << '\n';

    } else if (performRecoveryCmd->parsed()) {
      nlohmann::json jsonOutput;
      
      if (!performUsbRecovery(portPath, imagePaths, blobPath, jsonOutput, verbose)) {
        // Output the error JSON (formatted)
        std::cout << jsonOutput.dump(2) << '\n';
        return EXIT_FAILURE;
      }

      // Output the success JSON (formatted)
      std::cout << jsonOutput.dump(2) << '\n';

    } else if (forceRecoveryCmd->parsed()) {
      nlohmann::json jsonOutput;
      forceRecoveryMode(configType, jsonOutput);
      
      // Output the result JSON (formatted)
      std::cout << jsonOutput.dump(2) << '\n';
      
      // Check top-level Status field for exit code
      if (jsonOutput.contains("Status") && jsonOutput["Status"] == "Failed") {
        return EXIT_FAILURE;
      }
    }

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
