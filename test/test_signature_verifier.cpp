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

// Sign a SHA-384 digest with the given P-384 key and return the raw 96-byte
// R||S signature in the layout the verifier expects.
std::vector<uint8_t> signDigest(EVP_PKEY* key,
                                const std::vector<uint8_t>& digest)
{
    EvpPkeyCtxPtr signCtx(EVP_PKEY_CTX_new(key, nullptr), &EVP_PKEY_CTX_free);
    if (!signCtx)
    {
        throw std::runtime_error("Failed to allocate sign context");
    }
    if (EVP_PKEY_sign_init(signCtx.get()) <= 0)
    {
        throw std::runtime_error("EVP_PKEY_sign_init failed");
    }
    size_t derSigLen = 0;
    if (EVP_PKEY_sign(signCtx.get(), nullptr, &derSigLen, digest.data(),
                      digest.size()) <= 0)
    {
        throw std::runtime_error("Failed to size DER signature");
    }
    std::vector<uint8_t> derSignature(derSigLen);
    if (EVP_PKEY_sign(signCtx.get(), derSignature.data(), &derSigLen,
                      digest.data(), digest.size()) <= 0)
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

    std::vector<uint8_t> rawSignature;
    rawSignature.reserve(P384_ECDSA_SIGNATURE_LEN);
    rawSignature.insert(rawSignature.end(), rBytes.begin(), rBytes.end());
    rawSignature.insert(rawSignature.end(), sBytes.begin(), sBytes.end());
    return rawSignature;
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

    image.rawSignature = signDigest(key.get(), image.digest);

    std::copy(image.rawSignature.begin(), image.rawSignature.end(),
              fileData.begin() + image.metadata.nvSignatureOffset);

    std::ofstream output(filePath, std::ios::binary);
    output.write(reinterpret_cast<const char*>(fileData.data()),
                 fileData.size());
    output.close();

    return image;
}

// Build a correctly signed image whose single AP hash region ends exactly at
// the end of the file (imageOffset + offset + length == file size). The
// signature region is placed before the AP region so the AP bytes do not
// depend on the signature (which is computed last), avoiding a circular
// dependency.
SignedImage
    createSignedImageEndingAtBoundary(const std::filesystem::path& filePath)
{
    auto key = generateP384Key();

    constexpr uint32_t apLength = 64;
    const uint32_t signatureOffset = sizeof(Metadata);
    const uint32_t apOffset = signatureOffset + P384_ECDSA_SIGNATURE_LEN;
    const std::size_t fileSize = apOffset + apLength;

    SignedImage image;
    image.filePath = filePath;
    image.publicKeyHex = publicKeyToHex(key.get());
    image.metadata.imageOffset = apOffset;
    image.metadata.apFwImagesCnt = 1;
    image.metadata.hashTable[0].offset = 0;
    image.metadata.hashTable[0].length = apLength;
    image.metadata.nvPayloadSize = static_cast<uint16_t>(sizeof(Metadata));
    image.metadata.nvSignatureOffset = static_cast<uint16_t>(signatureOffset);

    std::vector<uint8_t> fileData(fileSize, 0);
    for (std::size_t i = 0; i < apLength; ++i)
    {
        fileData[apOffset + i] = static_cast<uint8_t>(i + 1U);
    }

    std::vector<uint8_t> apImage(fileData.begin() + apOffset,
                                 fileData.begin() + apOffset + apLength);
    auto apHash = calculateSHA384(apImage);
    std::memcpy(image.metadata.hashTable[0].hash, apHash.data(),
                SHA384_DIGEST_LENGTH);

    // Finalize metadata (now containing the AP hash) before computing the
    // payload digest so the signature covers the final metadata bytes.
    std::memcpy(fileData.data(), &image.metadata, sizeof(Metadata));

    std::vector<uint8_t> payload(
        fileData.begin(), fileData.begin() + image.metadata.nvPayloadSize);
    image.digest = calculateSHA384(payload);

    image.rawSignature = signDigest(key.get(), image.digest);
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

// ========== Bounds-checking regression tests (CVE: short-image OOB) ==========

namespace
{
void writeRawFile(const std::filesystem::path& path,
                  const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
} // namespace

TEST(SignatureVerifier, ZeroByteImageLeavesVerifierUninitialized)
{
    ScopedTempDir tempDir;
    auto path = tempDir.path() / "empty.bin";
    writeRawFile(path, {});

    SignatureVerifier verifier(path.string(), "ABCD");
    EXPECT_FALSE(verifier.isInitialized());
}

TEST(SignatureVerifier, OneByteImageLeavesVerifierUninitialized)
{
    // Reproduces the reported ASAN OOB: a 1-byte file used to be accepted
    // by loadImage() and then memcpy'd into a 950-byte Metadata struct.
    ScopedTempDir tempDir;
    auto path = tempDir.path() / "one-byte.bin";
    writeRawFile(path, {0xAB});

    SignatureVerifier verifier(path.string(), "ABCD");
    EXPECT_FALSE(verifier.isInitialized());
}

TEST(SignatureVerifier, ImageJustUnderMetadataSizeRejected)
{
    ScopedTempDir tempDir;
    auto path = tempDir.path() / "short.bin";
    writeRawFile(path, std::vector<uint8_t>(sizeof(Metadata) - 1, 0));

    SignatureVerifier verifier(path.string(), "ABCD");
    EXPECT_FALSE(verifier.isInitialized());
}

TEST(SignatureVerifier, NvPayloadSizeBeyondImageReturnsEmptyDigest)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.nvPayloadSize = 0xFFFFu; // larger than the file
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_TRUE(verifier.getDigest().empty());
    EXPECT_FALSE(verifier.verify());
}

TEST(SignatureVerifier, NvSignatureOffsetBeyondImageReturnsEmptySignature)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.nvSignatureOffset = 0xFFFFu; // larger than the file
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_TRUE(verifier.getSignature().empty());
    EXPECT_FALSE(verifier.verify());
}

