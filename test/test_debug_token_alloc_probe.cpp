/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <sdbusplus/test/sdbus_mock.hpp>

#include "gmock/gmock.h"
#undef FAIL

#define private public
#define protected public
#include "../debug_token/update_debug_token.hpp"
#undef private
#undef protected

#include <endian.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

using ::testing::NiceMock;

namespace alloc_fail
{
thread_local bool enabled = false;
thread_local size_t failAt = 0;
thread_local size_t allocationCount = 0;

void reset()
{
    enabled = false;
    failAt = 0;
    allocationCount = 0;
}

void* allocate(std::size_t size)
{
    if (enabled && failAt != 0 && ++allocationCount == failAt)
    {
        throw std::bad_alloc();
    }

    if (void* ptr = std::malloc(size == 0 ? 1 : size))
    {
        return ptr;
    }
    throw std::bad_alloc();
}

class Guard
{
  public:
    explicit Guard(size_t nthAllocation)
    {
        enabled = true;
        failAt = nthAllocation;
        allocationCount = 0;
    }

    ~Guard()
    {
        reset();
    }
};

class ScopedPause
{
  public:
    ScopedPause() : wasEnabled(enabled)
    {
        enabled = false;
    }

    ~ScopedPause()
    {
        enabled = wasEnabled;
    }

  private:
    bool wasEnabled = false;
};
} // namespace alloc_fail

void* operator new(std::size_t size)
{
    return alloc_fail::allocate(size);
}

void* operator new[](std::size_t size)
{
    return alloc_fail::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return alloc_fail::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return alloc_fail::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
    std::free(ptr);
}

namespace popen_mock
{
struct Response
{
    std::string output;
    int retCode = 0;
    bool fail = false;
};

static std::string nextOutput;
static int nextReturnCode = 0;
static bool shouldFail = false;
static std::deque<Response> queuedResponses;

void reset()
{
    queuedResponses.clear();
    nextOutput.clear();
    nextReturnCode = 0;
    shouldFail = false;
}

void set(const std::string& output, int retCode = 0)
{
    reset();
    nextOutput = output;
    nextReturnCode = retCode;
}

void setFail()
{
    reset();
    shouldFail = true;
}

void enqueue(const std::string& output, int retCode = 0)
{
    queuedResponses.push_back({output, retCode, false});
}
} // namespace popen_mock

extern "C" FILE* __wrap_popen(const char*, const char* mode)
{
    if (!popen_mock::queuedResponses.empty())
    {
        auto response = popen_mock::queuedResponses.front();
        popen_mock::queuedResponses.pop_front();
        if (response.fail)
        {
            popen_mock::nextOutput.clear();
            popen_mock::nextReturnCode = 0;
            popen_mock::shouldFail = false;
            return nullptr;
        }
        popen_mock::nextOutput = response.output;
        popen_mock::nextReturnCode = response.retCode;
        popen_mock::shouldFail = false;
    }
    if (popen_mock::shouldFail)
    {
        return nullptr;
    }
    return fmemopen(const_cast<char*>(popen_mock::nextOutput.c_str()),
                    popen_mock::nextOutput.size(), mode);
}

extern "C" int __wrap_pclose(FILE* stream)
{
    if (stream)
    {
        fclose(stream);
    }
    return popen_mock::nextReturnCode;
}

template <typename Callback>
void swallowAll(Callback&& callback)
{
    try
    {
        callback();
    }
    catch (const std::bad_alloc&)
    {}
    catch (const std::exception&)
    {}
}

template <typename PrepareFn, typename InvokeFn>
size_t countAllocations(PrepareFn&& prepare, InvokeFn&& invoke)
{
    prepare();
    alloc_fail::enabled = true;
    alloc_fail::failAt = std::numeric_limits<size_t>::max();
    alloc_fail::allocationCount = 0;
    swallowAll(std::forward<InvokeFn>(invoke));
    size_t totalAllocations = alloc_fail::allocationCount;
    alloc_fail::reset();
    return totalAllocations;
}

template <typename PrepareFn, typename InvokeFn>
void runForkedAllocSweep(size_t firstFailure, size_t lastFailure,
                         PrepareFn&& prepare, InvokeFn&& invoke)
{
    for (size_t failIndex = firstFailure; failIndex <= lastFailure; ++failIndex)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            std::set_terminate([] { std::exit(0); });
            prepare();
            alloc_fail::Guard guard(failIndex);
            swallowAll(invoke);
            std::exit(0);
        }
        if (pid < 0)
        {
            continue;
        }

        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        {
        }
    }
}

template <typename PrepareFn, typename InvokeFn>
void runMeasuredForkedAllocSweep(size_t firstFailure, size_t lastFailure,
                                 PrepareFn&& prepare, InvokeFn&& invoke)
{
    size_t totalAllocations = countAllocations(prepare, invoke);
    if (totalAllocations == 0 || firstFailure > totalAllocations)
    {
        return;
    }

    runForkedAllocSweep(firstFailure, std::min(lastFailure, totalAllocations),
                        std::forward<PrepareFn>(prepare),
                        std::forward<InvokeFn>(invoke));
}

