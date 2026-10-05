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

#include "cak_baked_key.hpp"

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

using json = nlohmann::json;

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
    if (::chmod(path.c_str(), 0700) != 0)
    {
        throw std::runtime_error("chmod failed on " + path.string() + ": " +
                                 std::string(std::strerror(errno)));
    }
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
    try
    {
        fs::rename(tmpPath, path);
    }
    catch (...)
    {
        ::unlink(tmpPath.c_str());
        throw;
    }
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
        throw std::invalid_argument("CAK is empty");
    }
    if (data.size() > maxBytes)
    {
        throw std::invalid_argument("CAK exceeds maximum size");
    }
    if (!isValidPem(data))
    {
        throw std::invalid_argument("CAK is not a valid PEM public key");
    }
}

/** Rejects a CAKKey/LAKKey ECDSAKey that is empty, oversized or not a PEM
 *  public key, naming the field so the caller can report it. */
void validateEcdsaPem(const std::string& field, const std::string& pem,
                      size_t maxBytes)
{
    if (pem.empty())
    {
        throw std::invalid_argument(field + ".ECDSAKey cannot be empty");
    }
    if (pem.size() > maxBytes)
    {
        throw std::invalid_argument(field + ".ECDSAKey exceeds maximum size");
    }
    if (!isValidPem(pem))
    {
        throw std::invalid_argument(field +
                                    ".ECDSAKey is not a valid PEM public key");
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

namespace
{

/** Returns the canonical spelling of a DOT key authentication scheme.
 *  The Redfish schema spells the ECDSA member "ECDSA" while nsmd and the
 *  baked payload use "Ecdsa"; accept both and settle on one spelling. */
std::string canonicalAuthScheme(const std::string& field,
                                const std::string& scheme)
{
    if (scheme == "Ecdsa" || scheme == "ECDSA" || scheme == "ecdsa")
    {
        return "Ecdsa";
    }
    if (scheme == "Hybrid" || scheme == "hybrid")
    {
        return "Hybrid";
    }
    throw std::invalid_argument(field +
                                ".AuthenticationScheme must be Ecdsa or "
                                "Hybrid, got '" +
                                scheme + "'");
}

/** Validates one KeyStructure (CAKKey or LAKKey): the allowed members, the
 *  scheme, the ECDSA key, and the LMS key that a Hybrid scheme requires. */
void validateKeyStructure(const std::string& field, const json& key)
{
    if (!key.is_object())
    {
        throw std::invalid_argument(field + " must be an object");
    }

    static const std::set<std::string> allowed = {"AuthenticationScheme",
                                                  "ECDSAKey", "LMSKey"};
    for (const auto& [member, value] : key.items())
    {
        if (allowed.find(member) == allowed.end())
        {
            throw std::invalid_argument(
                "Disallowed field in JSON payload: " + field + "." + member);
        }
    }

    if (!key.contains("AuthenticationScheme"))
    {
        throw std::invalid_argument(field +
                                    ".AuthenticationScheme is required");
    }
    if (!key["AuthenticationScheme"].is_string())
    {
        throw std::invalid_argument(field +
                                    ".AuthenticationScheme must be a string");
    }
    std::string scheme = key["AuthenticationScheme"].get<std::string>();
    if (scheme.empty())
    {
        throw std::invalid_argument(field +
                                    ".AuthenticationScheme cannot be empty");
    }
    scheme = canonicalAuthScheme(field, scheme);

    if (!key.contains("ECDSAKey"))
    {
        throw std::invalid_argument(field + ".ECDSAKey is required");
    }
    if (!key["ECDSAKey"].is_string())
    {
        throw std::invalid_argument(field + ".ECDSAKey must be a string");
    }
    if (key["ECDSAKey"].get<std::string>().empty())
    {
        throw std::invalid_argument(field + ".ECDSAKey cannot be empty");
    }

    if (scheme == "Hybrid")
    {
        if (!key.contains("LMSKey"))
        {
            throw std::invalid_argument(
                field +
                ".LMSKey is required when AuthenticationScheme is Hybrid");
        }
        if (!key["LMSKey"].is_string())
        {
            throw std::invalid_argument(field + ".LMSKey must be a string");
        }
        if (key["LMSKey"].get<std::string>().empty())
        {
            throw std::invalid_argument(field + ".LMSKey cannot be empty");
        }
    }
    else if (key.contains("LMSKey"))
    {
        throw std::invalid_argument(
            field +
            ".LMSKey is only valid when AuthenticationScheme is Hybrid");
    }
}

} // namespace

std::string dotAuthScheme(const std::string& scheme)
{
    return canonicalAuthScheme("Key", scheme);
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
            throw std::invalid_argument("Disallowed field in JSON payload: " +
                                        key);
        }
    }

    if (!payload.contains("CAKKey"))
    {
        throw std::invalid_argument("CAKKey is required in JSON payload");
    }
    validateKeyStructure("CAKKey", payload["CAKKey"]);

    if (payload.contains("LAKKey"))
    {
        validateKeyStructure("LAKKey", payload["LAKKey"]);
    }

    if (!payload.contains("LockDisable"))
    {
        throw std::invalid_argument("LockDisable is required in JSON payload");
    }
    if (!payload["LockDisable"].is_boolean())
    {
        throw std::invalid_argument("LockDisable must be a boolean");
    }
    if (!payload["LockDisable"].get<bool>())
    {
        throw std::invalid_argument("LockDisable must be true");
    }

    if (!payload.contains("VendorMinimumSecurityVersion"))
    {
        throw std::invalid_argument(
            "VendorMinimumSecurityVersion is required in JSON payload");
    }
    if (!payload["VendorMinimumSecurityVersion"].is_number())
    {
        throw std::invalid_argument(
            "VendorMinimumSecurityVersion must be a number");
    }

    if (!payload.contains("OwnerMinimumSecurityVersion"))
    {
        throw std::invalid_argument(
            "OwnerMinimumSecurityVersion is required in JSON payload");
    }
    if (!payload["OwnerMinimumSecurityVersion"].is_number())
    {
        throw std::invalid_argument(
            "OwnerMinimumSecurityVersion must be a number");
    }
}

