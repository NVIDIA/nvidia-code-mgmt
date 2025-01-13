/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
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
#include <openssl/asn1.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <phosphor-logging/lg2.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

constexpr uint8_t P384_ECDSA_SIGNATURE_LEN = 96;

/*
 * Metadat 2.0 structure
 */
class Metadata
{
  public:
    uint16_t id;
    uint16_t revision;
    uint32_t imageOffset;
    uint32_t flashOffsetPartition;
    uint8_t apCfgKeyIdx;
    uint8_t apFwImagesCnt;
    uint8_t secVersion;
    uint8_t apStrapValue;
    uint32_t fwVersion;
    uint32_t buildDate;
    uint64_t keyRevocInfo;
    uint32_t spiOpcodeAllowList[8];

    struct SpiFlashRgnAttr
    {
        uint32_t field1;
        uint32_t field2;
        uint32_t field3;
    } spiFlashRgnAttr[8];

    struct HashTableEntry
    {
        uint32_t offset;
        uint32_t length;
        uint32_t apSpecificInfo[2];
        uint8_t hash[48];
    } hashTable[8];

    uint8_t verifyPubKey[96];
    uint32_t secVerInfo[4];
    uint8_t customize[64];
    uint8_t wpRegion;
    uint8_t apStrapSetting[39];
    uint8_t reserved1[24];
    uint8_t reserved2[4];
    uint32_t apSkuId;
    uint16_t pciVendorId;
    uint16_t pciDevicerId;
    uint16_t pciSubVendorId;
    uint16_t pciSubId;
    uint16_t nvPayloadSize;
    uint16_t nvSignatureOffset;
    uint16_t dotFieleOffset;
    uint8_t comVersionStr[16];
} __attribute__((packed));

/**
 *  @class SignatureVerifier
 *  @brief A class for verifying signatures of firmware images
 */
class SignatureVerifier
{
  public:
    SignatureVerifier(const std::string& filename,
                      const std::string& publicKey);
    ~SignatureVerifier() = default;

    bool verify();
    bool isInitialized()
    {
        return initialized;
    }

  private:
    bool initialized;
    Metadata metadata;
    std::string publicKey;
    std::vector<char> imageData;

    /**
     * @brief Load the firmware image from file
     * @param filename Path to the firmware image file
     * @return true if loading succeeds, false otherwise
     */
    bool loadImage(const std::string& filename);

    /**
     * @brief Parse the loaded image data and extract metadata
     */
    inline void parseMetaData()
    {
        std::memcpy(&metadata, imageData.data(), sizeof(Metadata));
    }

    /**
     * @brief Verify the hash of AP image
     * @return true if hash verification succeeds, false otherwise
     */
    bool verifyApImageHash() const;

    /**
     * @brief Verify the signature
     * @param digest Digest of the data to be verified
     * @param signature Signature to be verified
     * @return true if signature is valid, false otherwise
     */
    bool verifySignature(const std::vector<uint8_t>& digest,
                         const std::vector<uint8_t>& signature);

    /**
     * @brief Calculate the digest of given data
     * @return Calculated digest
     */
    std::vector<uint8_t> getDigest() const;

    /**
     * @brief Convert R/S format signature to DER format
     * @param nvSignature signature data in R/S format
     * @return Signature in DER format
     */
    std::vector<uint8_t> signatureToDer(const uint8_t* nvSignature);

    /**
     * @brief Extract signature from image data and call signatureToDer
     * @return Signature in DER format
     */
    std::vector<uint8_t> getSignature() const;
};
