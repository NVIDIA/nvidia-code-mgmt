#include "../recovery_tool/glacier_recovery_tool/glacier_recovery_interface.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

#include "gtest/gtest.h"

namespace fake_glacier_interface
{

struct Behavior
{
    bool throwInCtor = false;
    bool throwGetStatus = false;
    bool throwGetInfo = false;
    bool throwPerformRecovery = false;
    nlohmann::json statusJson = {{"Status", "OK"}};
    nlohmann::json firmwareInfoJson = {{"Build", "1234"}};
    nlohmann::json recoveryJson = {{"Result", "done"}};
};

inline Behavior behavior{};
inline int lastBusAddress = -1;
inline int lastSlaveAddress = -1;
inline bool lastVerbose = false;
inline std::string lastImagePath;

void reset()
{
    behavior = {};
    lastBusAddress = -1;
    lastSlaveAddress = -1;
    lastVerbose = false;
    lastImagePath.clear();
}

} // namespace fake_glacier_interface

namespace glacier_recovery_tool
{

class FakeGlacierRecoveryTool
{
  public:
    FakeGlacierRecoveryTool(int busAddress, int slaveAddress, bool verbose)
    {
        fake_glacier_interface::lastBusAddress = busAddress;
        fake_glacier_interface::lastSlaveAddress = slaveAddress;
        fake_glacier_interface::lastVerbose = verbose;

        if (fake_glacier_interface::behavior.throwInCtor)
        {
            throw std::runtime_error("fake constructor failure");
        }
    }

    nlohmann::json getRecoveryStatusJson()
    {
        if (fake_glacier_interface::behavior.throwGetStatus)
        {
            throw std::runtime_error("fake status failure");
        }
        return fake_glacier_interface::behavior.statusJson;
    }

    nlohmann::json getFirmwareInfoJson()
    {
        if (fake_glacier_interface::behavior.throwGetInfo)
        {
            throw std::runtime_error("fake info failure");
        }
        return fake_glacier_interface::behavior.firmwareInfoJson;
    }

    nlohmann::json performRecovery(const std::string& imagePath)
    {
        fake_glacier_interface::lastImagePath = imagePath;
        if (fake_glacier_interface::behavior.throwPerformRecovery)
        {
            throw std::runtime_error("fake perform failure");
        }
        return fake_glacier_interface::behavior.recoveryJson;
    }
};

} // namespace glacier_recovery_tool

#define GlacierRecoveryTool FakeGlacierRecoveryTool
#define main glacier_fake_cli_main
#include "../recovery_tool/glacier_recovery_tool/glacier_recovery_interface.cpp"
#undef main
#undef GlacierRecoveryTool

namespace
{

class GlacierInterfaceFakeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake_glacier_interface::reset();
        glacier_recovery_tool::interface::commands.clear();
    }

    void TearDown() override
    {
        fake_glacier_interface::reset();
        glacier_recovery_tool::interface::commands.clear();
    }
};

TEST_F(GlacierInterfaceFakeTest, CommandsExecuteAndPropagateArguments)
{
    testing::internal::CaptureStdout();
    const char* getStatusArgv[] = {
        "glacier", "GetRecoveryStatus", "-b", "3", "-s", "48", "-v"};
    EXPECT_EQ(glacier_fake_cli_main(7, const_cast<char**>(getStatusArgv)), 0);
    const auto statusOutput = testing::internal::GetCapturedStdout();
    EXPECT_NE(statusOutput.find("\"Status\": \"OK\""), std::string::npos);
    EXPECT_EQ(fake_glacier_interface::lastBusAddress, 3);
    EXPECT_EQ(fake_glacier_interface::lastSlaveAddress, 48);
    EXPECT_TRUE(fake_glacier_interface::lastVerbose);

    fake_glacier_interface::reset();
    testing::internal::CaptureStdout();
    const char* getInfoArgv[] = {"glacier", "GetFirmwareInfo", "-b", "5", "-s",
                                 "64"};
    EXPECT_EQ(glacier_fake_cli_main(6, const_cast<char**>(getInfoArgv)), 0);
    const auto infoOutput = testing::internal::GetCapturedStdout();
    EXPECT_NE(infoOutput.find("\"Build\": \"1234\""), std::string::npos);
    EXPECT_EQ(fake_glacier_interface::lastBusAddress, 5);
    EXPECT_EQ(fake_glacier_interface::lastSlaveAddress, 64);
    EXPECT_FALSE(fake_glacier_interface::lastVerbose);

    fake_glacier_interface::reset();
    testing::internal::CaptureStdout();
    const char* performArgv[] = {
        "glacier",    "PerformGlacierRecovery", "-b", "7", "-s", "80", "-i",
        "/tmp/fw.bin"};
    EXPECT_EQ(glacier_fake_cli_main(8, const_cast<char**>(performArgv)), 0);
    const auto performOutput = testing::internal::GetCapturedStdout();
    EXPECT_NE(performOutput.find("\"Result\": \"done\""), std::string::npos);
    EXPECT_EQ(fake_glacier_interface::lastImagePath, "/tmp/fw.bin");
}

