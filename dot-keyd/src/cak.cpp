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

#include "cak.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

bool isValidPem(const std::string& data)
{
    return data.find("BEGIN PUBLIC KEY") != std::string::npos &&
           data.find("END PUBLIC KEY") != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// File utilities
// ---------------------------------------------------------------------------

void ensureDir(const fs::path& path)
{
    fs::create_directories(path);
    ::chmod(path.c_str(), 0700);
}

void atomicWrite(const fs::path& path, const std::string& data)
{
    ensureDir(path.parent_path());
    std::string tmpl = (path.parent_path() / "tmp.XXXXXX").string();
    std::vector<char> tmp(tmpl.begin(), tmpl.end());
    tmp.push_back('\0');
    int fd = ::mkstemp(tmp.data());
    if (fd < 0)
    {
        throw std::runtime_error("mkstemp failed: " +
                                 std::string(std::strerror(errno)));
    }
    fs::path tmpPath(tmp.data());
    const char* buf = data.data();
    size_t remaining = data.size();
    while (remaining > 0)
    {
        ssize_t n = ::write(fd, buf, remaining);
        if (n < 0)
        {
            int err = errno;
            ::close(fd);
            ::unlink(tmpPath.c_str());
            throw std::runtime_error("write failed: " +
                                     std::string(std::strerror(err)));
        }
        buf += n;
        remaining -= static_cast<size_t>(n);
    }
    if (::fsync(fd) < 0 || ::fchmod(fd, 0600) < 0)
    {
        int err = errno;
        ::close(fd);
        ::unlink(tmpPath.c_str());
        throw std::runtime_error("fsync/fchmod failed: " +
                                 std::string(std::strerror(err)));
    }
    ::close(fd);
    fs::rename(tmpPath, path);
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

fs::path payloadPath(const fs::path& keyStorePath)
{
    return keyStorePath / kDefaultPayloadFilename;
}

// ---------------------------------------------------------------------------
// CAK key validation and normalization
// ---------------------------------------------------------------------------

void validateCakBytes(const std::string& data, size_t maxBytes)
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

void normalizePemKeyFormat(std::string& key)
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

void validateDotPayload(const json& payload)
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

    if (!cakKey["AuthenticationScheme"].is_string())
    {
        throw std::runtime_error(
            "CAKKey.AuthenticationScheme must be a string");
    }
    std::string authScheme = cakKey["AuthenticationScheme"].get<std::string>();
    if (authScheme.empty())
    {
        throw std::runtime_error("CAKKey.AuthenticationScheme cannot be empty");
    }

    if (!cakKey["ECDSAKey"].is_string())
    {
        throw std::runtime_error("CAKKey.ECDSAKey must be a string");
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

        if (!lakKey["AuthenticationScheme"].is_string())
        {
            throw std::runtime_error(
                "LAKKey.AuthenticationScheme must be a string");
        }
        std::string lakAuthScheme =
            lakKey["AuthenticationScheme"].get<std::string>();
        if (lakAuthScheme.empty())
        {
            throw std::runtime_error(
                "LAKKey.AuthenticationScheme cannot be empty");
        }

        if (!lakKey["ECDSAKey"].is_string())
        {
            throw std::runtime_error("LAKKey.ECDSAKey must be a string");
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

std::string extractCakFromJson(const std::string& jsonPayload)
{
    json payload = json::parse(jsonPayload);
    validateDotPayload(payload);
    return payload["CAKKey"]["ECDSAKey"].get<std::string>();
}

// ---------------------------------------------------------------------------
// CAK key storage
// ---------------------------------------------------------------------------

void storeCak(const Config& config, const std::string& cakBytes)
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

void deleteCak(const Config& config)
{
    auto removeIfPresent = [](const fs::path& p) {
        std::error_code ec;
        fs::remove(p, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
        {
            throw fs::filesystem_error("failed to remove file", p, ec);
        }
    };

    removeIfPresent(payloadPath(config.keyStorePath));
    removeIfPresent(config.keyStorePath / "cak.pem");
    removeIfPresent(config.keyStorePath / "cak_metadata.json");
}