template <typename PrepareFn, typename InvokeFn>
void runProbeScenario(size_t firstFailure, size_t lastFailure,
                      PrepareFn&& prepare, InvokeFn&& invoke)
{
    prepare();
    swallowAll(invoke);
    runMeasuredForkedAllocSweep(firstFailure, lastFailure,
                                std::forward<PrepareFn>(prepare),
                                std::forward<InvokeFn>(invoke));
}

static std::vector<uint8_t> makeRawItem(uint16_t type,
                                        const std::vector<uint8_t>& data)
{
    debug_token::ItemHeader hdr{};
    hdr.type = htole16(type);
    hdr.size = htole16(static_cast<uint16_t>(data.size()));
    std::vector<uint8_t> result(sizeof(debug_token::ItemHeader) + data.size());
    std::memcpy(result.data(), &hdr, sizeof(hdr));
    if (!data.empty())
    {
        std::memcpy(result.data() + sizeof(hdr), data.data(), data.size());
    }
    return result;
}

static std::vector<uint8_t>
    makeRawStructure(uint16_t versionMajor, uint16_t versionMinor,
                     const std::vector<std::vector<uint8_t>>& items)
{
    size_t payloadSize = 0;
    for (const auto& item : items)
    {
        payloadSize += item.size();
    }

    debug_token::StructureHeader hdr{};
    std::memcpy(hdr.identifier, debug_token::TLV_IDENTIFIER, 4);
    hdr.versionMajor = htole16(versionMajor);
    hdr.versionMinor = htole16(versionMinor);
    hdr.size = htole32(static_cast<uint32_t>(payloadSize));

    std::vector<uint8_t> result(sizeof(debug_token::StructureHeader) +
                                payloadSize);
    std::memcpy(result.data(), &hdr, sizeof(hdr));
    size_t offset = sizeof(debug_token::StructureHeader);
    for (const auto& item : items)
    {
        std::memcpy(result.data() + offset, item.data(), item.size());
        offset += item.size();
    }
    return result;
}

template <typename T>
static std::vector<uint8_t> makeScalarValueBytes(T value)
{
    T encoded = value;
    if constexpr (sizeof(T) == sizeof(uint16_t))
    {
        encoded = static_cast<T>(htole16(static_cast<uint16_t>(value)));
    }
    else if constexpr (sizeof(T) == sizeof(uint32_t))
    {
        encoded = static_cast<T>(htole32(static_cast<uint32_t>(value)));
    }
    else if constexpr (sizeof(T) == sizeof(uint64_t))
    {
        encoded = static_cast<T>(htole64(static_cast<uint64_t>(value)));
    }

    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &encoded, sizeof(T));
    return bytes;
}

template <typename T>
static std::vector<uint8_t> makeVectorValueBytes(const std::vector<T>& values)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(values.size() * sizeof(T));
    for (const auto& value : values)
    {
        auto encoded = makeScalarValueBytes(value);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    return bytes;
}

static Token makePatternToken(size_t size, uint8_t seed)
{
    Token token(size, 0);
    for (size_t idx = 0; idx < size; ++idx)
    {
        token[idx] = static_cast<uint8_t>(seed + idx);
    }
    return token;
}

static std::string makeQueryResponse(size_t size,
                                     const std::vector<std::string>& overrides)
{
    std::string rx = "teid = 31\nRX:";
    for (size_t idx = 0; idx < size; ++idx)
    {
        if (idx < overrides.size() && !overrides[idx].empty())
        {
            rx += " " + overrides[idx];
        }
        else
        {
            rx += " 00";
        }
    }
    rx += "\n";
    return rx;
}

struct ProbeContext
{
    ProbeContext() :
        sdbusMockStorage(makeMock()), sdbusMock(*sdbusMockStorage),
        bus(makeBus(sdbusMockStorage.get())),
        udtStorage(makeUpdateDebugToken(bus)), udt(*udtStorage)
    {}

    static std::unique_ptr<NiceMock<sdbusplus::SdBusMock>> makeMock()
    {
        alloc_fail::ScopedPause pause;
        return std::make_unique<NiceMock<sdbusplus::SdBusMock>>();
    }

    static sdbusplus::bus_t makeBus(sdbusplus::SdBusMock* mock)
    {
        alloc_fail::ScopedPause pause;
        return sdbusplus::get_mocked_new(mock);
    }

    static std::unique_ptr<UpdateDebugToken>
        makeUpdateDebugToken(sdbusplus::bus_t& bus)
    {
        alloc_fail::ScopedPause pause;
        return std::make_unique<UpdateDebugToken>(bus);
    }

    std::unique_ptr<NiceMock<sdbusplus::SdBusMock>> sdbusMockStorage;
    NiceMock<sdbusplus::SdBusMock>& sdbusMock;
    sdbusplus::bus_t bus;
    std::unique_ptr<UpdateDebugToken> udtStorage;
    UpdateDebugToken& udt;
};

