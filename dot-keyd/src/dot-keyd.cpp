/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <curl/curl.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/i2c-dev.h>
#include <openssl/sha.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus/match.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr int kDefaultInstallTimeoutMs = 30000;
constexpr int kDefaultDotCakInitTimeoutMs = 30000;
constexpr int kHmcRequestTimeoutMs = 3000;
constexpr size_t kDefaultCakMaxBytes = 16 * 1024;
constexpr const char* kDefaultPayloadFilename = "cak_payload.json";
constexpr int kDefaultCakInstallRetries = 30;
constexpr const char* kDefaultKeyStorePath = "/var/lib/dot-keyd";

struct ExitCode
{
    static constexpr int kSuccess = 0;
    static constexpr int kInvalidArgs = 1;
    static constexpr int kInvalidCak = 2;
    static constexpr int kStorageError = 3;
    static constexpr int kProvisioningError = 4;
    static constexpr int kL1ResetError = 5;
};

struct HmcConfig
{
    std::string host;
    std::optional<std::string> username;
    std::optional<std::string> password;
    bool verifyTls{false};
    std::string dotCakInitPath{"/redfish/v1/Systems/HGX_Baseboard_0"};
    std::vector<std::string> installPaths{
        "/redfish/v1/Chassis/HGX_CPU_0/TrustedComponents/IRoT_CPU_0/Oem/Nvidia/"
        "DOT/Actions/NvidiaDOT.Install",
        "/redfish/v1/Chassis/HGX_CPU_1/TrustedComponents/IRoT_CPU_1/Oem/Nvidia/"
        "DOT/Actions/NvidiaDOT.Install",
    };
    std::vector<std::string> statusPaths{
        "/redfish/v1/Chassis/HGX_CPU_0/TrustedComponents/IRoT_CPU_0/Oem/Nvidia/"
        "DOT",
        "/redfish/v1/Chassis/HGX_CPU_1/TrustedComponents/IRoT_CPU_1/Oem/Nvidia/"
        "DOT",
    };
};

struct HmclessExecConfig
{
    std::string path;
    std::vector<std::string> args;
};

struct L1ResetConfig
{
    int i2cBus{70};
    std::string i2cAddr{"0x38"};
};

struct TimeoutsConfig
{
    int installMs{kDefaultInstallTimeoutMs};
    int dotCakInitMs{kDefaultDotCakInitTimeoutMs};
};

struct Config
{
    fs::path keyStorePath;
    std::optional<HmcConfig> hmc;
    std::optional<HmclessExecConfig> hmclessExec;
    L1ResetConfig l1Reset;
    TimeoutsConfig timeouts;
    size_t maxCakBytes{kDefaultCakMaxBytes};
    bool allowCakReadout{true};
    std::optional<int> minimumSecurityVersion;
    int cakInstallRetries{kDefaultCakInstallRetries};
};

struct RunResult
{
    int code{0};
    std::string stdoutStr;
    std::string stderrStr;
    bool timedOut{false};
};

struct Args
{
    std::string configPath;
    bool install{false};
    bool deleteCak{false};
    std::optional<std::string> cakPath;
    std::optional<std::string> keyStorePath;
    std::optional<std::string> hmclessExec;
    std::vector<std::string> hmclessArgs;
    std::optional<int> i2cBus;
    std::optional<std::string> i2cAddr;
};

/**
 * @brief Ensure a directory exists and apply secure permissions.
 *
 * @param path Directory path to create or validate.
 */
static void ensureDir(const fs::path& path)
{
    fs::create_directories(path);
    ::chmod(path.c_str(), 0700);
}

/**
 * @brief Atomically write file contents using a temporary file and rename.
 *
 * @param path Destination file path.
 * @param data Serialized content to persist.
 */
static void atomicWrite(const fs::path& path, const std::string& data)
{
    ensureDir(path.parent_path());
    std::string tmpl = (path.parent_path() / "tmp.XXXXXX").string();
    std::vector<char> tmp(tmpl.begin(), tmpl.end());
    tmp.push_back('\0');
    int fd = ::mkstemp(tmp.data());
    if (fd < 0)
    {
        throw std::runtime_error("mkstemp failed");
    }
    if (::write(fd, data.data(), data.size()) < 0)
    {
        ::close(fd);
        throw std::runtime_error("write failed");
    }
    ::fsync(fd);
    ::fchmod(fd, 0600);
    ::close(fd);
    fs::path tmpPath(tmp.data());
    fs::rename(tmpPath, path);
}

/**
 * @brief Read an entire file into a string buffer.
 *
 * @param path File path to read.
 * @return std::string Complete file contents.
 */
static std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("failed to open file");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/**
 * @brief Build the CAK payload file path under the key store.
 *
 * @param keyStorePath Key store root path.
 * @return fs::path Full payload file path.
 */
static fs::path payloadPath(const fs::path& keyStorePath)
{
    return keyStorePath / kDefaultPayloadFilename;
}

/**
 * @brief Check whether input includes PEM public-key markers.
 *
 * @param data Key material to validate.
 * @return true PEM markers are present.
 * @return false PEM markers are not present.
 */
static bool isValidPem(const std::string& data)
{
    return data.find("BEGIN PUBLIC KEY") != std::string::npos &&
           data.find("END PUBLIC KEY") != std::string::npos;
}

/**
 * @brief Validate CAK bytes for non-empty size and PEM shape.
 *
 * @param data CAK bytes to validate.
 * @param maxBytes Maximum allowed CAK size in bytes.
 */
static void validateCakBytes(const std::string& data, size_t maxBytes)
{
    if (data.empty())
    {
        throw std::runtime_error("CAK is empty");
    }
    if (data.size() > maxBytes)
    {
        throw std::runtime_error("CAK exceeds maximum size");
    }
    if (!isValidPem(data))
    {
        throw std::runtime_error("CAK is not a valid PEM public key");
    }
}

/**
 * @brief Compute SHA-256 fingerprint text for key material.
 *
 * @param data Raw key bytes.
 * @return std::string Hex-encoded SHA-256 digest.
 */
[[maybe_unused]] static std::string computeFingerprint(const std::string& data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           hash);
    std::ostringstream out;
    for (auto byte : hash)
    {
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(byte);
    }
    return out.str();
}

/**
 * @brief Normalize PEM input by removing markers and whitespace.
 *
 * @param data Input PEM key string.
 * @return std::string Normalized key body.
 */
[[maybe_unused]] static std::string normalizePemKey(const std::string& data)
{
    std::istringstream stream(data);
    std::string line;
    std::string out;
    while (std::getline(stream, line))
    {
        if (line.find("BEGIN") != std::string::npos ||
            line.find("END") != std::string::npos)
        {
            continue;
        }
        for (char ch : line)
        {
            if (!std::isspace(static_cast<unsigned char>(ch)))
            {
                out.push_back(ch);
            }
        }
    }
    if (out.empty())
    {
        for (char ch : data)
        {
            if (!std::isspace(static_cast<unsigned char>(ch)))
            {
                out.push_back(ch);
            }
        }
    }
    return out;
}

/**
 * @brief Normalize PEM formatting to a canonical newline layout.
 *
 * @param key PEM key string to normalize in place.
 */
