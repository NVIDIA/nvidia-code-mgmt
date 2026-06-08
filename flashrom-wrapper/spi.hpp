#pragma once

#include <boost/asio.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <com/nvidia/GraceSPI/server.hpp>
#include <com/nvidia/GraceSPIData/server.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/server.hpp>
#include <xyz/openbmc_project/Common/Progress/server.hpp>
#include <xyz/openbmc_project/Software/ApplyTime/server.hpp>
#include <xyz/openbmc_project/Software/Update/server.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using ApplyTimeIntf =
    sdbusplus::xyz::openbmc_project::Software::server::ApplyTime;
using SpiIntf = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::GraceSPI,
    sdbusplus::xyz::openbmc_project::Software::server::Update>;
using UpdateIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Software::server::Update>;

using SpiProgress = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::common::Progress,
    sdbusplus::com::nvidia::server::GraceSPIData>;

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";
constexpr auto inventoryRootPath = "/xyz/openbmc_project/inventory";
constexpr auto spiObjectInterfaces = "xyz.openbmc_project.Configuration.SPI";
constexpr auto spiStatusPath = "/xyz/openbmc_project/status/SPI_Operation";
constexpr size_t maxProgressHistory = 3;
constexpr auto maxSpiDumps = 3;
constexpr auto maxGpios = 10;
constexpr auto spiDumpDir = "/var/emmc/user-logs/logging/spi_dumps";

// Forward declarations for USB port management functions
bool isUsbInUse(const std::string& usbPort);
bool markUsbInUse(const std::string& usbPort);
void releaseUsb(const std::string& usbPort);

// Mapping table structure for chip types and their expected operation times
struct ChipTiming
{
    std::string chipName;
    int eraseTimeSeconds;
    int readTimeSeconds;
    int writeTimeSeconds;
};

enum class SpiFailureReason
{
    None,
    HostPowerNotOff,
    Unavailable,
};

class Spi : public SpiIntf
{
    enum class Operation
    {
        Erase,
        Read,
        Write
    };

  public:
    Spi(sdbusplus::bus_t& bus, const std::string& objPath,
        const std::string& usbPort, const std::string& name,
        const std::string& programmer, const std::string& type,
        const std::string& chipSelect,
        const std::vector<std::pair<std::string, bool>>& activeGpios,
        const std::vector<std::pair<std::string, bool>>& deactiveGpios) :
        SpiIntf(bus, objPath.c_str(), action::emit_interface_added),
        usbPort(usbPort), name(name), programmer(programmer), chip(""),
        type(type), chipSelect(chipSelect), activeGpios(activeGpios),
        deactiveGpios(deactiveGpios)
    {
        if (!getUsbBusNum())
        {
            std::cerr << "Failed to get USB Bus number" << std::endl;
        }
    }
    ~Spi() = default;

    /**
     * @brief D-Bus method to erase SPI flash memory
     *
     * @return sdbusplus::object_path The object path of the progress
     * tracking object
     */
    sdbusplus::object_path eraseSpi();

    /**
     * @brief D-Bus method to read SPI flash memory
     *
     * @return sdbusplus::object_path The object path of the progress
     * tracking object
     */
    sdbusplus::object_path readSpi();

    /**
     * @brief D-Bus method to write/update SPI flash memory with new firmware
     * image
     *
     * @param image File descriptor of the firmware image to be written to SPI
     * flash
     * @param applyTime Requested time to apply the update (currently unused)
     * @param forceUpdate Flag to force update even if version check fails
     * (currently unused)
     * @param targets List of target object paths for the update (currently
     * unused)
     * @return sdbusplus::object_path The object path of the progress
     * tracking object
     */
    sdbusplus::object_path startUpdate(
        sdbusplus::message::unix_fd image,
        ApplyTimeIntf::RequestedApplyTimes applyTime [[maybe_unused]],
        bool forceUpdate [[maybe_unused]],
        std::vector<sdbusplus::object_path> targets [[maybe_unused]]);

