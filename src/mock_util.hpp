#pragma once

#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace phosphor::logging;

namespace nvidia
{
namespace mock
{
namespace common
{
inline json loadJSONFile([[maybe_unused]] const char* path)
{
    json data;
    return data;
}
/**
 * @brief Mock class for Utility
 *
 */
class Util
{
  protected:
    uint8_t b, d, m, c;

  public:
    virtual ~Util() = default;

    virtual bool getPresence() const
    {
        return true;
    }

    virtual std::string getSerialNumber() const
    {
        return "";
    }
    virtual std::string getPartNumber() const
    {
        return "";
    }
    virtual std::string getManufacturer() const
    {
        return "";
    }
    virtual std::string getModel() const
    {
        return "";
    }
    virtual std::string getVersion() const
    {
        return "";
    }
};
} // namespace common
} // namespace mock
} // namespace nvidia