static std::pair<size_t, size_t> readFailRange()
{
    auto readValue = [](const char* name, size_t fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
        {
            return fallback;
        }
        return static_cast<size_t>(std::strtoull(value, nullptr, 10));
    };

    size_t first = readValue("DEBUG_TOKEN_ALLOC_FAIL_FIRST", 1);
    size_t last = readValue("DEBUG_TOKEN_ALLOC_FAIL_LAST", first);
    if (last < first)
    {
        last = first;
    }
    return {first, last};
}

static void runTlvProbe(size_t firstFailure, size_t lastFailure)
{
    const auto serialItem =
        makeRawItem(debug_token::types::Common::DeviceSerialNumber,
                    {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17});
    const auto nonceItem = makeRawItem(
        debug_token::types::Common::ChallengeNonce, {0xAA, 0xBB, 0xCC, 0xDD});
    const auto deviceTypeItem =
        makeRawItem(debug_token::types::Common::DeviceType, {0x01});
    const auto validStruct =
        makeRawStructure(2, 0, {deviceTypeItem, serialItem, nonceItem});
    auto badIdentifier = validStruct;
    std::memcpy(badIdentifier.data(), "BAD!", 4);
    const auto emptyStruct = makeRawStructure(2, 0, {});
    const auto duplicateStruct =
        makeRawStructure(2, 0, {deviceTypeItem, deviceTypeItem});
    auto sizeMismatch = validStruct;
    sizeMismatch.push_back(0x00);

    const auto rawU16 =
        makeRawItem(0x0001, makeScalarValueBytes<uint16_t>(0x1234));
    const auto rawU8 = makeRawItem(0x0001, {0x7F});
    const auto rawU32 =
        makeRawItem(0x0001, makeScalarValueBytes<uint32_t>(0x89ABCDEFu));
    const auto rawU64 = makeRawItem(
        0x0001, makeScalarValueBytes<uint64_t>(0x0102030405060708ULL));
    const auto rawVecU8 = makeRawItem(0x0001, {0x01, 0x02, 0x03, 0x04});
    const auto rawVecU16 = makeRawItem(
        0x0001, makeVectorValueBytes<uint16_t>({0x1111, 0x2222, 0x3333}));
    const auto rawVecU32 = makeRawItem(
        0x0001, makeVectorValueBytes<uint32_t>({0x01020304u, 0xAABBCCDDu}));
    const auto rawVecU64 = makeRawItem(
        0x0001, makeVectorValueBytes<uint64_t>(
                    {0x0102030405060708ULL, 0x1112131415161718ULL}));
    const auto rawEmptyVec = makeRawItem(0x0001, {});
    const std::vector<uint8_t> tooShortItem = {0x01};
    const auto wrongScalarSize = makeRawItem(0x0001, {0x01, 0x02, 0x03});
    const auto wrongVectorSize = makeRawItem(0x0001, {0x01, 0x02, 0x03});
    const auto wrongScalarSize64 =
        makeRawItem(0x0001, {0x01, 0x02, 0x03, 0x04});
    const auto wrongVectorSize64 =
        makeRawItem(0x0001, {0x01, 0x02, 0x03, 0x04, 0x05});
    debug_token::ItemHeader oversizedItemHeader{};
    oversizedItemHeader.type = htole16(0x0002);
    oversizedItemHeader.size = htole16(32);
    std::vector<uint8_t> oversizedItem(sizeof(debug_token::ItemHeader) + 2,
                                       0xAB);
    std::memcpy(oversizedItem.data(), &oversizedItemHeader,
                sizeof(oversizedItemHeader));
    const auto malformedInnerStruct = makeRawStructure(2, 0, {oversizedItem});
    const auto lateMalformedInnerStruct =
        makeRawStructure(2, 0, {deviceTypeItem, tooShortItem});

    auto prepare = [] { alloc_fail::reset(); };
    auto metadataInvoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawU16});
            (void)item.getType();
            (void)item.getValueSize();
            (void)item.getTotalSize();
            (void)item.getRawValue();
        });
    };
    auto scalarU8Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawU8});
            (void)item.getValue<uint8_t>();
        });
    };
    auto scalarU16Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawU16});
            (void)item.getValue<uint16_t>();
        });
    };
    auto scalarU32Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawU32});
            (void)item.getValue<uint32_t>();
        });
    };
    auto scalarU64Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawU64});
            (void)item.getValue<uint64_t>();
        });
    };
    auto vectorU8Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawVecU8});
            (void)item.getValue<std::vector<uint8_t>>();
        });
    };
    auto vectorU16Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawVecU16});
            (void)item.getValue<std::vector<uint16_t>>();
        });
    };
    auto vectorU32Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawVecU32});
            (void)item.getValue<std::vector<uint32_t>>();
        });
    };
    auto vectorU64Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawVecU64});
            (void)item.getValue<std::vector<uint64_t>>();
        });
    };
    auto emptyVectorU16Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawEmptyVec});
            (void)item.getValue<std::vector<uint16_t>>();
        });
    };
    auto emptyVectorU32Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawEmptyVec});
            (void)item.getValue<std::vector<uint32_t>>();
        });
    };
    auto emptyVectorU64Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{rawEmptyVec});
            (void)item.getValue<std::vector<uint64_t>>();
        });
    };
    auto malformedScalarU8Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{wrongScalarSize});
            (void)item.getValue<uint8_t>();
        });
    };
    auto malformedScalarU16Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{wrongScalarSize});
            (void)item.getValue<uint16_t>();
        });
    };
    auto malformedScalarU32Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{wrongScalarSize});
            (void)item.getValue<uint32_t>();
        });
    };
    auto malformedScalarU64Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{wrongScalarSize64});
            (void)item.getValue<uint64_t>();
        });
    };
    auto malformedVectorU16Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{wrongVectorSize});
            (void)item.getValue<std::vector<uint16_t>>();
        });
    };
    auto malformedVectorU32Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{wrongVectorSize});
            (void)item.getValue<std::vector<uint32_t>>();
        });
    };
    auto malformedVectorU64Invoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Item item(std::span{wrongVectorSize64});
            (void)item.getValue<std::vector<uint64_t>>();
        });
    };
    auto malformedItemShortInvoke = [&] {
        swallowAll([&] {
            (void)debug_token::tlv_decoder::Item(std::span{tooShortItem});
        });
    };
    auto malformedItemOversizedInvoke = [&] {
        swallowAll([&] {
            (void)debug_token::tlv_decoder::Item(std::span{oversizedItem});
        });
    };
    auto structureVersionInvoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Structure structure(validStruct);
            (void)structure.getVersion();
            (void)structure.getTypes();
        });
    };
    auto structureGetInvoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Structure structure(validStruct);
            (void)structure.get(debug_token::types::Common::DeviceSerialNumber)
                .getRawValue();
        });
    };
    auto structureMissingItemInvoke = [&] {
        swallowAll([&] {
            debug_token::tlv_decoder::Structure structure(validStruct);
            (void)structure.get(0xFFFF);
        });
    };
    auto emptyStructInvoke = [&] {
        swallowAll(
            [&] { (void)debug_token::tlv_decoder::Structure(emptyStruct); });
    };
    auto badIdentifierInvoke = [&] {
        swallowAll(
            [&] { (void)debug_token::tlv_decoder::Structure(badIdentifier); });
    };
    auto sizeMismatchInvoke = [&] {
        swallowAll(
            [&] { (void)debug_token::tlv_decoder::Structure(sizeMismatch); });
    };
    auto duplicateStructInvoke = [&] {
        swallowAll([&] {
            (void)debug_token::tlv_decoder::Structure(duplicateStruct);
        });
    };
    auto malformedInnerStructInvoke = [&] {
        swallowAll([&] {
            (void)debug_token::tlv_decoder::Structure(malformedInnerStruct);
        });
    };
    auto lateMalformedInnerStructInvoke = [&] {
        swallowAll([&] {
            (void)debug_token::tlv_decoder::Structure(lateMalformedInnerStruct);
        });
    };

    runProbeScenario(firstFailure, lastFailure, prepare, metadataInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, scalarU8Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, scalarU16Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, scalarU32Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, scalarU64Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, vectorU8Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, vectorU16Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, vectorU32Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, vectorU64Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, emptyVectorU16Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, emptyVectorU32Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, emptyVectorU64Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedScalarU8Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedScalarU16Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedScalarU32Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedScalarU64Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedVectorU16Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedVectorU32Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedVectorU64Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedItemShortInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedItemOversizedInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     structureVersionInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, structureGetInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     structureMissingItemInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, emptyStructInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, badIdentifierInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, sizeMismatchInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, duplicateStructInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     malformedInnerStructInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     lateMalformedInnerStructInvoke);
}