static void normalizePemKeyFormat(std::string& key)
{
    if (key.empty())
    {
        return;
    }

    bool hasEscapedNewlines = (key.find("\\n") != std::string::npos);

    std::string normalized;
    normalized.reserve(key.size());
    for (size_t i = 0; i < key.size(); ++i)
    {
        if (key[i] == '\\' && i + 1 < key.size() && key[i + 1] == 'n')
        {
            normalized.push_back('\n');
            ++i;
        }
        else
        {
            normalized.push_back(key[i]);
        }
    }
    key = std::move(normalized);

    if (hasEscapedNewlines)
    {
        if (key.find("-----BEGIN PUBLIC KEY-----") != std::string::npos &&
            key.find("-----END PUBLIC KEY-----") != std::string::npos)
        {
            if (!key.empty() && key.back() != '\n')
            {
                key.push_back('\n');
            }
            return;
        }
    }

    const std::string beginMarker = "-----BEGIN PUBLIC KEY-----";
    const std::string endMarker = "-----END PUBLIC KEY-----";

    size_t beginPos = key.find(beginMarker);
    size_t endPos = key.find(endMarker);

    if (beginPos == std::string::npos && endPos == std::string::npos)
    {
        return;
    }

    bool hasNewlineAfterBegin = (beginPos != std::string::npos) &&
                                (beginPos + beginMarker.size() < key.size()) &&
                                (key[beginPos + beginMarker.size()] == '\n');
    bool hasNewlineBeforeEnd = (endPos != std::string::npos) && (endPos > 0) &&
                               (key[endPos - 1] == '\n');
    bool hasNewlineAfterEnd = (endPos != std::string::npos) &&
                              (endPos + endMarker.size() < key.size()) &&
                              (key[endPos + endMarker.size()] == '\n');

    if (hasNewlineAfterBegin && hasNewlineBeforeEnd && hasNewlineAfterEnd)
    {
        return;
    }

    std::string base64Data;
    if (beginPos != std::string::npos && endPos != std::string::npos &&
        endPos > beginPos)
    {
        size_t contentStart = beginPos + beginMarker.size();
        while (contentStart < endPos &&
               std::isspace(static_cast<unsigned char>(key[contentStart])))
        {
            ++contentStart;
        }

        if (contentStart < endPos)
        {
            size_t contentEnd = endPos;
            while (
                contentEnd > contentStart &&
                std::isspace(static_cast<unsigned char>(key[contentEnd - 1])))
            {
                --contentEnd;
            }

            if (contentEnd > contentStart)
            {
                base64Data =
                    key.substr(contentStart, contentEnd - contentStart);
            }
        }
    }
    else
    {
        return;
    }

    std::string cleanBase64;
    cleanBase64.reserve(base64Data.size());
    for (char ch : base64Data)
    {
        if (!std::isspace(static_cast<unsigned char>(ch)))
        {
            cleanBase64.push_back(ch);
        }
    }

    if (cleanBase64.empty())
    {
        return;
    }

    key = beginMarker + "\n";

    for (size_t i = 0; i < cleanBase64.size(); ++i)
    {
        key.push_back(cleanBase64[i]);
        if ((i + 1) % 64 == 0)
        {
            key.push_back('\n');
        }
    }

    if (!key.empty() && key.back() != '\n')
    {
        key.push_back('\n');
    }

    key += endMarker + "\n";
}

/**
 * @brief Store a minimal legacy payload using only the CAK key.
 *
 * @param config Active runtime configuration.
 * @param cakBytes CAK PEM bytes.
 */
static void storeCak(const Config& config, const std::string& cakBytes)
{
    validateCakBytes(cakBytes, config.maxCakBytes);

    json payload = {
        {"CAKKey", {{"AuthenticationScheme", "Ecdsa"}, {"ECDSAKey", cakBytes}}},
        {"LockDisable", true},
        {"VendorMinimumSecurityVersion",
         config.minimumSecurityVersion.value_or(0)},
        {"OwnerMinimumSecurityVersion",
         config.minimumSecurityVersion.value_or(0)}};

    atomicWrite(payloadPath(config.keyStorePath), payload.dump());
}

/**
 * @brief Remove persisted CAK payload and legacy migration files.
 *
 * @param config Active runtime configuration.
 */
static void deleteCak(const Config& config)
{
    fs::path payloadFile = payloadPath(config.keyStorePath);
    if (fs::exists(payloadFile))
    {
        fs::remove(payloadFile);
    }
    fs::path legacyCakFile = config.keyStorePath / "cak.pem";
    if (fs::exists(legacyCakFile))
    {
        fs::remove(legacyCakFile);
    }
    fs::path legacyMetaFile = config.keyStorePath / "cak_metadata.json";
    if (fs::exists(legacyMetaFile))
    {
        fs::remove(legacyMetaFile);
    }
}

/**
 * @brief Execute a process and collect output with timeout enforcement.
 *
 * @param command Command vector where index 0 is executable.
 * @param timeoutMs Timeout in milliseconds.
 * @return RunResult Exit code, stdout/stderr, and timeout status.
 */
static RunResult runCommandWithTimeout(const std::vector<std::string>& command,
                                       int timeoutMs)
{
    int stdoutPipe[2]{-1, -1};
    int stderrPipe[2]{-1, -1};
    if (::pipe(stdoutPipe) < 0 || ::pipe(stderrPipe) < 0)
    {
        throw std::runtime_error("pipe failed");
    }

    pid_t pid = ::fork();
    if (pid < 0)
    {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0)
    {
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(stderrPipe[1], STDERR_FILENO);
        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& arg : command)
        {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);

    RunResult result;
    auto start = std::chrono::steady_clock::now();
    int status = 0;
    while (true)
    {
        pid_t finished = ::waitpid(pid, &status, WNOHANG);
        if (finished == pid)
        {
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                .count() > timeoutMs)
        {
            result.timedOut = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto readPipe = [](int fd) -> std::string {
        std::string out;
        char buf[4096];
        ssize_t n = 0;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        {
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    };
    result.stdoutStr = readPipe(stdoutPipe[0]);
    result.stderrStr = readPipe(stderrPipe[0]);
    ::close(stdoutPipe[0]);
    ::close(stderrPipe[0]);

    if (result.timedOut)
    {
        result.code = 124;
    }
    else if (WIFEXITED(status))
    {
        result.code = WEXITSTATUS(status);
    }
    else
    {
        result.code = 1;
    }
    return result;
}

/**
 * @brief libcurl write callback to append response bytes.
 *
 * @param contents Byte buffer from libcurl.
 * @param size Element size.
 * @param nmemb Element count.
 * @param userp Output std::string pointer.
 * @return size_t Number of bytes consumed.
 */
static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb,
                                void* userp)
{
    size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<const char*>(contents), total);
    return total;
}

/**
 * @brief Install CAK using configured hmcless helper executable.
 *
 * @param config Active runtime configuration.
 * @param cakFile Path to temporary CAK file consumed by helper.
 */
static void installCakHmcless(const Config& config, const fs::path& cakFile)
{
    if (!config.hmclessExec)
    {
        throw std::runtime_error("hmclessExec is not configured");
    }
    std::vector<std::string> command;
    command.push_back(config.hmclessExec->path);
    for (const auto& arg : config.hmclessExec->args)
    {
        command.push_back(arg);
    }
    command.push_back(cakFile.string());
    RunResult result =
        runCommandWithTimeout(command, config.timeouts.installMs);
    if (result.code != 0)
    {
        throw std::runtime_error("hmcless exec failed: " + result.stderrStr);
    }
}

/**
 * @brief Install CAK payload to all configured HMC Redfish endpoints.
 *
 * @param config Active runtime configuration.
 * @param jsonPayload Serialized JSON payload to POST.
 */
[[maybe_unused]] static void installCakHmc(const Config& config,
                                           const std::string& jsonPayload)
{
    for (const auto& installPath : config.hmc->installPaths)
    {
        std::string url = "http://" + config.hmc->host + installPath;
        lg2::info("Sending CAK installation request to Redfish endpoint: {URL}",
                  "URL", url);

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            throw std::runtime_error("curl init failed for " + url);
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config.timeouts.installMs);
        if (config.hmc->username && config.hmc->password)
        {
            std::string auth =
                *config.hmc->username + ":" + *config.hmc->password;
            curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
        }
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            throw std::runtime_error(
                "Redfish CAK installation request failed for " + url +
                ": curl error " + std::to_string(res));
        }
        if (status >= 300)
        {
            throw std::runtime_error("Redfish CAK installation failed for " +
                                     url + " with HTTP status " +
                                     std::to_string(status));
        }
    }
}

