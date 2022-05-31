
#include "base_item_updater.hpp"
#include "watch.hpp"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>

#include <algorithm>
#include <cstring>
#include <experimental/any>
#include <filesystem>
#include <string>

namespace nvidia
{
namespace software
{
namespace updater
{

using namespace phosphor::logging;

// Due to a libstdc++ bug, we got compile error using std::any with gmock.
// A temporary workaround is to use std::experimental::any.
// See details in https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90415
using std::experimental::any;
using std::experimental::any_cast;
using PropertyType = std::variant<std::string, bool>;

std::vector<std::string> DBUSUtils::getinventoryPath(const std::string& iface)
{
    std::vector<std::string> paths;
    auto method = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                      MAPPER_INTERFACE, "GetSubTreePaths");
    method.append(INVENTORY_PATH_BASE);
    method.append(0); // Depth 0 to search all
    method.append(std::vector<std::string>({iface.c_str()}));
    auto reply = bus.call(method);

    reply.read(paths);
    return paths;
}

bool DBUSUtils::isAssociated(const std::string& inventoryPath,
                             const AssociationList& assocs)
{
    return std::find_if(assocs.begin(), assocs.end(),
                        [&inventoryPath](const auto& assoc) {
                            return inventoryPath == std::get<2>(assoc);
                        }) != assocs.end();
}
std::string DBUSUtils::createVersionID(const std::string& updaterName,
                                       const std::string& version)
{
    if (version.empty() || updaterName.empty())
    {
        log<level::ERR>("Error version is empty");
        return {};
    }
    std::string chars = ".";
    std::string stripped = updaterName + "_" + version;
    for (char c : chars)
    {
        stripped.erase(std::remove(stripped.begin(), stripped.end(), c),
                       stripped.end());
    }
    // Version - 1.1
    // updateName - PSU
    //  stripped - PSU_11
    return stripped;

    // unsigned char digest[SHA512_DIGEST_LENGTH];
    // SHA512_CTX ctx;
    // SHA512_Init(&ctx);
    // SHA512_Update(&ctx, version.c_str(), strlen(version.c_str()));
    // SHA512_Final(digest, &ctx);
    // char mdString[SHA512_DIGEST_LENGTH * 2 + 1];
    // for (int i = 0; i < SHA512_DIGEST_LENGTH; i++)
    // {
    //     snprintf(&mdString[i * 2], 3, "%02x", (unsigned int)digest[i]);
    // }

    // // Only need 8 hex digits.
    // std::string hexId = std::string(mdString);
    // return (hexId.substr(0, 8));
}

any DBUSUtils::getPropertyImpl(const char* service, const char* path,
                               const char* interface, const char* propertyName)
                               const
{
    auto method = bus.new_method_call(service, path,
                                      "org.freedesktop.DBus.Properties", "Get");
    method.append(interface, propertyName);
    try
    {
        PropertyType value{};
        auto reply = bus.call(method);
        reply.read(value);
        return any(value);
    }
    catch (const sdbusplus::exception::SdBusError& ex)
    {
        log<level::ERR>("GetProperty call failed", entry("PATH=%s", path),
                        entry("INTERFACE=%s", interface),
                        entry("PROPERTY=%s", propertyName));
        throw std::runtime_error("GetProperty call failed");
    }
}

std::vector<std::string> DBUSUtils::getServices(const char* path,
                                                const char* interface)
{
    auto mapper = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                      MAPPER_INTERFACE, "GetObject");

    mapper.append(path, std::vector<std::string>({interface}));
    const int maxRetry=10;
    const int delay1sec=1000000;
    for (int retry = 0; retry < maxRetry; retry++)
    {
        try
        {
            auto mapperResponseMsg = bus.call(mapper);

            std::vector<std::pair<std::string, std::vector<std::string>>>
                mapperResponse;
            mapperResponseMsg.read(mapperResponse);
            if (mapperResponse.empty())
            {
                log<level::ERR>("Error reading mapper response");
                throw std::runtime_error("Error reading mapper response");
            }
            std::vector<std::string> ret;
            for (const auto& i : mapperResponse)
            {
                ret.emplace_back(i.first);
            }
            return ret;
        }
        catch (const sdbusplus::exception::SdBusError& ex)
        {
            log<level::ERR>("GetObject call failed", entry("PATH=%s", path),
                        entry("INTERFACE=%s", interface));
		    std::cerr << ex.what() << std::endl;
		
            if (retry == 9){
                throw std::runtime_error("Retry attemped to 9,GetObject call failed");
            }
        }
		usleep(delay1sec);
	}
    return {};
}

std::string DBUSUtils::getService(const char* path, const char* interface)
{
    auto services = getServices(path, interface);
    if (services.empty())
    {
        return {};
    }
    return services[0];
}

std::vector<std::string> DBUSUtils::getSoftwareObjects()
{
    std::vector<std::string> paths;
    auto method = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                      MAPPER_INTERFACE, "GetSubTreePaths");
    method.append(SOFTWARE_OBJPATH);
    method.append(0); // Depth 0 to search all
    method.append(std::vector<std::string>({VERSION_IFACE}));
    auto reply = bus.call(method);
    reply.read(paths);
    return paths;
}

bool DBUSUtils::findSoftwareObject(std::string& objPath)
{
    auto allSoftwareObjs = getSoftwareObjects();
    auto it =
        std::find(allSoftwareObjs.begin(), allSoftwareObjs.end(), objPath);
    if (it == allSoftwareObjs.end())
    {
        // Not found
        return false;
    }
    return true;
}
} // namespace updater
} // namespace software
} // namespace nvidia