static void runTokenUtilityFilesProbe(size_t firstFailure, size_t lastFailure)
{
    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto validHeaderPath = tmpDir / "debug_token_probe_header_valid.bin";
    const auto invalidHeaderPath =
        tmpDir / "debug_token_probe_header_invalid.bin";
    const auto validTokenPath = tmpDir / "debug_token_probe_token_valid.bin";
    const auto mcuTokenPath = tmpDir / "debug_token_probe_token_mcu.bin";
    const auto truncatedHeaderPath =
        tmpDir / "debug_token_probe_token_truncated_header.bin";
    const auto truncatedBodyPath =
        tmpDir / "debug_token_probe_token_truncated_body.bin";

    auto writeHeaderFile = [](const std::filesystem::path& path, uint8_t type) {
        std::ofstream ofs(path, std::ios::binary);
        DebugTokenHeader hdr{};
        hdr.version = 2;
        hdr.type = type;
        hdr.numberOfRecords = 1;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    };

    auto writeTokenFile = [](const std::filesystem::path& path,
                             const char* identifier, uint16_t tokenSize,
                             size_t bytesToWrite) {
        std::ofstream ofs(path, std::ios::binary);
        TokenHeader hdr{};
        std::memcpy(hdr.identifier, identifier, 4);
        hdr.structSize = tokenSize;
        std::vector<uint8_t> data(bytesToWrite, 0);
        std::memcpy(data.data(), &hdr,
                    std::min<size_t>(sizeof(TokenHeader), data.size()));
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    };

    writeHeaderFile(validHeaderPath, FileTypeDebugToken);
    writeHeaderFile(invalidHeaderPath, 1);
    writeTokenFile(validTokenPath, "EDTI", 96, 96);
    writeTokenFile(mcuTokenPath, "MCDT", 96, 96);
    writeTokenFile(truncatedHeaderPath, "EDTI", 96, 2);
    writeTokenFile(truncatedBodyPath, "EDTI", 512, 64);

    auto prepare = [] { alloc_fail::reset(); };
    auto invoke = [&] {
        TokenUtility utility;
        swallowAll([&] {
            std::ifstream pkg(validHeaderPath, std::ios::binary);
            std::vector<uint8_t> headerData(sizeof(DebugTokenHeader));
            (void)utility.getDebugTokenHeader(headerData, pkg);
        });
        swallowAll([&] {
            std::ifstream pkg(invalidHeaderPath, std::ios::binary);
            std::vector<uint8_t> headerData(sizeof(DebugTokenHeader));
            (void)utility.getDebugTokenHeader(headerData, pkg);
        });
        swallowAll([&] {
            std::ifstream pkg(validTokenPath, std::ios::binary);
            std::vector<uint8_t> tokenData;
            std::vector<uint8_t> serialNumber;
            (void)utility.getNextDebugToken(pkg, 0, tokenData, serialNumber);
        });
        swallowAll([&] {
            std::ifstream pkg(mcuTokenPath, std::ios::binary);
            std::vector<uint8_t> tokenData;
            std::vector<uint8_t> serialNumber;
            (void)utility.getNextDebugToken(pkg, 0, tokenData, serialNumber);
        });
        swallowAll([&] {
            std::ifstream pkg(truncatedHeaderPath, std::ios::binary);
            std::vector<uint8_t> tokenData;
            std::vector<uint8_t> serialNumber;
            (void)utility.getNextDebugToken(pkg, 0, tokenData, serialNumber);
        });
        swallowAll([&] {
            std::ifstream pkg(truncatedBodyPath, std::ios::binary);
            std::vector<uint8_t> tokenData;
            std::vector<uint8_t> serialNumber;
            (void)utility.getNextDebugToken(pkg, 0, tokenData, serialNumber);
        });
        swallowAll([&] {
            std::ifstream pkg(validTokenPath, std::ios::binary);
            std::vector<uint8_t> tokenData{0xAA};
            std::vector<uint8_t> serialNumber;
            (void)utility.getNextDebugToken(pkg, 4096, tokenData, serialNumber);
        });
    };

    runMeasuredForkedAllocSweep(firstFailure, lastFailure, prepare, invoke);

    std::filesystem::remove(validHeaderPath);
    std::filesystem::remove(invalidHeaderPath);
    std::filesystem::remove(validTokenPath);
    std::filesystem::remove(mcuTokenPath);
    std::filesystem::remove(truncatedHeaderPath);
    std::filesystem::remove(truncatedBodyPath);
}