/**
 * @brief Fetch JSON content from an HMC Redfish path.
 *
 * @param hmc HMC connectivity configuration.
 * @param path Redfish path to query.
 * @param timeoutMs Request timeout in milliseconds.
 * @return std::string Raw JSON response body.
 */
static std::string fetchHmcJson(const HmcConfig& hmc, const std::string& path,
                                int timeoutMs)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        throw std::runtime_error("curl init failed");
    }

    std::string url = "http://" + hmc.host + path;
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (hmc.username && hmc.password)
    {
        std::string auth = *hmc.username + ":" + *hmc.password;
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        throw std::runtime_error("HMC request failed");
    }
    if (status >= 300)
    {
        throw std::runtime_error("HMC request failed with status " +
                                 std::to_string(status));
    }
    return response;
}

/**
 * @brief Get DOTCAKInitialization state from HMC when available.
 *
 * @param config Active runtime configuration.
 * @return std::string State string or empty on unavailable/error.
 */
static std::string getDotCakInitializationState(const Config& config)
{
    if (!config.hmc)
    {
        return "";
    }

    try
    {
        std::string response = fetchHmcJson(
            *config.hmc, config.hmc->dotCakInitPath, kHmcRequestTimeoutMs);
        json payload = json::parse(response);
        return payload["Oem"]["Nvidia"].value("DOTCAKInitialization", "");
    }
    catch (const std::exception&)
    {
        return "";
    }
}

/**
 * @brief Wait for expected DOT-related state conditions or timeout.
 *
 * @param config Active runtime configuration.
 * @param expectedDotState Expected DOTState value.
 */
