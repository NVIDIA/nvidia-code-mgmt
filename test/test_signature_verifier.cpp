/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <openssl/asn1.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define private public
#include "../src/signature_verifier.hpp"
#undef private

#include "../src/signature_verifier.cpp"

#include "gtest/gtest.h"

namespace
{

class ScopedTempDir
{
  public:
    ScopedTempDir()
    {
        std::array<char, 64> templ{};
        std::snprintf(templ.data(), templ.size(), "/tmp/sigverXXXXXX");
        char* created = mkdtemp(templ.data());
        if (created == nullptr)
        {
            throw std::runtime_error("Failed to create temporary directory");
        }
        dir = created;
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::filesystem::path path() const
    {
        return dir;
    }

  private:
    std::filesystem::path dir;
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpPkeyCtxPtr =
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using EcdsaSigPtr = std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)>;

struct SignedImage
{
    std::filesystem::path filePath;
    std::string publicKeyHex;
    std::vector<uint8_t> digest;
    std::vector<uint8_t> rawSignature;
    Metadata metadata{};
};

std::string toHex(const std::string& input)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size() * 2);
    for (unsigned char ch : input)
    {
        output.push_back(digits[ch >> 4]);
        output.push_back(digits[ch & 0x0F]);
    }
    return output;
}

std::vector<uint8_t> bnToFixedBytes(const BIGNUM* value, std::size_t size)
{
    int numBytes = BN_num_bytes(value);
    if (numBytes > static_cast<int>(size))
    {
        throw std::runtime_error("Unexpected bignum size");
    }

    std::vector<uint8_t> output(size, 0);
    BN_bn2bin(value, output.data() + (size - numBytes));
    return output;
}

EvpPkeyPtr generateP384Key()
{
    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr),
                      &EVP_PKEY_CTX_free);
    if (!ctx)
    {
        throw std::runtime_error("Failed to allocate EVP_PKEY_CTX");
    }
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0)
    {
        throw std::runtime_error("EVP_PKEY_keygen_init failed");
    }
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_secp384r1) <= 0)
    {
        throw std::runtime_error("Failed to set curve");
    }

    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &key) <= 0)
    {
        throw std::runtime_error("EVP_PKEY_keygen failed");
    }
    return EvpPkeyPtr(key, &EVP_PKEY_free);
}

std::string publicKeyToHex(const EVP_PKEY* publicKey)
{
    BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!bio)
    {
        throw std::runtime_error("Failed to allocate BIO");
    }
    if (PEM_write_bio_PUBKEY(bio.get(), publicKey) != 1)
    {
        throw std::runtime_error("Failed to write public key");
    }

    char* data = nullptr;
    long length = BIO_get_mem_data(bio.get(), &data);
    return toHex(std::string(data, static_cast<std::size_t>(length)));
}

SignedImage createSignedImage(const std::filesystem::path& filePath)
{
    auto key = generateP384Key();

    SignedImage image;
    image.filePath = filePath;
    image.publicKeyHex = publicKeyToHex(key.get());
    image.metadata.imageOffset = sizeof(Metadata);
    image.metadata.apFwImagesCnt = 1;
    image.metadata.hashTable[0].offset = 0;
    image.metadata.hashTable[0].length = 64;
    image.metadata.nvPayloadSize = sizeof(Metadata) + 64;
    image.metadata.nvSignatureOffset = image.metadata.nvPayloadSize;

    std::vector<uint8_t> fileData(
        image.metadata.nvSignatureOffset + P384_ECDSA_SIGNATURE_LEN, 0);
    std::memcpy(fileData.data(), &image.metadata, sizeof(Metadata));
    for (std::size_t i = 0; i < 64; ++i)
    {
        fileData[image.metadata.imageOffset + i] = static_cast<uint8_t>(i + 1U);
    }

    std::vector<uint8_t> apImage(fileData.begin() + image.metadata.imageOffset,
                                 fileData.begin() + image.metadata.imageOffset +
                                     image.metadata.hashTable[0].length);
    auto apHash = calculateSHA384(apImage);
    std::memcpy(image.metadata.hashTable[0].hash, apHash.data(),
                SHA384_DIGEST_LENGTH);
    std::memcpy(fileData.data(), &image.metadata, sizeof(Metadata));

    std::vector<uint8_t> payload(
        fileData.begin(), fileData.begin() + image.metadata.nvPayloadSize);
    image.digest = calculateSHA384(payload);

    EvpPkeyCtxPtr signCtx(EVP_PKEY_CTX_new(key.get(), nullptr),
                          &EVP_PKEY_CTX_free);
    if (!signCtx)
    {
        throw std::runtime_error("Failed to allocate sign context");
    }
    if (EVP_PKEY_sign_init(signCtx.get()) <= 0)
    {
        throw std::runtime_error("EVP_PKEY_sign_init failed");
    }
    size_t derSigLen = 0;
    if (EVP_PKEY_sign(signCtx.get(), nullptr, &derSigLen, image.digest.data(),
                      image.digest.size()) <= 0)
    {
        throw std::runtime_error("Failed to size DER signature");
    }
    std::vector<uint8_t> derSignature(derSigLen);
    if (EVP_PKEY_sign(signCtx.get(), derSignature.data(), &derSigLen,
                      image.digest.data(), image.digest.size()) <= 0)
    {
        throw std::runtime_error("Failed to sign digest");
    }
    derSignature.resize(derSigLen);

    const unsigned char* derData = derSignature.data();
    EcdsaSigPtr sig(d2i_ECDSA_SIG(nullptr, &derData, derSignature.size()),
                    &ECDSA_SIG_free);
    if (!sig)
    {
        throw std::runtime_error("Failed to decode DER signature");
    }

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig.get(), &r, &s);
    auto rBytes = bnToFixedBytes(r, P384_ECDSA_SIGNATURE_LEN / 2);
    auto sBytes = bnToFixedBytes(s, P384_ECDSA_SIGNATURE_LEN / 2);
    image.rawSignature.reserve(P384_ECDSA_SIGNATURE_LEN);
    image.rawSignature.insert(image.rawSignature.end(), rBytes.begin(),
                              rBytes.end());
    image.rawSignature.insert(image.rawSignature.end(), sBytes.begin(),
                              sBytes.end());

    std::copy(image.rawSignature.begin(), image.rawSignature.end(),
              fileData.begin() + image.metadata.nvSignatureOffset);

    std::ofstream output(filePath, std::ios::binary);
    output.write(reinterpret_cast<const char*>(fileData.data()),
                 fileData.size());
    output.close();

    return image;
}

} // namespace