TEST(SignatureVerifier, ApFwImagesCntAboveTableMaxIsClamped)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.apFwImagesCnt = 255; // hashTable is sized [8]
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    // The valid entry [0] still matches; entries [1..7] are zeroed so
    // their offset+length=0 satisfies the bounds check and the all-zero
    // hash mismatch returns false cleanly — no OOB on the hashTable[].
    EXPECT_FALSE(verifier.verifyApImageHash());
}

// ===== Integer-overflow regression tests (CVE: wrapped offset/length OOB) ====
//
// verifyApImageHash() computes offset = imageOffset + hashTable[i].offset and
// length = hashTable[i].length, then checks offset + length <= imageData.size()
// before slicing imageData[offset, offset + length). In the original code these
// were uint32_t, so attacker-controlled metadata could wrap the additions: the
// guard passed while the slice started far out of bounds. The fix widens the
// arithmetic to 64-bit (with a static_cast<uint64_t> on the first operand so
// the addition itself happens in 64-bit). Run tests 1 and 2 under ASan; the
// unfixed code performs an out-of-bounds read there.

// Test 1: the guard sum wraps via a large imageOffset.
// Unfixed: 0xFFFFFF00 + 0x180 wraps to 0x80, passes the guard, and the slice
// starts ~4 GiB out of bounds. Fixed: the 64-bit sum exceeds the file size, so
// verifyApImageHash() returns false with no out-of-bounds access.
TEST(SignatureVerifier, GuardSumWrapsViaLargeImageOffset)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.imageOffset = 0xFFFFFF00u;
    metadata.hashTable[0].offset = 0;
    metadata.hashTable[0].length = 0x180u;
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_FALSE(verifier.verifyApImageHash());
    EXPECT_FALSE(verifier.verify());
}

// Test 2: the guard sum wraps via a large length field instead of the offset.
// Unfixed: imageOffset + offset is in range but + 0xFFFFFFF0 wraps the guard.
// Fixed: the 64-bit sum exceeds the file size, so the function returns false.
TEST(SignatureVerifier, GuardSumWrapsViaLargeLength)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.imageOffset = sizeof(Metadata);
    metadata.hashTable[0].offset = 0x10u;
    metadata.hashTable[0].length = 0xFFFFFFF0u;
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_FALSE(verifier.verifyApImageHash());
    EXPECT_FALSE(verifier.verify());
}

// Test 3: a wrapped start offset that lands back in bounds at the wrong region.
// imageOffset + hashTable[0].offset == 0x80000000 + 0x80000040 wraps (in
// 32-bit) to 0x40; the unfixed code then hashes the in-bounds-but-wrong bytes
// [0x40, 0x60). We plant that region's real SHA-384 in hashTable[0].hash, so
// the unfixed code returns true. The fix widens the addition to 0x100000040,
// which fails the guard, so verifyApImageHash() returns false.
//
// This test specifically catches a fix that widens only the guard (the second
// addition) but leaves the first addition in 32-bit — i.e. one that forgot the
// static_cast<uint64_t>. The planted hash is essential: without it both fixed
// and unfixed code would return false for a different reason.
TEST(SignatureVerifier, WrappedStartOffsetPassesGuard)
{
    ScopedTempDir tempDir;
    auto image = createSignedImage(tempDir.path() / "signed-image.bin");

    // Read the raw file so we can hash the exact in-bounds region [0x40, 0x60)
    // that the unfixed code would slice once the start offset wraps to 0x40.
    std::ifstream in(image.filePath, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    std::vector<uint8_t> fileBytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    in.close();
    ASSERT_GE(fileBytes.size(), 0x60u);

    constexpr uint32_t wrappedStart = 0x40u;
    constexpr uint32_t wrappedLength = 0x20u;
    std::vector<uint8_t> wrappedRegion(fileBytes.begin() + wrappedStart,
                                       fileBytes.begin() + wrappedStart +
                                           wrappedLength);
    auto wrappedHash = calculateSHA384(wrappedRegion);

    std::fstream file(image.filePath,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    Metadata metadata{};
    file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    metadata.imageOffset = 0x80000000u;
    metadata.hashTable[0].offset = 0x80000040u;
    metadata.hashTable[0].length = wrappedLength;
    std::memcpy(metadata.hashTable[0].hash, wrappedHash.data(),
                SHA384_DIGEST_LENGTH);
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    file.close();

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_FALSE(verifier.verifyApImageHash());
}

// Test 4: a correctly signed image whose AP hash region ends exactly at the end
// of the file (imageOffset + offset + length == imageData.size()). This must
// still verify. It guards against an over-tightened fix that changes the guard
// from <= to <, which would reject this legitimate boundary case.
TEST(SignatureVerifier, ValidImageEndingAtBoundaryVerifies)
{
    ScopedTempDir tempDir;
    auto image =
        createSignedImageEndingAtBoundary(tempDir.path() / "boundary.bin");

    SignatureVerifier verifier(image.filePath.string(), image.publicKeyHex);
    ASSERT_TRUE(verifier.isInitialized());
    EXPECT_TRUE(verifier.verifyApImageHash());
    EXPECT_EQ(verifier.getDigest(), image.digest);
    EXPECT_TRUE(verifier.verify());
}
