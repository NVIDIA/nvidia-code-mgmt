#pragma once

#include "base_resource.hpp"

#include <string_view>

constexpr static auto mctpEndpointIntfName{"xyz.openbmc_project.MCTP.Endpoint"};
constexpr static auto mctpEndpointEnableIntfName{"xyz.openbmc_project.Object.Enable"};
constexpr static auto uuidIntfName{"xyz.openbmc_project.Common.UUID"};
constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
constexpr static std::string_view mctpObjPathPrefix = "/xyz/openbmc_project/mctp/0/";


/**@class MCTPDiscoveryResource
 *
 *  Represents a resource which is expected to have one or more associated MCTP Endpoints,
 *  and is capable of triggering MCTP Discovery based on its health
 *
 */
class MCTPDiscoveryResource : public BaseResource
{
    public:
        /**@brief Constructor for the MCTPDiscoveryResource class
         * Creates watchers for MCTP EIDs associated with the resource
         *
         * @param bus - SystemD bus to publish the object
         * @param objPath - Path of D-Bus object to publish
         * @param uuid - UUID of the Resource //TODO: Duplicated from Entity Manager, probably not needed
         *
         */
        MCTPDiscoveryResource(sdbusplus::bus::bus& bus, const std::string& objPath,
                 const std::string& uuid) :
            BaseResource(bus, objPath),
            uuid(uuid)
        {
            startWatchingMCTPObjects();
        }

        /**@brief Fetches EID for the resource
         *
         * @return uint8_t - EID of the resource
         *
         */
        uint8_t fetchEid() const noexcept
        {
            for (const auto& [_, mctpObjPath]: mctpEidObjects)
            {
                return std::stoi(std::filesystem::path(mctpObjPath).stem());
            }
            return 0;
        }

        /**@brief Checks whether any of the associated MCTP endpoints are enabled
         *
         * @return bool - True if any of the MCTP EIDs are enabled,
         *                False otherwise
         *
         */
        bool checkForEnabledMCTPEids() const noexcept;

        /**@brief Checks whether the resource has any associated MCTP endpoints enumerated
         *
         * @return bool - True if any of the MCTP EIDs are enumerated,
         *                False otherwise
         *
         */
        inline bool isDeviceEnumerated() const noexcept
        {
            return !mctpEidObjects.empty();
        }


    protected:

        std::unordered_map<std::string, std::string> mctpEidObjects;
        std::vector<sdbusplus::bus::match_t> deviceMatches;

        /**@brief Callback function for MCTP event listeners
         * Updates Health and State of the D-Bus object
         *
         * @return void
         *
         */
        inline void onMCTPDiscoveryMsg(sdbusplus::message::message& msg)
        {
            lg2::info("MCTP Event received from Object: {OBJECT}, Updating Health",
                    "OBJECT", msg.get_path());
            updateHealth();
        }

    private:

        std::string uuid;
        std::vector<sdbusplus::bus::match_t> mctpObjManagerMatch;

        /**@brief Fetches the list of currently active MCTP Services
         *
         * @return set<string> - Set containing the currently active MCTP services
         *
         */
        std::unordered_set<std::string> getMctpServices() const noexcept;

        /**@brief Fetches a mapping of MCTP service to the list of EIDs associated with the resource
         *
         * @return Map between service name and mctp object path
         *
         */
        std::unordered_map<std::string, std::string> getMCTPObjects();

        /**@brief Updates the Health and State of the D-Bus Object
         *
         * @return void
         *
         */
        virtual void updateHealth() = 0;

        /**@brief Start listening for events on MCTP objects 
         *
         * @return void
         *
         */
        void startWatchingMCTPObjects();

};

