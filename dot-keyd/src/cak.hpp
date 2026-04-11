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
void validateDotPayload(const nlohmann::json& payload);
std::string extractCakFromJson(const std::string& jsonPayload);

// ---------------------------------------------------------------------------
// CAK key storage
// ---------------------------------------------------------------------------

void storeCak(const Config& config, const std::string& cakBytes);
void deleteCak(const Config& config);