static void runTokenUtilityParseProbe(size_t firstFailure, size_t lastFailure)
{
    const auto serialItemA =
        makeRawItem(debug_token::types::Common::DeviceSerialNumber,
                    {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17});
    const auto serialItemB =
        makeRawItem(debug_token::types::Common::DeviceSerialNumber,
                    {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27});
    const auto typeItem =
        makeRawItem(debug_token::types::Common::DeviceType, {0x01});

    const auto validRecordA = makeRawStructure(2, 0, {serialItemA});
    const auto validRecordB = makeRawStructure(2, 0, {serialItemB});
    const auto missingSerialRecord = makeRawStructure(2, 0, {typeItem});

    debug_token::StructureHeader malformedHeader{};
    std::memcpy(malformedHeader.identifier, "BAD!", 4);
    malformedHeader.versionMajor = htole16(2);
    malformedHeader.versionMinor = htole16(0);
    malformedHeader.size = htole32(0);
    std::vector<uint8_t> malformedRecord(sizeof(debug_token::StructureHeader));
    std::memcpy(malformedRecord.data(), &malformedHeader,
                sizeof(malformedHeader));

    debug_token::StructureHeader oversizedHeader{};
    std::memcpy(oversizedHeader.identifier, debug_token::TLV_IDENTIFIER, 4);
    oversizedHeader.versionMajor = htole16(2);
    oversizedHeader.versionMinor = htole16(0);
    oversizedHeader.size = htole32(128);
    std::vector<uint8_t> oversizedRecord(sizeof(debug_token::StructureHeader) +
                                         4);
    std::memcpy(oversizedRecord.data(), &oversizedHeader,
                sizeof(oversizedHeader));

    std::vector<uint8_t> shortHeader(sizeof(debug_token::StructureHeader) - 1,
                                     0xAA);

    auto makeFullFile = [](uint16_t numberOfRecords,
                           const std::vector<std::vector<uint8_t>>& records) {
        DebugTokenHeader hdr{};
        hdr.version = 2;
        hdr.type = FileTypeDebugToken;
        hdr.numberOfRecords = numberOfRecords;
        hdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

        std::vector<uint8_t> fullFile(sizeof(DebugTokenHeader));
        std::memcpy(fullFile.data(), &hdr, sizeof(hdr));
        for (const auto& record : records)
        {
            fullFile.insert(fullFile.end(), record.begin(), record.end());
        }
        return std::pair{hdr, fullFile};
    };

    const auto [validHdr, validFile] =
        makeFullFile(2, {validRecordA, validRecordB});
    const auto [missingHdr, missingFile] =
        makeFullFile(1, {missingSerialRecord});
    const auto [mixedHdr, mixedFile] =
        makeFullFile(2, {validRecordA, malformedRecord});
    const auto [overflowHdr, overflowFile] = makeFullFile(3, {validRecordA});
    const auto [oversizedHdr, oversizedFile] =
        makeFullFile(1, {oversizedRecord});
    const auto [recoverHdr, recoverFile] =
        makeFullFile(3, {validRecordA, malformedRecord, validRecordB});
    const auto [malformedFirstHdr, malformedFirstFile] =
        makeFullFile(2, {malformedRecord, validRecordB});
    auto shortTrailingPair = makeFullFile(2, {validRecordA});
    auto shortTrailingHdr = shortTrailingPair.first;
    auto shortTrailingFile = shortTrailingPair.second;
    shortTrailingFile.push_back(0xAB);

    auto prepare = [] { alloc_fail::reset(); };
    auto singleRecordInvoke = [&] {
        size_t recordSize = 0;
        TokenMap tokens;
        swallowAll([&] {
            recordSize = 0;
            tokens.clear();
            (void)TokenUtility::parseSingleTlvRecord(validRecordA, 0, tokens,
                                                     recordSize);
        });
        swallowAll([&] {
            recordSize = 0;
            tokens.clear();
            (void)TokenUtility::parseSingleTlvRecord(missingSerialRecord, 0,
                                                     tokens, recordSize);
        });
        swallowAll([&] {
            recordSize = 0;
            tokens.clear();
            (void)TokenUtility::parseSingleTlvRecord(shortHeader, 0, tokens,
                                                     recordSize);
        });
        swallowAll([&] {
            recordSize = 0;
            tokens.clear();
            (void)TokenUtility::parseSingleTlvRecord(oversizedRecord, 0, tokens,
                                                     recordSize);
        });
    };
    auto parseFileInvoke = [&] {
        TokenMap tokens;
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(validFile, &validHdr, tokens);
        });
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(missingFile, &missingHdr,
                                               tokens);
        });
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(mixedFile, &mixedHdr, tokens);
        });
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(overflowFile, &overflowHdr,
                                               tokens);
        });
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(oversizedFile, &oversizedHdr,
                                               tokens);
        });
    };
    auto recoveryInvoke = [&] {
        TokenMap tokens;
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(recoverFile, &recoverHdr,
                                               tokens);
        });
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(malformedFirstFile,
                                               &malformedFirstHdr, tokens);
        });
    };
    auto trailingInvoke = [&] {
        TokenMap tokens;
        swallowAll([&] {
            tokens.clear();
            (void)TokenUtility::parseTlvTokens(shortTrailingFile,
                                               &shortTrailingHdr, tokens);
        });
    };

    runProbeScenario(firstFailure, lastFailure, prepare, singleRecordInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, parseFileInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, recoveryInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, trailingInvoke);
}