    /**
     * @brief function to prepare command line arguments for flashrom operation
     *
     * @param ops The operation type (Erase, Read, or Write)
     * @return std::vector<std::string> Vector of command line arguments
     */
    std::vector<std::string> prepareArgs(Operation ops);

    /**
     * @brief function to start SPI operation
     *
     * @param ops The operation type (Erase or Read)
     * @return SpiFailureReason::None if operation started successfully,
     *         otherwise the reason the operation could not start
     */
    SpiFailureReason startSpiOperation(Operation ops);

    /**
     * @brief function to execute flashrom command with given arguments
     *
     * @param args Vector of command line arguments for flashrom
     * @param ops The operation type (Erase, Read, or Write)
     * @return void
     */
    void executeFlashrom(std::vector<std::string> args, Operation ops);

    /**
     * @brief function to finish SPI operation and update status
     *
     * @param opStatus The final operation status
     * @return void
     */
    void finishSpiOperation(SpiProgress::OperationStatus opStatus);

    /**
     * @brief function to get the current progress object
     *
     * @return SpiProgress* Pointer to current progress object, nullptr if no
     * progress
     */
    SpiProgress* getCurrentProgressObj()
    {
        return progressHistory.empty() ? nullptr : progressHistory.back().get();
    }

    /**
     * @brief function to set SPI multiplexer according to the GPIO
     * configuration
     *
     * @return void
     */
    void setSpiMux();

    /**
     * @brief function to reset SPI multiplexer according to the GPIO
     * configuration
     *
     * @return void
     */
    void resetSpiMux();

    /**
     * @brief function to get USB bus number from the USB port string
     *
     * @return bool True if successful, false if failed
     */
    bool getUsbBusNum();

    /**
     * @brief function to get USB device number from the
     * /sys/bus/usb/devices/USB_PORT/devnum
     *
     * @return bool True if successful, false if failed
     */
    bool getUsbDevNum();

    /**
     * @brief function to check and clean up old dump files if exceeding max
     * limit
     *
     * @return bool True if cleanup successful, false otherwise
     */
    bool checkDumpFiles();

    /**
     * @brief function to dynamically detect the SPI chip model by running
     * flashrom without -c parameter
     *
     * @return std::string The detected chip model name, empty string if
     * detection failed
     */
    std::string detectChipModel();

  private:
    std::deque<std::unique_ptr<SpiProgress>> progressHistory;
    std::deque<std::pair<std::string, int>> dumpFiles;
    std::string dumpFile;
    std::string filePath;
    std::string usbPort;
    std::string name;
    std::string programmer;
    std::string chip;
    std::string type;
    std::string chipSelect;
    std::vector<std::pair<std::string, bool>> activeGpios;
    std::vector<std::pair<std::string, bool>> deactiveGpios;
    std::string usbBusNum;
    std::string usbDevNum;

    // Timer for progress updates
    std::shared_ptr<boost::asio::steady_timer> progressTimer;
    // Timer for operation timeout
    std::shared_ptr<boost::asio::steady_timer> timeoutTimer;
    // dbus match for monitoring host power state changes
    std::unique_ptr<sdbusplus::bus::match_t> hostPowerStateMatch;
    // Reference to current flashrom process for timeout handling
    std::shared_ptr<boost::process::v2::process> currentProcess;
    int expectedOpTimeSec;
    int currentProgress;

    // Mapping table for chip types and their expected operation times
    static const std::vector<ChipTiming> chipTimingMap;

    /**
     * @brief function to update progress status during operation
     *
     * @return void
     */

    void updateProgress();
    /**
     * @brief function to handle host power state changes via dbus signal
     *
     * @param msg The dbus message containing property changes
     * @return void
     */
    void onHostPowerStateChanged(sdbusplus::message_t& msg);
    /**
     * @brief function to get expected operation time based on chip type
     *
     * @param ops The operation type (Erase or Read)
     * @return int Expected time in seconds
     */
    int getExpectedTime(Operation ops) const;
};