static void ensureDotState(const Config& config,
                           const std::string& expectedDotState)
{
    if (!config.hmc)
    {
        throw std::runtime_error("hmc is not configured");
    }

    bool isBeforeInstall = (expectedDotState == "Uninitialized");
    bool isAfterInstall = (expectedDotState == "Volatile");
    std::string expectedCakInit = isBeforeInstall ? "Waiting" : "Complete";

    auto startTime = std::chrono::steady_clock::now();
    auto deadline =
        startTime + std::chrono::milliseconds(config.timeouts.installMs);
    std::vector<std::string> dotStates;

    while (true)
    {
        bool cakInitMatch = false;
        bool cakInitFetched = false;
        std::string cakInitState;
        try
        {
            cakInitState = getDotCakInitializationState(config);
            cakInitFetched = !cakInitState.empty();
            if (cakInitFetched)
            {
                lg2::info("Checking DOTCAKInitialization: current={STATE}, "
                          "expected={EXPECTED}",
                          "STATE", cakInitState, "EXPECTED", expectedCakInit);
            }
            if (cakInitFetched && cakInitState == expectedCakInit)
            {
                cakInitMatch = true;
                lg2::info(
                    "DOTCAKInitialization state matches expected: {STATE}",
                    "STATE", cakInitState);
            }
        }
        catch (const std::exception& ex)
        {
            cakInitFetched = false;
            lg2::debug("Failed to fetch DOTCAKInitialization: {ERROR}", "ERROR",
                       ex.what());
        }

        if (cakInitMatch)
        {
            return;
        }

        if (isBeforeInstall && cakInitFetched && cakInitState == "Complete")
        {
            return;
        }

        if (isBeforeInstall && cakInitFetched)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(
                    "DOTCAKInitialization not " + expectedCakInit +
                    " (current: " + cakInitState + ") before timeout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        bool allDotStateMatch = true;
        dotStates.clear();
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            throw std::runtime_error("curl init failed");
        }

        bool allRequestsSucceeded = true;
        for (const auto& statusPath : config.hmc->statusPaths)
        {
            std::string url = "http://" + config.hmc->host + statusPath;
            std::string response;

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kHmcRequestTimeoutMs);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            if (config.hmc->username && config.hmc->password)
            {
                std::string auth =
                    *config.hmc->username + ":" + *config.hmc->password;
                curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
            }

            CURLcode res = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

            if (res != CURLE_OK)
            {
                allRequestsSucceeded = false;
                allDotStateMatch = false;
                dotStates.push_back("");
                continue;
            }
            if (status >= 300)
            {
                allRequestsSucceeded = false;
                allDotStateMatch = false;
                dotStates.push_back("");
                continue;
            }

            json payload = json::parse(response);
            std::string state = payload.value("DOTState", "");
            dotStates.push_back(state);
            if (state != expectedDotState)
            {
                allDotStateMatch = false;
            }
        }
        curl_easy_cleanup(curl);

        if (!allRequestsSucceeded)
        {
            allDotStateMatch = false;
        }

        if (isAfterInstall)
        {
            std::string statesStr;
            for (size_t i = 0; i < dotStates.size(); ++i)
            {
                if (i > 0)
                    statesStr += ", ";
                statesStr += "CPU" + std::to_string(i) + "=" +
                             (dotStates[i].empty() ? "<empty>" : dotStates[i]);
            }
            lg2::info("Checking DOTState after installation: {STATES} "
                      "(expected={EXPECTED}), DOTCAKInitialization={CAKINIT}",
                      "STATES", statesStr, "EXPECTED", expectedDotState,
                      "CAKINIT",
                      cakInitFetched ? cakInitState : "<not available>");
        }

        bool success = false;
        if (isAfterInstall)
        {
            success = allDotStateMatch || cakInitMatch;
        }
        else
        {
            success = allDotStateMatch;
        }

        if (success)
        {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - startTime)
                           .count();

        if (isAfterInstall && elapsed > 0 && elapsed % 1000 < 200)
        {
            std::string statesStr;
            for (size_t i = 0; i < dotStates.size(); ++i)
            {
                if (i > 0)
                    statesStr += ", ";
                statesStr += "CPU" + std::to_string(i) + "=" +
                             (dotStates[i].empty() ? "<empty>" : dotStates[i]);
            }
            lg2::info("Still waiting for verification ({ELAPSED}ms elapsed): "
                      "DOTState={STATES}, DOTCAKInitialization={CAKINIT}",
                      "ELAPSED", elapsed, "STATES", statesStr, "CAKINIT",
                      cakInitFetched ? cakInitState : "<not available>");
        }

        if (now >= deadline)
        {
            std::string errorMsg;
            std::string statesStr;
            for (size_t i = 0; i < dotStates.size(); ++i)
            {
                if (i > 0)
                    statesStr += ", ";
                statesStr += "CPU" + std::to_string(i) + "=" +
                             (dotStates[i].empty() ? "<empty>" : dotStates[i]);
            }

            if (isBeforeInstall)
            {
                if (!cakInitFetched)
                {
                    errorMsg =
                        "DOTCAKInitialization not available and DOTState not " +
                        expectedDotState +
                        " for both CPUs before timeout. Final DOTState: " +
                        statesStr;
                }
                else
                {
                    errorMsg =
                        "DOTCAKInitialization not " + expectedCakInit +
                        " (current: " + cakInitState + ") and DOTState not " +
                        expectedDotState +
                        " for both CPUs before timeout. Final DOTState: " +
                        statesStr;
                }
            }
            else if (isAfterInstall)
            {
                errorMsg = "Neither DOTState " + expectedDotState +
                           " for both CPUs (current: " + statesStr +
                           ") nor DOTCAKInitialization " + expectedCakInit +
                           " (current: " +
                           (cakInitFetched ? cakInitState : "<not available>") +
                           ") before timeout";
            }
            else
            {
                errorMsg = "DOTState not " + expectedDotState +
                           " for both CPUs before timeout. Final DOTState: " +
                           statesStr;
            }
            throw std::runtime_error(errorMsg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

/**
 * @brief Parse and validate an I2C 7-bit address string.
 *
 * @param addrStr I2C address text in decimal or hex form.
 * @return int Parsed I2C address value.
 */
static int parseI2cAddr(const std::string& addrStr)
{
    size_t parsed = 0;
    int addr = std::stoi(addrStr, &parsed, 0);
    if (parsed != addrStr.size())
    {
        throw std::runtime_error("Invalid I2C address: " + addrStr);
    }
    if (addr < 0 || addr > 0x7f)
    {
        throw std::runtime_error("I2C address out of range: " + addrStr);
    }
    return addr;
}

/**
 * @brief Write raw bytes to a target I2C bus/address.
 *
 * @param bus I2C bus number.
 * @param addr 7-bit I2C address.
 * @param bytes Payload bytes to transmit.
 */
static void writeI2cBytes(int bus, int addr, const std::vector<uint8_t>& bytes)
{
    std::string devPath = "/dev/i2c-" + std::to_string(bus);
    int fd = ::open(devPath.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        throw std::runtime_error("Failed to open " + devPath + ": " +
                                 std::strerror(errno));
    }
    if (::ioctl(fd, I2C_SLAVE, addr) < 0)
    {
        int savedErrno = errno;
        ::close(fd);
        throw std::runtime_error("Failed to set I2C address " +
                                 std::to_string(addr) + " on " + devPath +
                                 ": " + std::strerror(savedErrno));
    }
    ssize_t written = ::write(fd, bytes.data(), bytes.size());
    int savedErrno = errno;
    ::close(fd);
    if (written < 0)
    {
        throw std::runtime_error("I2C write failed on " + devPath + ": " +
                                 std::strerror(savedErrno));
    }
    if (static_cast<size_t>(written) != bytes.size())
    {
        throw std::runtime_error("I2C write short write on " + devPath +
                                 " (wrote " + std::to_string(written) + " of " +
                                 std::to_string(bytes.size()) + " bytes)");
    }
}

/**
 * @brief Execute the two-step L1 reset sequence over I2C.
 *
 * @param config Active runtime configuration.
 */
static void l1Reset(const Config& config)
{
    lg2::info("Starting L1 reset via I2C (bus {BUS}, addr {ADDR})", "BUS",
              config.l1Reset.i2cBus, "ADDR", config.l1Reset.i2cAddr);
    const int addr = parseI2cAddr(config.l1Reset.i2cAddr);
    std::vector<std::vector<uint8_t>> payloads = {
        {0xF0, 0x04, 0x00, 0x40, 0x00, 0x00},
        {0xF2, 0x04, 0x01, 0x00, 0x00, 0x00},
    };

    for (size_t index = 0; index < payloads.size(); ++index)
    {
        const auto& payload = payloads[index];
        bool success = false;
        std::string lastError;
        lg2::info("L1 reset command {INDEX}/{TOTAL}", "INDEX", index + 1,
                  "TOTAL", payloads.size());
        for (int attempt = 1; attempt <= 5; ++attempt)
        {
            try
            {
                writeI2cBytes(config.l1Reset.i2cBus, addr, payload);
                success = true;
                lg2::info(
                    "L1 reset command {INDEX} succeeded on attempt {ATTEMPT}",
                    "INDEX", index + 1, "ATTEMPT", attempt);
                break;
            }
            catch (const std::exception& ex)
            {
                lastError = ex.what();
            }
            lg2::warning(
                "L1 reset command {INDEX} attempt {ATTEMPT}/5 failed: {ERROR}",
                "INDEX", index + 1, "ATTEMPT", attempt, "ERROR", lastError);
            if (attempt < 5)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        if (!success)
        {
            throw std::runtime_error("L1 reset command " +
                                     std::to_string(index + 1) +
                                     " failed after 5 attempts: " + lastError);
        }
        if (index + 1 < payloads.size())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    lg2::info("L1 reset completed successfully");
}

/**
 * @brief Validate DOT payload schema and required fields.
 *
 * @param payload JSON payload to validate.
 */
static void validateDotPayload(const json& payload)
{
    const std::set<std::string> allowedFields = {
        "CAKKey", "LAKKey", "LockDisable", "VendorMinimumSecurityVersion",
        "OwnerMinimumSecurityVersion"};
    for (const auto& [key, value] : payload.items())
    {
        if (allowedFields.find(key) == allowedFields.end())
        {
            throw std::runtime_error("Disallowed field in JSON payload: " +
                                     key);
        }
    }

    if (!payload.contains("CAKKey"))
    {
        throw std::runtime_error("CAKKey is required in JSON payload");
    }

    const auto& cakKey = payload["CAKKey"];
    if (!cakKey.is_object())
    {
        throw std::runtime_error("CAKKey must be an object");
    }

    if (!cakKey.contains("AuthenticationScheme"))
    {
        throw std::runtime_error("CAKKey.AuthenticationScheme is required");
    }
    if (!cakKey.contains("ECDSAKey"))
    {
        throw std::runtime_error("CAKKey.ECDSAKey is required");
    }

    std::string authScheme = cakKey["AuthenticationScheme"].get<std::string>();
    if (authScheme.empty())
    {
        throw std::runtime_error("CAKKey.AuthenticationScheme cannot be empty");
    }

    std::string ecdsaKey = cakKey["ECDSAKey"].get<std::string>();
    if (ecdsaKey.empty())
    {
        throw std::runtime_error("CAKKey.ECDSAKey cannot be empty");
    }

    if (payload.contains("LAKKey"))
    {
        const auto& lakKey = payload["LAKKey"];
        if (!lakKey.is_object())
        {
            throw std::runtime_error("LAKKey must be an object");
        }

        if (!lakKey.contains("AuthenticationScheme"))
        {
            throw std::runtime_error("LAKKey.AuthenticationScheme is required");
        }
        if (!lakKey.contains("ECDSAKey"))
        {
            throw std::runtime_error("LAKKey.ECDSAKey is required");
        }

        std::string lakAuthScheme =
            lakKey["AuthenticationScheme"].get<std::string>();
        if (lakAuthScheme.empty())
        {
            throw std::runtime_error(
                "LAKKey.AuthenticationScheme cannot be empty");
        }

        std::string lakEcdsaKey = lakKey["ECDSAKey"].get<std::string>();
        if (lakEcdsaKey.empty())
        {
            throw std::runtime_error("LAKKey.ECDSAKey cannot be empty");
        }
    }

    if (!payload.contains("LockDisable"))
    {
        throw std::runtime_error("LockDisable is required in JSON payload");
    }
    if (!payload["LockDisable"].is_boolean())
    {
        throw std::runtime_error("LockDisable must be a boolean");
    }
    if (!payload["LockDisable"].get<bool>())
    {
        throw std::runtime_error("LockDisable must be true");
    }

    if (!payload.contains("VendorMinimumSecurityVersion"))
    {
        throw std::runtime_error(
            "VendorMinimumSecurityVersion is required in JSON payload");
    }
    if (!payload["VendorMinimumSecurityVersion"].is_number())
    {
        throw std::runtime_error(
            "VendorMinimumSecurityVersion must be a number");
    }

    if (!payload.contains("OwnerMinimumSecurityVersion"))
    {
        throw std::runtime_error(
            "OwnerMinimumSecurityVersion is required in JSON payload");
    }
    if (!payload["OwnerMinimumSecurityVersion"].is_number())
    {
        throw std::runtime_error(
            "OwnerMinimumSecurityVersion must be a number");
    }
}

/**
 * @brief Extract the CAK ECDSA key from a DOT JSON payload.
 *
 * @param jsonPayload Serialized DOT JSON payload.
 * @return std::string CAK ECDSA key bytes.
 */
static std::string extractCakFromJson(const std::string& jsonPayload)
{
    json payload = json::parse(jsonPayload);
    validateDotPayload(payload);

    const auto& cakKey = payload["CAKKey"];
    return cakKey["ECDSAKey"].get<std::string>();
}

/**
 * @brief Run end-to-end CAK provisioning, reset, and post-check flow.
 *
 * @param config Active runtime configuration.
 * @param hostIsOff Shared host power-off state flag.
 */
static void provisionCak(const Config& config, std::atomic<bool>& hostIsOff)
{
    lg2::info("Starting CAK provisioning process");
    fs::path payloadFile = payloadPath(config.keyStorePath);
    if (!fs::exists(payloadFile))
    {
        throw std::runtime_error("No CAK payload found");
    }

    lg2::info("Reading CAK payload from {FILE}", "FILE", payloadFile.string());
    json payload;
    try
    {
        payload = json::parse(readFile(payloadFile));
        validateDotPayload(payload);
    }
    catch (const json::parse_error& ex)
    {
        lg2::error(
            "Invalid JSON in CAK payload file: {ERROR}. File will not be used.",
            "ERROR", ex.what());
        throw std::runtime_error("Invalid JSON in CAK payload file: " +
                                 std::string(ex.what()));
    }
    catch (const std::exception& ex)
    {
        lg2::error(
            "Invalid or unreadable CAK payload file: {ERROR}. File will not "
            "be used.",
            "ERROR", ex.what());
        throw std::runtime_error("Invalid or unreadable CAK payload file: " +
                                 std::string(ex.what()));
    }

    std::string cakBytes = extractCakFromJson(payload.dump());
    std::string payloadStr = payload.dump();

    if (config.hmc)
    {
        std::string currentState = getDotCakInitializationState(config);
        if (currentState == "Complete")
        {
            lg2::info("DOTCAKInitialization state is Complete - CAK already "
                      "installed, skipping installation");
            return;
        }

        lg2::info("Verifying DOT state before installation (waiting for "
                  "DOTCAKInitialization=Waiting or DOTState=Uninitialized)");
        try
        {
            ensureDotState(config, "Uninitialized");
            lg2::info(
                "DOT state verification before installation succeeded - "
                "DOTCAKInitialization is Waiting or DOTState is Uninitialized");
        }
        catch (const std::exception& ex)
        {
            lg2::warning(
                "Failed to verify DOT state before installation: {ERROR}. "
                "Proceeding with installation anyway.",
                "ERROR", ex.what());
        }

        std::string lastError;
        std::set<std::string> successfulPaths;

        lg2::info(
            "DOTCAKInitialization is Waiting - waiting 1 second for HMC "
            "Redfish endpoint to initialize before starting CAK installation "
            "(using 1-second intervals for timing data)");

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        lg2::info("Starting Redfish CAK installation (max {RETRIES} attempts)",
                  "RETRIES", config.cakInstallRetries);

        for (int attempt = 1; attempt <= config.cakInstallRetries; ++attempt)
        {
            if (hostIsOff.load())
            {
                lg2::info(
                    "Host powered off during CAK installation - aborting installation");
                throw std::runtime_error(
                    "CAK installation aborted: host powered off");
            }

            try
            {
                lg2::info("Redfish CAK installation attempt {ATTEMPT}/{MAX}",
                          "ATTEMPT", attempt, "MAX", config.cakInstallRetries);

                std::vector<std::string> remainingPaths;
                for (const auto& path : config.hmc->installPaths)
                {
                    if (successfulPaths.find(path) == successfulPaths.end())
                    {
                        remainingPaths.push_back(path);
                    }
                }

                if (remainingPaths.empty())
                {
                    lg2::info("All CPUs already have CAK installed");
                    break;
                }

                for (const auto& installPath : remainingPaths)
                {
                    std::string url =
                        "http://" + config.hmc->host + installPath;
                    lg2::info(
                        "Sending CAK installation request to Redfish endpoint: {URL}",
                        "URL", url);
                    lg2::info(
                        "Request configuration: method=POST, payload_size={SIZE}",
                        "SIZE", payloadStr.size());
                    if (!payloadStr.empty())
                    {
                        lg2::info("JSON payload: {PAYLOAD}", "PAYLOAD",
                                  payloadStr);
                    }

                    CURL* curl = curl_easy_init();
                    if (!curl)
                    {
                        throw std::runtime_error("curl init failed for " + url);
                    }

                    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                                     payloadStr.c_str());
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                                     config.timeouts.installMs);
                    std::string responseBody;
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                                     curlWriteCallback);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
                    if (config.hmc->username && config.hmc->password)
                    {
                        std::string auth =
                            *config.hmc->username + ":" + *config.hmc->password;
                        curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
                        lg2::info("Using HTTP authentication: username={USER}",
                                  "USER", *config.hmc->username);
                    }
                    else
                    {
                        lg2::info(
                            "No HTTP authentication configured (userless)");
                    }
                    struct curl_slist* headers = nullptr;
                    headers = curl_slist_append(
                        headers, "Content-Type: application/json");
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                    lg2::info("HTTP headers: Content-Type: application/json");

                    CURLcode res = curl_easy_perform(curl);
                    long status = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

                    char* effectiveUrl = nullptr;
                    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL,
                                      &effectiveUrl);
                    if (effectiveUrl && strcmp(effectiveUrl, url.c_str()) != 0)
                    {
                        lg2::info("Effective URL (after redirects): {URL}",
                                  "URL", effectiveUrl);
                    }

                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);

                    if (!responseBody.empty())
                    {
                        lg2::info(
                            "Response body from {URL} (HTTP {STATUS}): {BODY}",
                            "URL", url, "STATUS", status, "BODY", responseBody);
                    }
                    else
                    {
                        lg2::info(
                            "Response body from {URL} (HTTP {STATUS}): <empty>",
                            "URL", url, "STATUS", status);
                    }

                    if (res != CURLE_OK)
                    {
                        throw std::runtime_error(
                            "Redfish CAK installation request failed for " +
                            url + ": curl error " + std::to_string(res));
                    }
                    if (status >= 300)
                    {
                        throw std::runtime_error(
                            "Redfish CAK installation failed for " + url +
                            " with HTTP status " + std::to_string(status));
                    }

                    successfulPaths.insert(installPath);
                    lg2::info("CAK installation succeeded for {PATH}", "PATH",
                              installPath);
                }

                lastError.clear();
                lg2::info(
                    "Redfish CAK installation succeeded on attempt {ATTEMPT}",
                    "ATTEMPT", attempt);
                break;
            }
            catch (const std::exception& ex)
            {
                if (hostIsOff.load())
                {
                    lg2::info(
                        "Host powered off during CAK installation - aborting "
                        "installation");
                    throw std::runtime_error(
                        "CAK installation aborted: host powered off");
                }

                lastError = ex.what();
                lg2::warning(
                    "Redfish CAK installation attempt {ATTEMPT}/{MAX} failed: {ERROR}",
                    "ATTEMPT", attempt, "MAX", config.cakInstallRetries,
                    "ERROR", ex.what());
                if (attempt == config.cakInstallRetries)
                {
                    throw std::runtime_error(
                        "Redfish CAK installation failed after " +
                        std::to_string(config.cakInstallRetries) +
                        " attempts: " + lastError);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
    }
    else
    {
        fs::path tempKeyFile = config.keyStorePath / "cak_temp.pem";
        atomicWrite(tempKeyFile, cakBytes);
        try
        {
            installCakHmcless(config, tempKeyFile);
        }
        catch (...)
        {
            fs::remove(tempKeyFile);
            throw;
        }
        fs::remove(tempKeyFile);
    }
    if (hostIsOff.load())
    {
        lg2::info("Host powered off before L1 reset - aborting installation");
        throw std::runtime_error("CAK installation aborted: host powered off");
    }

    lg2::info("Performing L1 reset");
    l1Reset(config);
    if (config.hmc)
    {
        lg2::info("Verifying DOT state after installation");
        try
        {
            ensureDotState(config, "Volatile");
            lg2::info("DOT state verification succeeded");
        }
        catch (const std::exception& ex)
        {
            int timeoutSeconds = config.timeouts.installMs / 1000;
            lg2::error(
                "Failed to verify DOT state after installation: {ERROR}. "
                "System failed to boot within {TIMEOUT} seconds after CAK "
                "installation. Make sure correct CAK is being used.",
                "ERROR", ex.what(), "TIMEOUT", timeoutSeconds);
            throw std::runtime_error(
                "CAK verification failed: system failed to boot within " +
                std::to_string(timeoutSeconds) +
                " seconds after CAK installation. Make sure correct CAK is being "
                "used.");
        }
    }
    lg2::info("CAK provisioning process completed");
}

static void
    storeCakFromDbusArgs(const Config& config,
                         const std::tuple<std::string, std::string>& cakKey,
                         bool lockDisable, int64_t vendorMinimumSecurityVersion,
                         int64_t ownerMinimumSecurityVersion, bool hasLakKey,
                         const std::tuple<std::string, std::string>& lakKey)
{
    json payload;
    payload["CAKKey"] = {
        {"AuthenticationScheme", std::get<0>(cakKey)},
        {"ECDSAKey", std::get<1>(cakKey)},
    };
    payload["LockDisable"] = lockDisable;
    payload["VendorMinimumSecurityVersion"] = vendorMinimumSecurityVersion;
    payload["OwnerMinimumSecurityVersion"] = ownerMinimumSecurityVersion;

    if (hasLakKey)
    {
        payload["LAKKey"] = {
            {"AuthenticationScheme", std::get<0>(lakKey)},
            {"ECDSAKey", std::get<1>(lakKey)},
        };
    }

    validateDotPayload(payload);

    std::string cakEcdsaKey = payload["CAKKey"]["ECDSAKey"].get<std::string>();
    normalizePemKeyFormat(cakEcdsaKey);
    payload["CAKKey"]["ECDSAKey"] = cakEcdsaKey;

    if (hasLakKey)
    {
        std::string lakEcdsaKey =
            payload["LAKKey"]["ECDSAKey"].get<std::string>();
        normalizePemKeyFormat(lakEcdsaKey);
        payload["LAKKey"]["ECDSAKey"] = lakEcdsaKey;
    }

    std::string normalizedPayload =
        payload.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
    validateCakBytes(cakEcdsaKey, config.maxCakBytes);
    atomicWrite(payloadPath(config.keyStorePath), normalizedPayload);
}

/**
 * @brief Build default runtime configuration values.
 *
 * @return Config Default configuration.
 */
static Config getDefaultConfig()
{
    Config config;
    config.keyStorePath = kDefaultKeyStorePath;

    HmcConfig hmc;
    hmc.host = "172.31.13.251";
    config.hmc = hmc;

    return config;
}

/**
 * @brief Parse configuration JSON into an internal Config model.
 *
 * @param data Source JSON object.
 * @return Config Parsed configuration.
 */
static Config parseConfig(const json& data)
{
    Config config = getDefaultConfig();
    if (data.contains("keyStorePath"))
    {
        config.keyStorePath = data.at("keyStorePath").get<std::string>();
    }
    else if (data.contains("cakStorePath"))
    {
        config.keyStorePath = data.at("cakStorePath").get<std::string>();
    }

    if (data.contains("hmc"))
    {
        HmcConfig hmc;
        const auto& hmcData = data.at("hmc");
        hmc.host = hmcData.at("host").get<std::string>();
        if (hmcData.contains("username"))
        {
            hmc.username = hmcData.at("username").get<std::string>();
        }
        if (hmcData.contains("password"))
        {
            hmc.password = hmcData.at("password").get<std::string>();
        }
        hmc.verifyTls = hmcData.value("verifyTls", false);
        if (hmcData.contains("installPaths"))
        {
            hmc.installPaths =
                hmcData.at("installPaths").get<std::vector<std::string>>();
        }
        if (hmcData.contains("statusPaths"))
        {
            hmc.statusPaths =
                hmcData.at("statusPaths").get<std::vector<std::string>>();
        }
        if (hmcData.contains("dotCakInitPath"))
        {
            hmc.dotCakInitPath =
                hmcData.at("dotCakInitPath").get<std::string>();
        }
        config.hmc = hmc;
    }

    if (data.contains("hmclessExec"))
    {
        HmclessExecConfig exec;
        const auto& execData = data.at("hmclessExec");
        exec.path = execData.at("path").get<std::string>();
        exec.args = execData.value("args", std::vector<std::string>{});
        config.hmclessExec = exec;
    }

    if (data.contains("l1Reset"))
    {
        const auto& l1Reset = data.at("l1Reset");
        if (l1Reset.contains("i2cBus"))
        {
            config.l1Reset.i2cBus = l1Reset.at("i2cBus").get<int>();
        }
        if (l1Reset.contains("i2cAddr"))
        {
            config.l1Reset.i2cAddr = l1Reset.at("i2cAddr").get<std::string>();
        }
    }

    const auto& timeouts = data.value("timeouts", json::object());
    config.timeouts.installMs =
        timeouts.value("installMs", kDefaultInstallTimeoutMs);
    config.timeouts.dotCakInitMs =
        timeouts.value("dotCakInitMs", kDefaultDotCakInitTimeoutMs);

    config.maxCakBytes = data.value("maxCakBytes", kDefaultCakMaxBytes);
    config.allowCakReadout = data.value("allowCakReadout", true);
    if (data.contains("minimumSecurityVersion"))
    {
        config.minimumSecurityVersion =
            data.at("minimumSecurityVersion").get<int>();
    }
    config.cakInstallRetries =
        data.value("cakInstallRetries", kDefaultCakInstallRetries);

    return config;
}

/**
 * @brief Apply CLI overrides to a resolved configuration.
 *
 * @param config Base configuration.
 * @param args Parsed CLI arguments.
 * @return Config Updated configuration.
 */
static Config applyCliOverrides(Config config, const Args& args)
{
    if (args.keyStorePath)
    {
        config.keyStorePath = *args.keyStorePath;
    }
    if (args.hmclessExec)
    {
        HmclessExecConfig exec;
        exec.path = *args.hmclessExec;
        exec.args = args.hmclessArgs;
        config.hmclessExec = exec;
    }
    if (args.i2cBus)
    {
        config.l1Reset.i2cBus = *args.i2cBus;
    }
    if (args.i2cAddr)
    {
        config.l1Reset.i2cAddr = *args.i2cAddr;
    }
    return config;
}

/**
 * @brief Resolve final configuration from defaults, file, and CLI.
 *
 * @param args Parsed CLI arguments.
 * @return Config Final resolved configuration.
 */
static Config resolveConfig(const Args& args)
{
    Config config = getDefaultConfig();

    if (!args.configPath.empty() && fs::exists(args.configPath))
    {
        try
        {
            json data = json::parse(readFile(args.configPath));
            config = parseConfig(data);
        }
        catch (const std::exception& ex)
        {
            throw std::runtime_error("Failed to parse config file: " +
                                     std::string(ex.what()));
        }
    }

    return applyCliOverrides(config, args);
}

/**
 * @brief Execute one-shot install flow for standalone CLI mode.
 *
 * @param args Parsed CLI arguments.
 * @return int Process-style exit code.
 */
static int installFlow(const Args& args)
{
    Config config;
    try
    {
        config = resolveConfig(args);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "config error: " << ex.what() << "\n";
        return ExitCode::kInvalidArgs;
    }

    try
    {
        ensureDir(config.keyStorePath);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "storage error: " << ex.what() << "\n";
        return ExitCode::kStorageError;
    }

    if (args.cakPath)
    {
        try
        {
            storeCak(config, readFile(*args.cakPath));
        }
        catch (const std::exception& ex)
        {
            std::cerr << "invalid CAK: " << ex.what() << "\n";
            return ExitCode::kInvalidCak;
        }
    }
    else if (!fs::exists(payloadPath(config.keyStorePath)))
    {
        std::cerr << "CAK is required (no stored CAK found)\n";
        return ExitCode::kInvalidArgs;
    }

    std::atomic<bool> hostIsOff{false};

    auto start = std::chrono::steady_clock::now();
    try
    {
        provisionCak(config, hostIsOff);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "provisioning error: " << ex.what() << "\n";
        return ExitCode::kProvisioningError;
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    if (elapsedMs > config.timeouts.installMs)
    {
        std::cerr << "install timeout exceeded\n";
        return ExitCode::kProvisioningError;
    }
    return ExitCode::kSuccess;
}

/**
 * @brief Run D-Bus service mode and host-state driven install logic.
 *
 * @param args Parsed CLI arguments.
 * @return int Process-style exit code.
 */
static int runService(const Args& args)
{
    Config config;
    try
    {
        config = resolveConfig(args);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "config error: " << ex.what() << "\n";
        return ExitCode::kInvalidArgs;
    }

    boost::asio::io_context io;
    auto bus = std::make_shared<sdbusplus::asio::connection>(io);
    bus->request_name("xyz.openbmc_project.Security.DOT");
    sdbusplus::asio::object_server server(bus);
    auto iface = server.add_interface("/xyz/openbmc_project/security/dot",
                                      "xyz.openbmc_project.Security.DOT");

    std::string lastInstallStatus = "NotInstalled";
    std::atomic<bool> installInProgress{false};
    std::atomic<bool> hostIsOff{false};

    iface->register_property_r<bool>(
        "Stored", sdbusplus::vtable::property_::emits_change,
        [&config](const bool&) {
            return fs::exists(payloadPath(config.keyStorePath));
        });
    iface->register_property_r<std::string>(
        "CAKValue", sdbusplus::vtable::property_::emits_change,
        [&config](const std::string&) {
            if (!config.allowCakReadout)
            {
                return std::string{};
            }
            try
            {
                fs::path payloadFile = payloadPath(config.keyStorePath);
                if (fs::exists(payloadFile))
                {
                    std::string content = readFile(payloadFile);
                    try
                    {
                        json payload = json::parse(content);
                        validateDotPayload(payload);
                        return content;
                    }
                    catch (const json::parse_error& ex)
                    {
                        lg2::error(
                            "CAKValue: Invalid JSON in payload file: {ERROR}. "
                            "Returning empty string.",
                            "ERROR", ex.what());
                        return std::string{};
                    }
                    catch (const std::exception& ex)
                    {
                        lg2::error(
                            "CAKValue: Invalid payload structure: {ERROR}. "
                            "Returning empty string.",
                            "ERROR", ex.what());
                        return std::string{};
                    }
                }
                return std::string{};
            }
            catch (const std::exception& ex)
            {
                lg2::error("CAKValue: Failed to read payload file: {ERROR}. "
                           "Returning empty string.",
                           "ERROR", ex.what());
                return std::string{};
            }
        });
    iface->register_property_r<std::string>(
        "KeyStorePath", sdbusplus::vtable::property_::emits_change,
        [&config](const std::string&) { return config.keyStorePath.string(); });
    iface->register_property_r<std::string>(
        "CAKInstalled", sdbusplus::vtable::property_::emits_change,
        [&lastInstallStatus](const std::string&) { return lastInstallStatus; });
    iface->register_property_r<int>(
        "CAKVerificationTimeoutInSeconds",
        sdbusplus::vtable::property_::emits_change,
        [&config](const int&) { return config.timeouts.installMs / 1000; });

    iface->register_method("InstallCak", [&config, &lastInstallStatus,
                                          &installInProgress, &hostIsOff,
                                          iface]() {
        if (installInProgress.exchange(true))
        {
            return;
        }

        std::thread([&config, &lastInstallStatus, &installInProgress,
                     &hostIsOff, iface]() {
            struct ResetFlag
            {
                std::atomic<bool>& flag;
                ~ResetFlag()
                {
                    flag.store(false);
                }
            } reset{installInProgress};

            try
            {
                provisionCak(config, hostIsOff);
                lastInstallStatus = "Installed";
                iface->signal_property("CAKInstalled");
            }
            catch (const std::exception& ex)
            {
                std::string errorMsg = ex.what();
                if (errorMsg.find("CAK verification failed") !=
                    std::string::npos)
                {
                    lastInstallStatus = "Error";
                    lg2::error(
                        "InstallCak: CAK verification failed - wrong CAK may "
                        "have been installed: {ERROR}",
                        "ERROR", errorMsg);
                }
                else if (hostIsOff.load() &&
                         errorMsg.find("host powered off") != std::string::npos)
                {
                    lastInstallStatus = "NotInstalled";
                    lg2::info(
                        "InstallCak: Installation aborted because host powered off");
                }
                else
                {
                    lastInstallStatus = "NotInstalled";
                    lg2::error(
                        "InstallCak: Installation failed: {ERROR}. Service "
                        "continues running.",
                        "ERROR", errorMsg);
                }
                iface->signal_property("CAKInstalled");
            }
        }).detach();
    });

    iface->register_method(
        "installCak2BmcFs",
        [&config, iface](const std::tuple<std::string, std::string>& cakKey,
                         const bool& lockDisable,
                         const int64_t& vendorMinimumSecurityVersion,
                         const int64_t& ownerMinimumSecurityVersion,
                         const bool& hasLakKey,
                         const std::tuple<std::string, std::string>& lakKey) {
            try
            {
                storeCakFromDbusArgs(
                    config, cakKey, lockDisable, vendorMinimumSecurityVersion,
                    ownerMinimumSecurityVersion, hasLakKey, lakKey);
                iface->signal_property("Stored");
                return std::string{"Success"};
            }
            catch (const std::exception& ex)
            {
                return std::string{"Error: "} + ex.what();
            }
        });
    iface->register_method("DeleteCak", [&config, iface]() {
        try
        {
            deleteCak(config);
            iface->signal_property("Stored");
            return std::string{"Success"};
        }
        catch (const std::exception& ex)
        {
            return std::string{"Error: "} + ex.what();
        }
    });

    iface->initialize();

    constexpr const char* hostStateService = "xyz.openbmc_project.State.Host";
    constexpr const char* hostStatePath = "/xyz/openbmc_project/state/host0";
    constexpr const char* hostStateInterface = "xyz.openbmc_project.State.Host";
    constexpr const char* hostStateProperty = "CurrentHostState";
    constexpr const char* hostStateOn =
        "xyz.openbmc_project.State.Host.HostState.Running";
    constexpr const char* hostStateOff =
        "xyz.openbmc_project.State.Host.HostState.Off";

    auto hostStateMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*bus),
        sdbusplus::bus::match::rules::propertiesChanged(hostStatePath,
                                                        hostStateInterface),
        [&config, &lastInstallStatus, &installInProgress, &hostIsOff, iface,
         bus](sdbusplus::message_t& msg) {
            std::string interface;
            std::map<std::string, std::variant<std::string>> properties;
            msg.read(interface, properties);

            auto it = properties.find(hostStateProperty);
            if (it != properties.end())
            {
                const std::string* state =
                    std::get_if<std::string>(&it->second);
                if (state != nullptr)
                {
                    if (*state == hostStateOn)
                    {
                        hostIsOff.store(false);

                        if (config.hmc)
                        {
                            std::string cakState =
                                getDotCakInitializationState(config);
                            if (cakState == "Complete")
                            {
                                lg2::info(
                                    "HostState changed to Running and CAK is already "
                                    "installed (DOTCAKInitialization=Complete) - "
                                    "skipping installation");
                                lastInstallStatus = "Installed";
                                iface->signal_property("CAKInstalled");
                                return;
                            }
                        }

                        if (!installInProgress.exchange(true))
                        {
                            lg2::info(
                                "HostState changed to Running - triggering CAK "
                                "installation");
                            std::thread([&config, &lastInstallStatus,
                                         &installInProgress, &hostIsOff,
                                         iface]() {
                                struct ResetFlag
                                {
                                    std::atomic<bool>& flag;
                                    ~ResetFlag()
                                    {
                                        flag.store(false);
                                    }
                                } reset{installInProgress};

                                try
                                {
                                    provisionCak(config, hostIsOff);
                                    lastInstallStatus = "Installed";
                                    iface->signal_property("CAKInstalled");
                                }
                                catch (const std::exception& ex)
                                {
                                    std::string errorMsg = ex.what();
                                    if (errorMsg.find(
                                            "CAK verification failed") !=
                                        std::string::npos)
                                    {
                                        lastInstallStatus = "Error";
                                        lg2::error(
                                            "CAK installation on power-on: CAK verification "
                                            "failed - wrong CAK may have been installed: {ERROR}",
                                            "ERROR", errorMsg);
                                    }
                                    else if (hostIsOff.load())
                                    {
                                        lastInstallStatus = "NotInstalled";
                                        lg2::info(
                                            "CAK installation aborted because host powered off");
                                    }
                                    else
                                    {
                                        lastInstallStatus = "NotInstalled";
                                        lg2::error(
                                            "CAK installation failed on power-on: "
                                            "{ERROR}. Service continues running.",
                                            "ERROR", errorMsg);
                                    }
                                    iface->signal_property("CAKInstalled");
                                }
                            }).detach();
                        }
                        else
                        {
                            lg2::info(
                                "HostState changed to Running but CAK installation "
                                "already in progress - skipping");
                        }
                    }
                    else if (*state == hostStateOff)
                    {
                        lg2::info(
                            "HostState changed to Off - resetting CAKInstalled to "
                            "NotInstalled and aborting any ongoing installation");
                        hostIsOff.store(true);
                        lastInstallStatus = "NotInstalled";
                        iface->signal_property("CAKInstalled");
                    }
                }
            }
        });

    sdbusplus::asio::getProperty<std::string>(
        *bus, hostStateService, hostStatePath, hostStateInterface,
        hostStateProperty,
        [&config, &lastInstallStatus, &installInProgress, &hostIsOff, iface,
         hostStateOn, hostStateOff](const boost::system::error_code& ec,
                                    const std::string& state) {
            if (ec)
            {
                lg2::warning(
                    "Failed to get initial HostState: {ERROR}. Will monitor "
                    "for changes.",
                    "ERROR", ec.message());
                return;
            }
            lg2::info("Initial HostState: {STATE}", "STATE", state);
            if (state == hostStateOn)
            {
                if (config.hmc)
                {
                    std::string cakState = getDotCakInitializationState(config);
                    if (cakState == "Complete")
                    {
                        lg2::info(
                            "Host is Running and CAK is already installed "
                            "(DOTCAKInitialization=Complete) - skipping installation");
                        lastInstallStatus = "Installed";
                        iface->signal_property("CAKInstalled");
                        return;
                    }
                }

                if (!installInProgress.exchange(true))
                {
                    lg2::info(
                        "Host is already Running on startup - triggering CAK "
                        "installation");
                    std::thread([&config, &lastInstallStatus,
                                 &installInProgress, &hostIsOff, iface]() {
                        struct ResetFlag
                        {
                            std::atomic<bool>& flag;
                            ~ResetFlag()
                            {
                                flag.store(false);
                            }
                        } reset{installInProgress};

                        try
                        {
                            provisionCak(config, hostIsOff);
                            lastInstallStatus = "Installed";
                            iface->signal_property("CAKInstalled");
                        }
                        catch (const std::exception& ex)
                        {
                            std::string errorMsg = ex.what();
                            if (errorMsg.find("CAK verification failed") !=
                                std::string::npos)
                            {
                                lastInstallStatus = "Error";
                                lg2::error(
                                    "Auto-install on startup: CAK verification failed "
                                    "- wrong CAK may have been installed: {ERROR}",
                                    "ERROR", errorMsg);
                            }
                            else if (hostIsOff.load() &&
                                     errorMsg.find("host powered off") !=
                                         std::string::npos)
                            {
                                lastInstallStatus = "NotInstalled";
                                lg2::info(
                                    "Auto-install on startup aborted because host "
                                    "powered off");
                            }
                            else
                            {
                                lastInstallStatus = "NotInstalled";
                                lg2::error(
                                    "Auto-install on startup failed: {ERROR}. Service "
                                    "continues running.",
                                    "ERROR", errorMsg);
                            }
                            iface->signal_property("CAKInstalled");
                        }
                    }).detach();
                }
            }
            else if (state == hostStateOff)
            {
                lastInstallStatus = "NotInstalled";
                iface->signal_property("CAKInstalled");
            }
        });

    boost::asio::signal_set signals(io, SIGTERM, SIGINT);
    signals.async_wait(
        [&](const boost::system::error_code&, int) { io.stop(); });
    io.run();
    return ExitCode::kSuccess;
}

/**
 * @brief Split whitespace-delimited text into argument tokens.
 *
 * @param input Input argument string.
 * @return std::vector<std::string> Tokenized arguments.
 */
static std::vector<std::string> splitArgs(const std::string& input)
{
    std::istringstream stream(input);
    std::vector<std::string> out;
    std::string token;
    while (stream >> token)
    {
        out.push_back(token);
    }
    return out;
}

/**
 * @brief Parse command-line options into Args.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Args Parsed argument structure.
 */
static Args parseArgs(int argc, char** argv)
{
    Args args;
    const char* const shortOpts = "";
    const option longOpts[] = {
        {"config", required_argument, nullptr, 0},
        {"install", no_argument, nullptr, 0},
        {"delete", no_argument, nullptr, 0},
        {"cak", required_argument, nullptr, 0},
        {"key-store-path", required_argument, nullptr, 0},
        {"hmcless-exec", required_argument, nullptr, 0},
        {"hmcless-args", required_argument, nullptr, 0},
        {"i2c-bus", required_argument, nullptr, 0},
        {"i2c-addr", required_argument, nullptr, 0},
        {nullptr, 0, nullptr, 0},
    };
    int longIndex = 0;
    while (true)
    {
        int opt = ::getopt_long(argc, argv, shortOpts, longOpts, &longIndex);
        if (opt == -1)
        {
            break;
        }
        if (opt != 0)
        {
            continue;
        }
        std::string name = longOpts[longIndex].name;
        if (name == "config")
        {
            args.configPath = optarg;
        }
        else if (name == "install")
        {
            args.install = true;
        }
        else if (name == "delete")
        {
            args.deleteCak = true;
        }
        else if (name == "cak")
        {
            args.cakPath = optarg;
        }
        else if (name == "key-store-path")
        {
            args.keyStorePath = optarg;
        }
        else if (name == "hmcless-exec")
        {
            args.hmclessExec = optarg;
        }
        else if (name == "hmcless-args")
        {
            args.hmclessArgs = splitArgs(optarg);
        }
        else if (name == "i2c-bus")
        {
            args.i2cBus = std::stoi(optarg);
        }
        else if (name == "i2c-addr")
        {
            args.i2cAddr = optarg;
        }
    }
    return args;
}

/**
 * @brief Program entry point and mode dispatcher.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return int Process-style exit code.
 */
int main(int argc, char** argv)
{
    Args args = parseArgs(argc, argv);
    if (args.deleteCak)
    {
        try
        {
            Config config = resolveConfig(args);
            deleteCak(config);
            return ExitCode::kSuccess;
        }
        catch (const std::exception& ex)
        {
            std::cerr << "delete error: " << ex.what() << "\n";
            return ExitCode::kStorageError;
        }
    }
    if (args.install)
    {
        return installFlow(args);
    }
    return runService(args);
}
