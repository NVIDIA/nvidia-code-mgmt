#pragma once

#include "orin_util.hpp"

#include <filesystem>
#include <iostream>
#include <sdbusplus/bus/match.hpp>
#include <stdexcept>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Chassis/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

using namespace nvidia::orin::common;

namespace nvidia::orin::device
{

using OrinInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus,
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Chassis,
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset>;

using VersionObject = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::Version>;

using PropertyType = std::variant<std::string, bool>;

using Properties = std::map<std::string, PropertyType>;

class VersionInterface : public VersionObject
{
  public:
    /**
     * @brief Construct a new VersionInterfaceobject
     *
     * @param bus
     * @param path
     * @param fwversion
     */
    VersionInterface(sdbusplus::bus::bus& bus, const std::string& path,
                     std::string& fwversion) :
        VersionObject(bus, path.c_str(), action::emit_interface_added)
    {
        version(fwversion);
    }
};

/**
 * @class Orin
 */
class Orin : public OrinInherit, public Util
{
  public:
    Orin() = delete;
    Orin(const Orin&) = delete;
    Orin(Orin&&) = delete;
    Orin& operator=(const Orin&) = delete;
    Orin& operator=(Orin&&) = delete;
    ~Orin() = default;

    Orin(sdbusplus::bus::bus& bus, const std::string& objPath) :
        OrinInherit(bus, (objPath).c_str()),
    bus(bus),
    inventoryPath(objPath)
    {
        sdbusplus::xyz::openbmc_project::Inventory::server::Item::prettyName(
            "ORIN");

        registerSoftwareVersion(bus, objPath);
    }

    void registerSoftwareVersion(sdbusplus::bus::bus& bus,
                                 const std::string& ifPath)
    {
        std::string swpath = SW_INV_PATH;
        std::string fName = std::filesystem::path(ifPath).filename().string();
        swpath += "/" + fName;
        std::string orinVersion = getVersion();
        VersionObj =
            std::make_unique<VersionInterface>(bus, swpath, orinVersion);
    }

    const std::string& getInventoryPath() const
    {
        return inventoryPath;
    }

  private:
    /** @brief systemd bus member */
    sdbusplus::bus::bus& bus;
    std::string inventoryPath;
    std::unique_ptr<VersionInterface> VersionObj;
};

} // namespace nvidia::orin::device
