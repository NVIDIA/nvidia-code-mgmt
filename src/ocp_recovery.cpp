
#include "config.h"

#include "ocp_recovery.hpp"

namespace nvidia
{
namespace software
{
namespace updater
{

std::string OCPRecovery::getVersion([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "";
}

std::string OCPRecovery::getManufacturer([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "OCPRecovery";
}

std::string OCPRecovery::getModel([
    [maybe_unused]] const std::string& inventoryPath) const
{
    return "Nvidia";
}

int OCPRecovery::processImage(std::filesystem::path& filePath)
{
    // Compute id
    std::string uniqueIdentifier = filePath.parent_path().parent_path().string();
    boost::replace_all(uniqueIdentifier, getImageUploadDir(), "");
    auto id = getIdProperty(uniqueIdentifier);
    if (id == "")
    {
        std::cerr << "\n Version ID not found ";
        return -1;
    }
    auto objPath = std::string{SOFTWARE_OBJPATH} + '/' + id;
    return initiateUpdateImage(objPath, filePath.parent_path().parent_path().string(), filePath.stem(), id,
                               uniqueIdentifier);
}

} // namespace updater
} // namespace software
} // namespace nvidia
