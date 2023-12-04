#include "glacier_recovery_interface.hpp"

namespace glacier_recovery_tool
{

namespace interface
{

std::vector<std::unique_ptr<CommandInterface>> commands;

class GetRecoveryStatus : public CommandInterface
{
  public:
    ~GetRecoveryStatus() = default;
    GetRecoveryStatus() = delete;
    GetRecoveryStatus(const GetRecoveryStatus&) = delete;
    GetRecoveryStatus(GetRecoveryStatus&&) = default;
    GetRecoveryStatus& operator=(const GetRecoveryStatus&) = delete;
    GetRecoveryStatus& operator=(GetRecoveryStatus&&) = default;

    using CommandInterface::CommandInterface;

    void exec() override
    {
        try
        {
            glacier_recovery_tool::GlacierRecoveryTool glacierRecoveryToolObj(
                busAddress, slaveAddress, verbose);
            nlohmann::json jsonResponse =
                glacierRecoveryToolObj.getRecoveryStatusJson();
            std::cout << jsonResponse.dump(4) << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error in GetRecoveryStatus: " << e.what() << "\n";
        }
    }
};

class GetFirmwareInfo : public CommandInterface
{
  public:
    ~GetFirmwareInfo() = default;
    GetFirmwareInfo() = delete;
    GetFirmwareInfo(const GetFirmwareInfo&) = delete;
    GetFirmwareInfo(GetFirmwareInfo&&) = default;
    GetFirmwareInfo& operator=(const GetFirmwareInfo&) = delete;
    GetFirmwareInfo& operator=(GetFirmwareInfo&&) = default;

    using CommandInterface::CommandInterface;

    void exec() override
    {
        try
        {
            glacier_recovery_tool::GlacierRecoveryTool glacierRecoveryToolObj(
                busAddress, slaveAddress, verbose);
            nlohmann::json jsonResponse =
                glacierRecoveryToolObj.getFirmwareInfoJson();
            std::cout << jsonResponse.dump(4) << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error in GetFirmwareInfo: " << e.what() << "\n";
        }
    }
};

class PerformGlacierRecovery : public CommandInterface
{
  private:
    std::string imagePath;

  public:
    ~PerformGlacierRecovery() = default;
    PerformGlacierRecovery() = delete;
    PerformGlacierRecovery(const PerformGlacierRecovery&) = delete;
    PerformGlacierRecovery(PerformGlacierRecovery&&) = default;
    PerformGlacierRecovery& operator=(const PerformGlacierRecovery&) = delete;
    PerformGlacierRecovery& operator=(PerformGlacierRecovery&&) = default;

    using CommandInterface::CommandInterface;

    explicit PerformGlacierRecovery(int busAddress, int slaveAddress,
                                    CLI::App* app) :
        CommandInterface(busAddress, slaveAddress, app)
    {
        app->add_option("-i,--image", imagePath,
                        "Image paths (e.g., -i /path/to/cms0 /path/to/cms1)");
    }

    void exec() override
    {
        try
        {
            glacier_recovery_tool::GlacierRecoveryTool glacierRecoveryToolObj(
                busAddress, slaveAddress, verbose);
            nlohmann::json jsonResponse =
                glacierRecoveryToolObj.performRecovery(imagePath);
            std::cout << jsonResponse.dump(4) << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error in PerformGlacierRecovery: " << e.what()
                      << "\n";
        }
    }
};

void registerCommand(CLI::App& app)
{
    int busAddress;
    int slaveAddress;

    auto getFirmwareInfoCmd =
        app.add_subcommand("GetFirmwareInfo", "Get the firmware information");
    commands.push_back(std::make_unique<GetFirmwareInfo>(
        busAddress, slaveAddress, getFirmwareInfoCmd));

    auto getRecoveryStatusCmd =
        app.add_subcommand("GetRecoveryStatus", "Get the recovery status");
    commands.push_back(std::make_unique<GetRecoveryStatus>(
        busAddress, slaveAddress, getRecoveryStatusCmd));

    auto performGlacierRecoveryCmd = app.add_subcommand(
        "PerformGlacierRecovery", "Perform Glacier recovery");
    commands.push_back(std::make_unique<PerformGlacierRecovery>(
        busAddress, slaveAddress, performGlacierRecoveryCmd));
}

} // namespace interface
} // namespace glacier_recovery_tool

int main(int argc, char** argv)
{
    try
    {
        CLI::App app{"Glacier Recovery Tool for OpenBMC"};
        app.require_subcommand(1)->ignore_case();

        glacier_recovery_tool::interface::registerCommand(app);

        CLI11_PARSE(app, argc, argv);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return -1;
    }
}