static void runUpdateCommandProbe(size_t firstFailure, size_t lastFailure)
{
    const Token token = makePatternToken(1024, 0x31);
    const std::string successInstall =
        "teid = 31\nRX: 00 00 16 47 00 01 0B 01 00 00\n";
    const std::string failedInstall =
        "teid = 31\nRX: 00 00 16 47 00 01 0B 01 01 01\n";
    const std::string failedErase =
        "teid = 31\nRX: 47 16 00 00 00 01 0C 01 00 01\n";
    const std::string bgSuccess =
        "teid = 31\nRX: 00 00 16 47 00 01 0A 01 00 00\n";

    std::vector<std::string> overridesV2(37);
    overridesV2[9] = "01";
    overridesV2[19] = "04";
    const std::string queryV2 = makeQueryResponse(37, overridesV2);

    std::vector<std::string> overridesV3(50);
    overridesV3[12] = "01";
    overridesV3[30] = "04";
    const std::string queryV3 = makeQueryResponse(50, overridesV3);

    std::vector<std::string> validV2(37, "00");
    validV2[9] = "01";
    validV2[19] = "04";
    std::vector<std::string> overflowV2(37, "00");
    overflowV2[9] = "FFFFFFFFFFFFFFFF";
    std::vector<std::string> validV3(50, "00");
    validV3[12] = "01";
    validV3[30] = "04";
    std::vector<std::string> overflowV3(50, "00");
    overflowV3[30] = "FFFFFFFFFFFFFFFF";

    const dbus::InterfaceMap availableInterfaces{
        {mctpEndpointIntfName,
         dbus::PropertyMap{
             {"EID", uint8_t{31}},
             {"SupportedMessageTypes",
              SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
        {mctpBindingIntfName,
         dbus::PropertyMap{
             {"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
        {mctpEndpointEnableIntfName,
         dbus::PropertyMap{{"Connectivity", std::string("Available")}}},
        {uuidEndpointIntfName,
         dbus::PropertyMap{{"UUID", std::string("uuid-a")}}},
    };

    const dbus::InterfaceMap unavailableInterfaces{
        {mctpEndpointIntfName,
         dbus::PropertyMap{
             {"EID", uint8_t{32}},
             {"SupportedMessageTypes", SupportedMessageTypes{mctpTypeSPDM}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
        {mctpEndpointEnableIntfName,
         dbus::PropertyMap{{"Connectivity", std::string("Unavailable")}}},
        {uuidEndpointIntfName,
         dbus::PropertyMap{{"UUID", std::string("uuid-b")}}},
    };
    const dbus::InterfaceMap missingConnectivityInterfaces{
        {mctpEndpointIntfName,
         dbus::PropertyMap{
             {"EID", uint8_t{33}},
             {"SupportedMessageTypes",
              SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")}}},
        {mctpBindingIntfName,
         dbus::PropertyMap{
             {"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB")}}},
        {mctpEndpointEnableIntfName, dbus::PropertyMap{}},
        {uuidEndpointIntfName,
         dbus::PropertyMap{{"UUID", std::string("uuid-c")}}},
    };
    const dbus::InterfaceMap noEnableInterfaces{
        {mctpEndpointIntfName,
         dbus::PropertyMap{
             {"EID", uint8_t{34}},
             {"SupportedMessageTypes",
              SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus")}}},
        {mctpBindingIntfName,
         dbus::PropertyMap{
             {"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus")}}},
        {uuidEndpointIntfName,
         dbus::PropertyMap{{"UUID", std::string("uuid-d")}}},
    };
    const dbus::InterfaceMap badTypeInterfaces{
        {mctpEndpointIntfName,
         dbus::PropertyMap{
             {"EID", std::string("bad-eid")},
             {"SupportedMessageTypes", std::string("bad-types")}}},
        {mctpBindingIntfName,
         dbus::PropertyMap{{"BindingType", static_cast<uint8_t>(1)}}},
        {mctpEndpointEnableIntfName,
         dbus::PropertyMap{{"Connectivity", static_cast<uint8_t>(1)}}},
        {uuidEndpointIntfName,
         dbus::PropertyMap{{"UUID", std::string("uuid-e")}}},
    };
    const dbus::InterfaceMap missingEndpointInterfaces{
        {uuidEndpointIntfName,
         dbus::PropertyMap{{"UUID", std::string("uuid-f")}}},
    };

    auto prepare = [] {
        popen_mock::reset();
        alloc_fail::reset();
    };

    auto installSuccessInvoke = [&] {
        ProbeContext ctx;
        ctx.udt.deviceNameMap.clear();
        ctx.udt.deviceNameMap[31] = std::string(192, 'D');

        swallowAll([&] {
            popen_mock::set(successInstall);
            (void)ctx.udt.installToken(31, token);
        });
    };
    auto installFailureInvoke = [&] {
        ProbeContext ctx;
        ctx.udt.deviceNameMap.clear();
        ctx.udt.deviceNameMap[31] = std::string(192, 'D');

        swallowAll([&] {
            popen_mock::reset();
            popen_mock::enqueue(failedInstall);
            popen_mock::enqueue(bgSuccess);
            (void)ctx.udt.installToken(31, token);
        });
    };
    auto eraseFailureInvoke = [&] {
        ProbeContext ctx;
        ctx.udt.deviceNameMap.clear();
        ctx.udt.deviceNameMap[31] = std::string(192, 'D');

        swallowAll([&] {
            popen_mock::reset();
            popen_mock::enqueue(failedErase);
            popen_mock::enqueue(bgSuccess);
            (void)ctx.udt.eraseToken(31);
        });
    };
    auto enableDisableInvoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            popen_mock::set("", 1);
            (void)ctx.udt.enableBackgroundCopy(31);
        });
        swallowAll([&] {
            popen_mock::set("", 1);
            (void)ctx.udt.disableBackgroundCopy(31);
        });
    };
    auto queryV1Invoke = [&] {
        ProbeContext ctx;
        swallowAll([&] {
            popen_mock::set(
                "teid = 31\nRX: 47 16 00 00 00 01 0F 01 00 01 02 1E 05 06 16 0B 04 01 01\n");
            (void)ctx.udt.queryDebugTokenV1(31);
        });
    };
    auto queryV2Invoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            popen_mock::set(queryV2);
            (void)ctx.udt.queryDebugTokenV2(31);
        });
    };
    auto queryV3Invoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            popen_mock::set(queryV3);
            (void)ctx.udt.queryDebugTokenV3(31);
        });
    };
    auto parseQueryV2Invoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            int tokenInstallStatus = 0;
            int installedTokenType = 0;
            (void)ctx.udt.parseQueryV2Response(validV2, tokenInstallStatus,
                                               installedTokenType, 31);
        });
        swallowAll([&] {
            int tokenInstallStatus = 0;
            int installedTokenType = 0;
            (void)ctx.udt.parseQueryV2Response(overflowV2, tokenInstallStatus,
                                               installedTokenType, 31);
        });
    };
    auto parseQueryV3Invoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            int tokenInstallStatus = 0;
            int installedTokenType = 0;
            (void)ctx.udt.parseQueryV3Response(validV3, tokenInstallStatus,
                                               installedTokenType, 31);
        });
        swallowAll([&] {
            int tokenInstallStatus = 0;
            int installedTokenType = 0;
            (void)ctx.udt.parseQueryV3Response(overflowV3, tokenInstallStatus,
                                               installedTokenType, 31);
        });
    };
    auto fetchEidInfoAvailableInvoke = [&] {
        ProbeContext ctx;
        swallowAll(
            [&] { (void)ctx.udt.fetchEidInfoFromObject(availableInterfaces); });
    };
    auto fetchEidInfoUnavailableInvoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            (void)ctx.udt.fetchEidInfoFromObject(unavailableInterfaces);
        });
    };
    auto fetchEidInfoMissingConnectivityInvoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            (void)ctx.udt.fetchEidInfoFromObject(missingConnectivityInterfaces);
        });
    };
    auto fetchEidInfoNoEnableInvoke = [&] {
        ProbeContext ctx;

        swallowAll(
            [&] { (void)ctx.udt.fetchEidInfoFromObject(noEnableInterfaces); });
    };
    auto fetchEidInfoBadTypesInvoke = [&] {
        ProbeContext ctx;

        swallowAll(
            [&] { (void)ctx.udt.fetchEidInfoFromObject(badTypeInterfaces); });
    };
    auto fetchEidInfoMissingEndpointInvoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            (void)ctx.udt.fetchEidInfoFromObject(missingEndpointInterfaces);
        });
    };
    auto supportChecksInvoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            (void)ctx.udt.checkSupportForSPDMandMCTPVDM(
                SupportedMessageTypes{mctpTypeSPDM, mctpTypeVDMIANA}, 31);
        });
        swallowAll([&] {
            (void)ctx.udt.checkSupportForSPDMandMCTPVDM(
                SupportedMessageTypes{mctpTypeSPDM}, 32);
        });
        swallowAll([&] {
            (void)ctx.udt.checkSupportForSPDMandMCTPVDM(
                SupportedMessageTypes{mctpTypeVDMIANA}, 33);
        });
    };
    auto getMessageInvoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            (void)ctx.udt.getMessage(
                OperationType::TokenInstall,
                static_cast<int>(InstallErrorCodes::InstallInternalError),
                std::string(160, 'X'));
        });
        swallowAll([&] {
            (void)ctx.udt.getMessage(
                OperationType::TokenErase,
                static_cast<int>(EraseErrorCodes::EraseInternalError),
                std::string(160, 'Y'));
        });
        swallowAll([&] {
            (void)ctx.udt.getMessage(
                OperationType::BackgroundCopy,
                static_cast<int>(
                    BackgroundCopyErrorCodes::BackgroundCopyFailed),
                std::string(160, 'Z'));
        });
        swallowAll([&] {
            (void)ctx.udt.getMessage(
                OperationType::Common,
                static_cast<int>(CommonErrorCodes::MCTPCommandInstallFailure),
                std::string(160, 'Q'));
        });
        swallowAll([&] {
            (void)ctx.udt.getMessage(OperationType::TokenQueryStatus, 0,
                                     std::string(96, 'W'));
        });
    };
    auto resourceErrorsInvoke = [&] {
        ProbeContext ctx;

        swallowAll([&] {
            ctx.udt.createMessageRegistryResourceErrors(
                resourceErrorsDetected, std::string(128, 'Y'),
                OperationType::Common,
                static_cast<int>(CommonErrorCodes::MCTPCommandInstallFailure),
                std::string(96, 'Z'));
        });
    };

    runProbeScenario(firstFailure, lastFailure, prepare, installSuccessInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, installFailureInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, eraseFailureInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, enableDisableInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, queryV1Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, queryV2Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, queryV3Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, parseQueryV2Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare, parseQueryV3Invoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     fetchEidInfoAvailableInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     fetchEidInfoUnavailableInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     fetchEidInfoMissingConnectivityInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     fetchEidInfoNoEnableInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     fetchEidInfoBadTypesInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare,
                     fetchEidInfoMissingEndpointInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, supportChecksInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, getMessageInvoke);
    runProbeScenario(firstFailure, lastFailure, prepare, resourceErrorsInvoke);
}

int main()
{
    auto [firstFailure, lastFailure] = readFailRange();
    const char* target = std::getenv("DEBUG_TOKEN_ALLOC_PROBE_TARGET");
    if (target == nullptr)
    {
        return 1;
    }

    std::string probeTarget(target);
    if (probeTarget == "tlv")
    {
        runTlvProbe(firstFailure, lastFailure);
        return 0;
    }
    if (probeTarget == "token_files")
    {
        runTokenUtilityFilesProbe(firstFailure, lastFailure);
        return 0;
    }
    if (probeTarget == "token_parse")
    {
        runTokenUtilityParseProbe(firstFailure, lastFailure);
        return 0;
    }
    if (probeTarget == "update_cmds")
    {
        runUpdateCommandProbe(firstFailure, lastFailure);
        return 0;
    }

    return 1;
}