TEST_F(GlacierInterfaceFakeTest, StatusCommandCatchesConstructorFailure)
{
    fake_glacier_interface::behavior.throwInCtor = true;

    testing::internal::CaptureStderr();
    const char* argv[] = {"glacier", "GetRecoveryStatus", "-b", "1", "-s",
                          "32"};
    EXPECT_EQ(glacier_fake_cli_main(6, const_cast<char**>(argv)), 0);
    const auto errorOutput = testing::internal::GetCapturedStderr();
    EXPECT_NE(errorOutput.find("Error in GetRecoveryStatus"),
              std::string::npos);
}

TEST_F(GlacierInterfaceFakeTest, FirmwareInfoCommandCatchesMethodFailure)
{
    fake_glacier_interface::behavior.throwGetInfo = true;

    testing::internal::CaptureStderr();
    const char* argv[] = {"glacier", "GetFirmwareInfo", "-b", "1", "-s", "32"};
    EXPECT_EQ(glacier_fake_cli_main(6, const_cast<char**>(argv)), 0);
    const auto errorOutput = testing::internal::GetCapturedStderr();
    EXPECT_NE(errorOutput.find("Error in GetFirmwareInfo"), std::string::npos);
}

TEST_F(GlacierInterfaceFakeTest, PerformRecoveryCommandCatchesMethodFailure)
{
    fake_glacier_interface::behavior.throwPerformRecovery = true;

    testing::internal::CaptureStderr();
    const char* argv[] = {
        "glacier", "PerformGlacierRecovery", "-b", "1", "-s", "32",
        "-i",      "/tmp/fail.bin"};
    EXPECT_EQ(glacier_fake_cli_main(8, const_cast<char**>(argv)), 0);
    const auto errorOutput = testing::internal::GetCapturedStderr();
    EXPECT_NE(errorOutput.find("Error in PerformGlacierRecovery"),
              std::string::npos);
    EXPECT_EQ(fake_glacier_interface::lastImagePath, "/tmp/fail.bin");
}

TEST_F(GlacierInterfaceFakeTest, AdditionalCommandExceptionBranchesAreCovered)
{
    fake_glacier_interface::behavior.throwGetStatus = true;

    testing::internal::CaptureStderr();
    const char* statusArgv[] = {"glacier", "GetRecoveryStatus", "-b", "2", "-s",
                                "33"};
    EXPECT_EQ(glacier_fake_cli_main(6, const_cast<char**>(statusArgv)), 0);
    const auto statusError = testing::internal::GetCapturedStderr();
    EXPECT_NE(statusError.find("Error in GetRecoveryStatus"),
              std::string::npos);

    fake_glacier_interface::reset();
    fake_glacier_interface::behavior.throwInCtor = true;

    testing::internal::CaptureStderr();
    const char* infoArgv[] = {"glacier", "GetFirmwareInfo", "-b", "3", "-s",
                              "34"};
    EXPECT_EQ(glacier_fake_cli_main(6, const_cast<char**>(infoArgv)), 0);
    const auto infoError = testing::internal::GetCapturedStderr();
    EXPECT_NE(infoError.find("Error in GetFirmwareInfo"), std::string::npos);

    fake_glacier_interface::reset();
    fake_glacier_interface::behavior.throwInCtor = true;

    testing::internal::CaptureStderr();
    const char* performArgv[] = {
        "glacier", "PerformGlacierRecovery", "-b", "4", "-s", "35",
        "-i",      "/tmp/ctor.bin"};
    EXPECT_EQ(glacier_fake_cli_main(8, const_cast<char**>(performArgv)), 0);
    const auto performError = testing::internal::GetCapturedStderr();
    EXPECT_NE(performError.find("Error in PerformGlacierRecovery"),
              std::string::npos);
}

TEST_F(GlacierInterfaceFakeTest, MainReturnsErrorOnParseFailure)
{
    testing::internal::CaptureStderr();
    const char* argv[] = {"glacier"};
    EXPECT_NE(glacier_fake_cli_main(1, const_cast<char**>(argv)), 0);
    const auto errorOutput = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(errorOutput.empty());
}

TEST_F(GlacierInterfaceFakeTest, MainReturnsErrorOnInvalidNumericOption)
{
    testing::internal::CaptureStderr();
    const char* argv[] = {"glacier", "GetFirmwareInfo", "-b", "bad", "-s",
                          "32"};
    EXPECT_NE(glacier_fake_cli_main(6, const_cast<char**>(argv)), 0);
    const auto errorOutput = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(errorOutput.empty());
}

TEST_F(GlacierInterfaceFakeTest, MainReturnsErrorOnUnexpectedOption)
{
    testing::internal::CaptureStderr();
    const char* argv[] = {
        "glacier", "PerformGlacierRecovery", "-b", "1", "-s", "32", "--bogus"};
    EXPECT_NE(glacier_fake_cli_main(7, const_cast<char**>(argv)), 0);
    const auto errorOutput = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(errorOutput.empty());
}

} // namespace
