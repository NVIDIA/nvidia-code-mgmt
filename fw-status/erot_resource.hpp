#pragma once 

#include <memory>
#include <format>

#include "mctp_discovery_resource.hpp"
#include "glacier_recovery_commands.hpp"
#include "ap_resource.hpp"

class APResource;


/**@class ERoTResource
 *
 *  Represents a MCTPDiscoveryResource whose recovery is performed through
 *  the Glacier Crisis recovery Protocol
 *
 *
 */
class ERoTResource : public MCTPDiscoveryResource
{
    public:
        /**@brief Constructor for the ERoTResource Class
         * Updates Health and Status of the D-Bus object on startup
         *
         * @param bus - SystemD bus to publish the object
         * @param objPath - Path of D-Bus object to publish
         * @param i2cBus - I2C Bus where the resource is present
         * @param i2cAddress - I2C Address of the resource
         * @param uuid - UUID of the Resource
         *
         */
        ERoTResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const uint64_t i2cBus, const uint64_t i2cAddress, const std::string& uuid) :
            MCTPDiscoveryResource(bus, objPath, uuid)
        {
            glacierRecoveryObj = std::make_unique<glacier_recovery_tool::glacier_recovery_commands::GlacierRecoveryCommands>(i2cBus, i2cAddress, false);

            updateHealth();
        }

        /**@brief Constructor for the ERoTResource Class
         * when AP FW configuration is provided
         *
         * @param bus - SystemD bus to publish the object
         * @param objPath - Path of D-Bus object to publish
         * @param i2cBus - I2C Bus where the resource is present
         * @param i2cAddress - I2C Address of the resource
         * @param uuid - UUID of the Resource
         * @param apEid - EID of the AP associated with the ERoT
         * @param chassisObjPath - Path of the Chassis D-Bus object to publish BootStatus
         * @param apObjPath - Path of the AP D-Bus object to publish Health/State
         * @param mctpVdmHelper - MCTP VDM helper object 
         * @param isRecoverable - Indicates whether recovery can be performed on the Resource
         *
         */
        ERoTResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                const uint64_t i2cBus, const uint64_t i2cAddress, const std::string& uuid,
                const uint64_t apEid, const std::string chassisObjPath, const std::string apObjPath,
                const bool isRecoverable, std::shared_ptr<MCTPVdmHelper> mctpVdmHelper) :
            MCTPDiscoveryResource(bus, objPath, uuid),
            mctpVdmHelper(mctpVdmHelper),
            isRecoverable(isRecoverable)
        {
            glacierRecoveryObj = std::make_unique<glacier_recovery_tool::glacier_recovery_commands::GlacierRecoveryCommands>(i2cBus, i2cAddress, false);
            bootStatus = std::make_unique<BootStatus>(bus, chassisObjPath);
            bootStatus->bootStatusType(BootStatusServer::BootStatusTypes::ERoTBootStatus);
            apResource = std::make_unique<APResource>(bus, apObjPath, apEid, this);

            updateHealth();
        }


        /**@brief Updates the BootStatus of the AP on chassis D-Bus object
         *
         * @return coroutine
         *
         */
        mctp_vdm::requester::Coroutine updateBootStatusAsync();

        /**@brief Updates the BootStatus D-Bus object
         *
         * @return coroutine
         *
         */
        void updateBootStatus()
        {
            if (co)
            {
                if (co.done())
                {
                    co.destroy();
                }
                co = nullptr;
            }
            auto rc = updateBootStatusAsync();
            co = rc.handle;
            return;
        }

        std::vector<uint8_t> getBootStatus() const noexcept;

    private:
        std::unique_ptr<glacier_recovery_tool::glacier_recovery_commands::GlacierRecoveryCommands> glacierRecoveryObj;
        std::unique_ptr<APResource> apResource;
        std::shared_ptr<MCTPVdmHelper> mctpVdmHelper;
        std::unique_ptr<BootStatus> bootStatus;
        bool isRecoverable;
        std::coroutine_handle<mctp_vdm::requester::Coroutine::promise_type> co;


        /* @brief Override function for updating Health and Status of D-Bus object
         * based on Device Status and MCTP enumeration
         * Uses Glacier Crisis Recovery Protocol to fetch device status
         *
         * Updates Health/State of the AP FW if available
         *
         * @return void
         */
        void updateHealth() override
        {
            if (bootStatus)
            {
                updateBootStatus();
            }
            if (apResource)
            {
                apResource->updateHealth();
            }

            if (!isRecoverable)
            {
                return;
            }

            if (MCTPDiscoveryResource::isDeviceEnumerated() and MCTPDiscoveryResource::checkForEnabledMCTPEids())
            {
                lg2::info("MCTP EID for {PATH} is enumerated and enabled",
                        "PATH", path.c_str());
                health(HealthServer::HealthType::OK);
                state(OperationalStatusServer::StateType::Enabled);
                return;
            }

            if (!glacierRecoveryObj->unlockI2CDevice())
            {
                lg2::error("Unable to unlock I2C for object {OBJECT}", 
                        "OBJECT", path.c_str());
                health(HealthServer::HealthType::Critical);
                if (MCTPDiscoveryResource::isDeviceEnumerated())
                {
                    state(OperationalStatusServer::StateType::UnavailableOffline);
                    return;
                }
                
                state(OperationalStatusServer::StateType::Absent);
                return;
            }

            const auto& status = glacierRecoveryObj->performInitialization();

            if (status != glacier_recovery_tool::glacier_recovery_commands::
                                RecoveryResult::FirmwareNotInRecovery)
            {
                lg2::info("Device associated with {PATH} is in recovery",
                        "PATH", path.c_str());

                health(HealthServer::HealthType::Critical);
                state(OperationalStatusServer::StateType::StandbyOffline);
                return;
            }

            lg2::info("Device associated with {PATH} is not in recovery",
                    "PATH", path.c_str());
            health(HealthServer::HealthType::OK);
            state(OperationalStatusServer::StateType::Enabled);
            return;

        }
};


