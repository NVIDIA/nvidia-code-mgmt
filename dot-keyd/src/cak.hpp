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

#pragma once

#include "config.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// File utilities
// ---------------------------------------------------------------------------

void ensureDir(const fs::path& path);
void atomicWrite(const fs::path& path, const std::string& data);
std::string readFile(const fs::path& path);
fs::path payloadPath(const fs::path& keyStorePath);

// ---------------------------------------------------------------------------
// CAK key validation and normalization
// ---------------------------------------------------------------------------

void validateCakBytes(const std::string& data, size_t maxBytes);
void normalizePemKeyFormat(std::string& key);
void validateEcdsaPem(const std::string& field, const std::string& pem,
                      size_t maxBytes);
void validateDotPayload(const nlohmann::json& payload);
// Validates the payload, then settles AuthenticationScheme on its canonical
// spelling ("Ecdsa"/"Hybrid") and normalizes the ECDSA PEM keys in place.
void normalizeDotPayload(nlohmann::json& payload);
// Maps any accepted spelling of a key authentication scheme ("ECDSA", "Ecdsa",
// "Hybrid", ...) to its canonical form; throws on an unknown scheme.
std::string dotAuthScheme(const std::string& scheme);
std::string extractCakFromJson(const std::string& jsonPayload);

// ---------------------------------------------------------------------------
// CAK payload assembly
// ---------------------------------------------------------------------------

// Builds the DOT install payload JSON from a CAK PEM public key, filling in the
// standard defaults (AuthenticationScheme=Ecdsa, LockDisable=true, no LAKKey,
// and both MinimumSecurityVersion fields from config, defaulting to 0).
nlohmann::json buildCakPayload(const Config& config,
                               const std::string& cakBytes);

// Returns the install payload synthesized from the compiled-in (baked) CAK PEM
// public key, or std::nullopt when no key was baked in at build time
// (-Dcak_key / DOT_KEYD_CAK_KEY empty). The baked PEM is normalized and
// validated before use.
std::optional<nlohmann::json> bakedCakPayload(const Config& config);

// ---------------------------------------------------------------------------
// CAK key storage
// ---------------------------------------------------------------------------

void storeCak(const Config& config, const std::string& cakBytes);
// Validates and normalizes a complete DOT install payload received as JSON
// text (the Redfish CAKInstall body) and writes it to the key store.
void storeDotPayload(const Config& config, nlohmann::json payload);
void storeDotPayload(const Config& config, const std::string& jsonPayload);
void deleteCak(const Config& config);