void normalizeDotPayload(json& payload)
{
    validateDotPayload(payload);
    for (const char* field : {"CAKKey", "LAKKey"})
    {
        if (!payload.contains(field))
        {
            continue;
        }
        json& key = payload[field];
        key["AuthenticationScheme"] = canonicalAuthScheme(
            field, key["AuthenticationScheme"].get<std::string>());
        std::string pem = key["ECDSAKey"].get<std::string>();
        normalizePemKeyFormat(pem);
        key["ECDSAKey"] = pem;
    }
}

std::string extractCakFromJson(const std::string& jsonPayload)
{
    json payload = json::parse(jsonPayload);
    validateDotPayload(payload);
    return payload["CAKKey"]["ECDSAKey"].get<std::string>();
}

// ---------------------------------------------------------------------------
// CAK payload assembly
// ---------------------------------------------------------------------------

json buildCakPayload(const Config& config, const std::string& cakBytes)
{
    return json{
        {"CAKKey", {{"AuthenticationScheme", "Ecdsa"}, {"ECDSAKey", cakBytes}}},
        {"LockDisable", true},
        {"VendorMinimumSecurityVersion",
         config.minimumSecurityVersion.value_or(0)},
        {"OwnerMinimumSecurityVersion",
         config.minimumSecurityVersion.value_or(0)}};
}

std::optional<json> bakedCakPayload(const Config& config)
{
    if constexpr (!dot::kCakKeyBaked)
    {
        return std::nullopt;
    }
    else
    {
        std::string pem(dot::kCakKeyBakedPem);
        normalizePemKeyFormat(pem);
        validateCakBytes(pem, config.maxCakBytes);
        return buildCakPayload(config, pem);
    }
}

// ---------------------------------------------------------------------------
// CAK key storage
// ---------------------------------------------------------------------------

void storeCak(const Config& config, const std::string& cakBytes)
{
    std::string pem(cakBytes);
    normalizePemKeyFormat(pem);
    validateCakBytes(pem, config.maxCakBytes);
    atomicWrite(payloadPath(config.keyStorePath),
                buildCakPayload(config, pem).dump());
}

void storeDotPayload(const Config& config, json payload)
{
    if (!payload.is_object())
    {
        throw std::invalid_argument("CAK payload is not an object");
    }

    normalizeDotPayload(payload);
    for (const char* field : {"CAKKey", "LAKKey"})
    {
        if (payload.contains(field))
        {
            validateEcdsaPem(field,
                             payload[field]["ECDSAKey"].get<std::string>(),
                             config.maxCakBytes);
        }
    }
    atomicWrite(
        payloadPath(config.keyStorePath),
        payload.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace));
}

void storeDotPayload(const Config& config, const std::string& jsonPayload)
{
    json payload;
    try
    {
        payload = json::parse(jsonPayload);
    }
    catch (const json::parse_error& ex)
    {
        throw std::invalid_argument("Invalid JSON in CAK payload: " +
                                    std::string(ex.what()));
    }
    storeDotPayload(config, std::move(payload));
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
