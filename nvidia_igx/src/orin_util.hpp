#include <unistd.h>
#include <iostream>
#include <string>

namespace nvidia::orin::common
{

inline std::string readVersionFile(const std::string& cmd)
{
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
    {
        throw std::runtime_error("popen() failed!");
    }
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }
    pclose(pipe);
    return output;
}

class Util
{
  public:
    virtual ~Util() = default;

    virtual std::string getVersion() const
    {
        std::string version = "";
        try
        {
            std::string cmd = "cat /etc/version";
            version = readVersionFile(cmd);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Failed to fetch version: " << e.what() << std::endl;
        }

        return version;
    }
};

} // namespace nvidia::orin::common