// ========================== calculateSHA384 ==========================

TEST(SignatureVerifier, SHA384EmptyInput)
{
    std::vector<uint8_t> empty;
    auto hash = calculateSHA384(empty);
    EXPECT_EQ(hash.size(), 48u); // SHA-384 produces 48 bytes
}

TEST(SignatureVerifier, SHA384KnownInput)
{
    // SHA-384 of "abc" is a known value
    std::vector<uint8_t> input = {'a', 'b', 'c'};
    auto hash = calculateSHA384(input);
    EXPECT_EQ(hash.size(), 48u);
    // First byte of SHA-384("abc") = 0xcb
    EXPECT_EQ(hash[0], 0xcb);
}

TEST(SignatureVerifier, SHA384DifferentInputsDifferentHashes)
{
    std::vector<uint8_t> input1 = {0x01, 0x02, 0x03};
    std::vector<uint8_t> input2 = {0x04, 0x05, 0x06};
    auto hash1 = calculateSHA384(input1);
    auto hash2 = calculateSHA384(input2);
    EXPECT_NE(hash1, hash2);
}

TEST(SignatureVerifier, SHA384SameInputSameHash)
{
    std::vector<uint8_t> input = {0xDE, 0xAD, 0xBE, 0xEF};
    auto hash1 = calculateSHA384(input);
    auto hash2 = calculateSHA384(input);
    EXPECT_EQ(hash1, hash2);
}

TEST(SignatureVerifier, SHA384LargeInput)
{
    std::vector<uint8_t> input(4096, 0xFF);
    auto hash = calculateSHA384(input);
    EXPECT_EQ(hash.size(), 48u);
}

// ========================== calculateECDSASignature ==========================

TEST(SignatureVerifier, ECDSASignatureValidRS)
{
    // Create dummy R and S values (48 bytes each for P-384)
    std::vector<uint8_t> r(48, 0x01);
    std::vector<uint8_t> s(48, 0x02);
    auto der = calculateECDSASignature(r, s);
    EXPECT_GT(der.size(), 0u);
    // DER encoding starts with SEQUENCE tag (0x30)
    EXPECT_EQ(der[0], 0x30);
}

TEST(SignatureVerifier, ECDSASignatureSmallRS)
{
    std::vector<uint8_t> r = {0x01};
    std::vector<uint8_t> s = {0x01};
    auto der = calculateECDSASignature(r, s);
    EXPECT_GT(der.size(), 0u);
}

// ========================== signatureToDerFormat ==========================

TEST(SignatureVerifier, SignatureToDerFormatValid)
{
    // Create a 96-byte raw signature (48 bytes R + 48 bytes S)
    std::vector<uint8_t> rawSig(P384_ECDSA_SIGNATURE_LEN, 0x42);
    auto der = signatureToDerFormat(rawSig.data());
    EXPECT_GT(der.size(), 0u);
    EXPECT_EQ(der[0], 0x30); // DER SEQUENCE tag
}

TEST(SignatureVerifier, EmptyPublicKeyLeavesVerifierUninitialized)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    SignatureVerifier verifier(image.filePath.string(), "");
    EXPECT_FALSE(verifier.isInitialized());
}

TEST(SignatureVerifier, MissingFileLeavesVerifierUninitialized)
{
    SignatureVerifier verifier("/tmp/does-not-exist.bin", "ABCD");
    EXPECT_FALSE(verifier.isInitialized());
}

TEST(SignatureVerifier, SignedImageVerifiesSuccessfully)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_TRUE(verifier.verifyApImageHash());

    auto digest = verifier.getDigest();
    EXPECT_EQ(digest, image.digest);

    auto derSignature = verifier.getSignature();
    EXPECT_GT(derSignature.size(), 0u);
    EXPECT_EQ(derSignature[0], 0x30);
    EXPECT_TRUE(verifier.verifySignature(digest, derSignature));
    EXPECT_TRUE(verifier.verify());
}

TEST(SignatureVerifier, HashMismatchFailsVerification)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.hashTable[0].hash[0] ^= 0xFF;
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_FALSE(verifier.verifyApImageHash());
    EXPECT_FALSE(verifier.verify());
}

TEST(SignatureVerifier, OutOfBoundsHashEntryFailsVerification)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.hashTable[0].offset = 0xFFFF;
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_FALSE(verifier.verifyApImageHash());
}

TEST(SignatureVerifier, WrongPublicKeyFailsSignatureVerification)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");
    auto wrongKeyPair = generateP384Key();
    auto wrongKey = publicKeyToHex(wrongKeyPair.get());

    SignatureVerifier verifier(image.filePath.string(), wrongKey);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_TRUE(verifier.verifyApImageHash());
    EXPECT_FALSE(verifier.verifySignature(verifier.getDigest(),
                                          verifier.getSignature()));
    EXPECT_FALSE(verifier.verify());
}
