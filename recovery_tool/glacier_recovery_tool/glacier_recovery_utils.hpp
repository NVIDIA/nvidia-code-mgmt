#pragma once

#include "glacier_recovery_commands.hpp"

#include <nlohmann/json.hpp>

namespace glacier_recovery_tool
{

/**
 * @class GlacierRecoveryTool
 * @brief Utility for managing and performing recovery operations on devices
 * that support glacier recovery protocol
 *
 * This class provides functionalities for querying device status,
 * getting recovery status, and performing recovery operations.
 */
class GlacierRecoveryTool
{

  private:
    bool verbose;
    glacier_recovery_commands::GlacierRecoveryCommands recoveryCommands;

  public:
    /**
     * @brief Constructs a Glacier Recovery Tool instance..
     *
     * @param busAddr The bus address for I2C communication.
     * @param slaveAddr The slave address for I2C communication.
     * @param verbose Set to true to enable verbose logging.
     */
    GlacierRecoveryTool(int busAddr, int slaveAddr, bool verbose);
    GlacierRecoveryTool() = delete;
    GlacierRecoveryTool(const GlacierRecoveryTool&) = delete;
    GlacierRecoveryTool(GlacierRecoveryTool&&) = delete;
    GlacierRecoveryTool& operator=(const GlacierRecoveryTool&) = delete;
    GlacierRecoveryTool& operator=(GlacierRecoveryTool&&) = delete;
    ~GlacierRecoveryTool() = default;
    /**
     * @brief Retrieves the device's firmware information in JSON format.
     *
     * @return A JSON object representing the device's status.
     */
    nlohmann::json getFirmwareInfoJson();

    /**
     * @brief Retrieves the recovery status in JSON format.
     *
     * @return A JSON object representing the recovery status.
     */
    nlohmann::json getRecoveryStatusJson();

    /**
     * @brief Initiates the recovery process using the specified image path.
     *
     * @param imgPath image path to use for recovery.
     * @return A JSON object indicating the result of the recovery process.
     */
    nlohmann::json performRecovery(const std::string& imgPath);
};

} // namespace glacier_recovery_tool
