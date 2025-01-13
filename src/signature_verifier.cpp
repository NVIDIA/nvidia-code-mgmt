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

#include "signature_verifier.hpp"

std::vector<uint8_t> calculateSHA384(const std::vector<uint8_t>& buffer)
{
    std::vector<uint8_t> hash(EVP_MAX_MD_SIZE);
    unsigned int hash_len;

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha384(), NULL);
    EVP_DigestUpdate(mdctx, buffer.data(), buffer.size());
    EVP_DigestFinal_ex(mdctx, hash.data(), &hash_len);
    EVP_MD_CTX_free(mdctx);

    hash.resize(hash_len);
    return hash;
}

std::vector<uint8_t> calculateECDSASignature(const std::vector<uint8_t>& r,
                                             const std::vector<uint8_t>& s)
{
    ECDSA_SIG* sig = ECDSA_SIG_new();
    BIGNUM* r_bn = BN_bin2bn(r.data(), r.size(), nullptr);
    BIGNUM* s_bn = BN_bin2bn(s.data(), s.size(), nullptr);
    ECDSA_SIG_set0(sig, r_bn, s_bn);

    int len = i2d_ECDSA_SIG(sig, nullptr);
    std::vector<uint8_t> der(len);
    uint8_t* p = der.data();
    i2d_ECDSA_SIG(sig, &p);

    ECDSA_SIG_free(sig);
    return der;
}

std::vector<uint8_t> signatureToDerFormat(const uint8_t* signature)
{
    // R/S values have equal length (48 bytes) in raw format for current signing
    // implementation
    uint8_t rsLen = P384_ECDSA_SIGNATURE_LEN / 2;
    std::vector<uint8_t> r(signature, signature + rsLen);
    std::vector<uint8_t> s(signature + rsLen,
                           signature + P384_ECDSA_SIGNATURE_LEN);
    return calculateECDSASignature(r, s);
}

SignatureVerifier::SignatureVerifier(const std::string& filename,
                                     const std::string& publicKey) :
    initialized(false),
    publicKey(publicKey)
{
    if (publicKey.empty())
    {
        lg2::error("Error: Empty Publicy Key");
        return;
    }

    if (loadImage(filename))
    {
        parseMetaData();
        initialized = true;
    }
    else
    {
        lg2::error("Failed to load image: {PATH}", "PATH", filename);
    }
}

bool SignatureVerifier::loadImage(const std::string& filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        lg2::error("Cannot open file: {PATH}", "PATH", filename);
        return false;
    }

    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    imageData.resize(fileSize);
    if (!file.read(imageData.data(), fileSize))
    {
        lg2::error("Failed to read file: {PATH}", "PATH", filename);
        return false;
    }

    lg2::info("Successfully loaded image {PATH}, size: {SZ}", "PATH", filename,
              "SZ", fileSize);
    return true;
}

bool SignatureVerifier::verifyApImageHash() const
{
    // Compare hash value of AP Images
    for (uint8_t i = 0; i < metadata.apFwImagesCnt; ++i)
    {
        uint32_t offset = metadata.imageOffset + metadata.hashTable[i].offset;
        uint32_t length = metadata.hashTable[i].length;

        if (offset + length <= imageData.size())
        {
            std::vector<uint8_t> image(imageData.begin() + offset,
                                       imageData.begin() + offset + length);

            uint8_t hash[EVP_MAX_MD_SIZE];
            uint32_t hashLen;

            EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
            EVP_DigestInit_ex(mdctx, EVP_sha384(), NULL);
            EVP_DigestUpdate(mdctx, image.data(), image.size());
            EVP_DigestFinal_ex(mdctx, hash, &hashLen);
            EVP_MD_CTX_free(mdctx);

            bool match = (memcmp(hash, metadata.hashTable[i].hash,
                                 SHA384_DIGEST_LENGTH) == 0);
            lg2::info("AP image [{IDX}] hash comparison result: {RES}", "IDX",
                      i, "RES", (match ? "Match" : "Mismatch"));
            if (!match)
            {
                return false;
            }
        }
        else
        {
            lg2::error(
                "Error: Offset and length out of buffer bounds for AP [{IDX}] Image",
                "IDX", i);
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> SignatureVerifier::getDigest() const
{
    const std::vector<uint8_t> payload(
        imageData.begin(), imageData.begin() + metadata.nvPayloadSize);

    return calculateSHA384(payload);
}

std::vector<uint8_t> SignatureVerifier::getSignature() const
{
    auto nvSignature = imageData.data() + metadata.nvSignatureOffset;
    return signatureToDerFormat(std::bit_cast<const uint8_t*>(nvSignature));
}

bool SignatureVerifier::verifySignature(const std::vector<uint8_t>& digest,
                                        const std::vector<uint8_t>& signature)
{
    int verificationErrorCode;
    BIGNUM* input = NULL;
    BIO* bo = NULL;
    EVP_PKEY* vkey = NULL;

    input = BN_new();
    std::unique_ptr<BIGNUM, decltype(&::BN_free)> ctxInputPtr{input,
                                                              &::BN_free};

    int inputLength = BN_hex2bn(&input, publicKey.c_str());
    inputLength = (inputLength + 1) / 2;

    std::vector<uint8_t> publicKeyBuffer(inputLength);
    BN_bn2bin(input, publicKeyBuffer.data());

    bo = BIO_new(BIO_s_mem());
    std::unique_ptr<BIO, decltype(&::BIO_free)> ctxBoPtr{bo, &::BIO_free};

    BIO_write(bo, publicKeyBuffer.data(), inputLength);
    vkey = PEM_read_bio_PUBKEY(bo, &vkey, nullptr, nullptr);
    std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)> ctxVkeyPtr{
        vkey, &::EVP_PKEY_free};

    // EVP_PKEY_CTX* verctx = EVP_PKEY_CTX_new(vkey, NULL);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&::EVP_PKEY_CTX_free)> verctx{
        EVP_PKEY_CTX_new(vkey, NULL), &::EVP_PKEY_CTX_free};
    if (!verctx)
    {
        lg2::error(
            "Verifying signature failed, cannot create a verify context");
        return false;
    }

    if (EVP_PKEY_verify_init(verctx.get()) <= 0)
    {
        lg2::error(
            "Verifying signature failed, cannot initialize a verify context");
        return false;
    }

    verificationErrorCode =
        EVP_PKEY_verify(verctx.get(), signature.data(), signature.size(),
                        digest.data(), digest.size());

    // ret == 1 indicates success
    if (verificationErrorCode != 1)
    {
        lg2::error(
            "Verifying signature failed, EVP_PKEY_verify error: {ERR} (expected 1)",
            "ERR", verificationErrorCode);
        return false;
    }
    return true;
}

bool SignatureVerifier::verify()
{
    if (!verifyApImageHash())
    {
        lg2::error("Verify AP Image Hash failed");
        return false;
    }

    // get the digest of payload
    auto digest = getDigest();

    // get the signature and turn into DER format
    auto signature = getSignature();

    if (verifySignature(digest, signature))
    {
        lg2::info("Signature verified successfully.");
    }
    else
    {
        lg2::error("Signature verification failed.");
        return false;
    }

    return true;
}
