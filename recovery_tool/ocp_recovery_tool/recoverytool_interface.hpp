#pragma once

#include "recoverytool_utils.hpp"

#include <CLI/CLI.hpp>

namespace recovery_tool
{

namespace interface

{
/**
 * @class CommandInterface
 * @brief Provides an interface for command operations.
 *
 */
class CommandInterface
{

  public:
    /**
     * @brief Constructs a new CommandInterface object.
     *
     * Initializes the object with provided parameters and sets up CLI options.
     *
     * @param busAddr Bus address for the command operation.
     * @param slaveAddr Slave address for the device.
     * @param app Pointer to the CLI app to add options to.
     */
    explicit CommandInterface(int busAddr, int slaveAddr, CLI::App* app) :
        busAddress(busAddr), slaveAddress(slaveAddr), verbose(false),
        emulation(false)
    {
        app->add_option("-b,--bus", busAddress, "Bus address")->required();
        app->add_option("-s,--slave", slaveAddress, "Slave address")
            ->required();
        app->add_flag("-v,--verbose", verbose, "Verbose output");
        app->add_flag("-e,--emulation", emulation, "To test emulation setup");
        app->callback([&]() { exec(); });
    }

    /**
     * @brief Virtual destructor to ensure correct cleanup in derived classes.
     */
    virtual ~CommandInterface() = default;

    /**
     * @brief Pure virtual method for command execution.
     *
     * Derived classes must provide an implementation.
     */
    virtual void exec() = 0;

  protected:
    int busAddress;
    int slaveAddress;
    bool verbose;
    bool emulation;
};

/**
 * @brief Registers all subcommands for the recovery tool.
 * @param app Reference to the CLI app to register the commands.
 */
void registerCommand(CLI::App& app);
} // namespace interface

} // namespace recovery_tool
