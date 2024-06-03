#include <memory>
#include <format>

#include "mctp_discovery_resource.hpp"
#include "glacier_recovery_commands.hpp"


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

    private:
        std::unique_ptr<glacier_recovery_tool::glacier_recovery_commands::GlacierRecoveryCommands> glacierRecoveryObj;

        /* @brief Override function for updating Health and Status of D-Bus object
         * based on Device Status and MCTP enumeration
         * Uses Glacier Crisis Recovery Protocol to fetch device status
         *
         * @return void
         */
        void updateHealth() override
        {
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


