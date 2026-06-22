/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#define private public
#define protected public
#include "../debug_token/update_debug_token.hpp"
#undef private
#undef protected

#include "../debug_token/nsm_debug_token.cpp"

namespace
{

using ResponseFn =
    std::function<int(sd_bus_message* request, sd_bus_message** reply)>;

using SubTreeEntry = std::tuple<std::string, std::string, std::string>;
using AsyncMethodArg =
    std::variant<std::monostate, std::string, std::vector<uint8_t>>;

constexpr const char* matchingSerial = "0x011E020E160A1017";
constexpr std::array<uint8_t, 8> matchingSerialBytes = {0x01, 0x1E, 0x02, 0x0E,
                                                        0x16, 0x0A, 0x10, 0x17};

namespace preload_state
{
struct PendingSignal
{
    std::string objectPath;
    std::string status;
    std::string propertyName = "Status";
};

static bool initialized = false;
static std::deque<ResponseFn> responses{};
static std::deque<PendingSignal> pendingSignals{};
static sd_bus_message_handler_t matchCallback = nullptr;
static void* matchUserdata = nullptr;
static sd_bus_slot* matchSlot = nullptr;
static sd_bus_slot* currentSlot = nullptr;
static void* slotUserdata = nullptr;
static int trackedFd = -1;
static int failMemfdCreateRemaining = 0;
static int shortWriteRemaining = 0;
using MainFn = int (*)(int, char**, char**);
static MainFn realMain = nullptr;
} // namespace preload_state

std::filesystem::path makeTempArtifactPath(const std::string& prefix,
                                           const std::string& suffix)
{
    return std::filesystem::temp_directory_path() /
           (prefix + "-" + std::to_string(getpid()) + "-" +
            std::to_string(::random()) + suffix);
}

void appendMarker(const char* event)
{
    if (const char* marker = std::getenv("DEBUG_TOKEN_PRELOAD_MARKER");
        marker != nullptr)
    {
        std::ofstream stream(marker, std::ios::app);
        if (stream.is_open())
        {
            stream << event << '\n';
        }
    }
}

void resetRuntimeState()
{
    preload_state::responses.clear();
    preload_state::pendingSignals.clear();
    preload_state::trackedFd = -1;
    preload_state::failMemfdCreateRemaining = 0;
    preload_state::shortWriteRemaining = 0;
}

void buildScenario(const std::string& scenario);
void pushError(int rc);
void pushGetSubTreeEntries(const std::vector<SubTreeEntry>& entries);
void pushGetSubTreeOneEndpoint(const char* path, const char* service,
                               const char* iface);
void pushVariantString(const char* value);
void pushVariantErrorTuple(uint16_t errorCode, const char* errorMessage);
void pushVariantTokenStatus(const char* tokenType, const char* tokenStatus,
                            const char* additionalInfo, uint32_t timeLeft);
void pushObjectPath(const char* path);
void queueSignal(const std::string& objectPath, const std::string& status,
                 const std::string& propertyName);

template <typename Fn>
Fn loadSymbol(const char* name)
{
    auto* sym = dlsym(RTLD_NEXT, name);
    return reinterpret_cast<Fn>(sym);
}

sd_bus* getBusFromMsg(sd_bus_message* msg)
{
    return sd_bus_message_get_bus(msg);
}

std::vector<uint8_t> makeRawItem(uint16_t type,
                                 const std::vector<uint8_t>& data)
{
    debug_token::ItemHeader hdr{};
    hdr.type = htole16(type);
    hdr.size = htole16(static_cast<uint16_t>(data.size()));

    std::vector<uint8_t> result(sizeof(debug_token::ItemHeader) + data.size());
    std::memcpy(result.data(), &hdr, sizeof(debug_token::ItemHeader));
    std::memcpy(result.data() + sizeof(debug_token::ItemHeader), data.data(),
                data.size());
    return result;
}

std::vector<uint8_t>
    makeRawStructure(uint16_t vMajor, uint16_t vMinor,
                     const std::vector<std::vector<uint8_t>>& items)
{
    size_t payloadSize = 0;
    for (const auto& item : items)
    {
        payloadSize += item.size();
    }

    debug_token::StructureHeader hdr{};
    std::memcpy(hdr.identifier, debug_token::TLV_IDENTIFIER, 4);
    hdr.versionMajor = htole16(vMajor);
    hdr.versionMinor = htole16(vMinor);
    hdr.size = htole32(static_cast<uint32_t>(payloadSize));

    std::vector<uint8_t> result(sizeof(debug_token::StructureHeader) +
                                payloadSize);
    std::memcpy(result.data(), &hdr, sizeof(debug_token::StructureHeader));
    size_t offset = sizeof(debug_token::StructureHeader);
    for (const auto& item : items)
    {
        std::memcpy(result.data() + offset, item.data(), item.size());
        offset += item.size();
    }
    return result;
}

std::filesystem::path makeLegacyTokenImage(
    const std::string& identifier = "EDTI",
    const std::vector<uint8_t>& serial = std::vector<uint8_t>(
        matchingSerialBytes.begin(), matchingSerialBytes.end()),
    uint8_t fileType = FileTypeDebugToken)
{
    auto tmpPath = makeTempArtifactPath("debug-token-binary-preload", ".bin");
    std::ofstream file(tmpPath, std::ios::binary);

    TokenHeader tokenHdr{};
    std::memcpy(tokenHdr.identifier, identifier.data(),
                std::min(identifier.size(), sizeof(tokenHdr.identifier)));
    tokenHdr.versionMinor = 0;
    tokenHdr.versionMajor = 1;
    tokenHdr.structSize =
        static_cast<uint16_t>(sizeof(TokenHeader) + serial.size() + 32);
    tokenHdr.tokenType = 1;

    std::vector<uint8_t> tokenData(tokenHdr.structSize, 0);
    std::memcpy(tokenData.data(), &tokenHdr, sizeof(TokenHeader));
    std::memcpy(tokenData.data() + sizeof(TokenHeader), serial.data(),
                serial.size());

    DebugTokenHeader fileHdr{};
    fileHdr.version = 1;
    fileHdr.type = fileType;
    fileHdr.numberOfRecords = 1;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
    fileHdr.fileSize =
        static_cast<uint32_t>(sizeof(DebugTokenHeader) + tokenData.size());

    file.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
    file.write(reinterpret_cast<const char*>(tokenData.data()),
               tokenData.size());
    return tmpPath;
}

std::filesystem::path makeTruncatedLegacyTokenImage()
{
    auto tmpPath =
        makeTempArtifactPath("debug-token-binary-preload-truncated", ".bin");
    std::ofstream file(tmpPath, std::ios::binary);

    TokenHeader tokenHdr{};
    std::memcpy(tokenHdr.identifier, "EDTI", 4);
    tokenHdr.versionMinor = 0;
    tokenHdr.versionMajor = 1;
    tokenHdr.structSize = 256;
    tokenHdr.tokenType = 1;

    DebugTokenHeader fileHdr{};
    fileHdr.version = 1;
    fileHdr.type = FileTypeDebugToken;
    fileHdr.numberOfRecords = 1;
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
    fileHdr.fileSize =
        static_cast<uint32_t>(sizeof(DebugTokenHeader) + sizeof(TokenHeader));

    file.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
    file.write(reinterpret_cast<const char*>(&tokenHdr), sizeof(tokenHdr));
    return tmpPath;
}

std::filesystem::path
    makeTlvTokenImage(const std::vector<std::vector<uint8_t>>& records)
{
    auto tmpPath = makeTempArtifactPath("debug-token-binary-preload", ".tlv");
    std::ofstream file(tmpPath, std::ios::binary);

    DebugTokenHeader fileHdr{};
    fileHdr.version = 2;
    fileHdr.type = FileTypeDebugToken;
    fileHdr.numberOfRecords = static_cast<uint16_t>(records.size());
    fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);

    size_t totalSize = sizeof(DebugTokenHeader);
    for (const auto& record : records)
    {
        totalSize += record.size();
    }
    fileHdr.fileSize = static_cast<uint32_t>(totalSize);

    file.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
    for (const auto& record : records)
    {
        file.write(reinterpret_cast<const char*>(record.data()), record.size());
    }
    return tmpPath;
}

std::vector<uint8_t> makeSerialRecord(const std::vector<uint8_t>& serialBytes)
{
    auto serialItem = makeRawItem(
        debug_token::types::Common::DeviceSerialNumber, serialBytes);
    auto typeItem = makeRawItem(debug_token::types::Common::DeviceType, {0x01});
    return makeRawStructure(2, 0, {typeItem, serialItem});
}

std::vector<uint8_t> makeMissingSerialRecord()
{
    auto typeItem = makeRawItem(debug_token::types::Common::DeviceType, {0x01});
    return makeRawStructure(2, 0, {typeItem});
}

std::vector<uint8_t> makeBadIdentifierRecord(uint32_t payloadSize = 1)
{
    debug_token::StructureHeader hdr{};
    std::memcpy(hdr.identifier, "BAD!", 4);
    hdr.versionMajor = htole16(2);
    hdr.versionMinor = htole16(0);
    hdr.size = htole32(payloadSize);

    std::vector<uint8_t> record(
        sizeof(debug_token::StructureHeader) + payloadSize, 0);
    std::memcpy(record.data(), &hdr, sizeof(hdr));
    return record;
}

struct ExecutableInfo
{
    uintptr_t baseAddress = 0;
    std::filesystem::path path{};
};

int captureExecutableInfo(dl_phdr_info* info, size_t, void* data)
{
    auto& executableInfo = *static_cast<ExecutableInfo*>(data);
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0')
    {
        executableInfo.baseAddress = static_cast<uintptr_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}

std::optional<ExecutableInfo> getExecutableInfo()
{
    ExecutableInfo info{};
    dl_iterate_phdr(captureExecutableInfo, &info);
    if (info.baseAddress == 0 && !std::filesystem::exists("/proc/self/exe"))
    {
        return std::nullopt;
    }

    std::array<char, 4096> exePath{};
    const auto length =
        readlink("/proc/self/exe", exePath.data(), exePath.size() - 1);
    if (length <= 0)
    {
        return std::nullopt;
    }
    info.path = std::string(exePath.data(), static_cast<size_t>(length));
    return info;
}

std::optional<uintptr_t>
    resolveExecutableSymbolAddress(const std::string_view symbolName)
{
    static std::optional<ExecutableInfo> executableInfo = getExecutableInfo();
    static std::unordered_map<std::string, uintptr_t> cache;

    if (!executableInfo)
    {
        return std::nullopt;
    }

    if (auto found = cache.find(std::string(symbolName)); found != cache.end())
    {
        return found->second;
    }

    std::ifstream elf(executableInfo->path, std::ios::binary | std::ios::ate);
    if (!elf.is_open())
    {
        return std::nullopt;
    }

    const auto size = static_cast<size_t>(elf.tellg());
    elf.seekg(0, std::ios::beg);
    std::vector<char> image(size);
    elf.read(image.data(), static_cast<std::streamsize>(image.size()));

    if (image.size() < sizeof(Elf64_Ehdr))
    {
        return std::nullopt;
    }

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(image.data());
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64)
    {
        return std::nullopt;
    }

    if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr))
    {
        return std::nullopt;
    }

    const auto* shdrs =
        reinterpret_cast<const Elf64_Shdr*>(image.data() + ehdr->e_shoff);

    for (size_t i = 0; i < ehdr->e_shnum; ++i)
    {
        const auto& section = shdrs[i];
        if (section.sh_type != SHT_SYMTAB)
        {
            continue;
        }
        if (section.sh_link >= ehdr->e_shnum)
        {
            continue;
        }

        const auto& strSection = shdrs[section.sh_link];
        const auto* strings = image.data() + strSection.sh_offset;
        const auto* symbols = reinterpret_cast<const Elf64_Sym*>(
            image.data() + section.sh_offset);
        const size_t symbolCount = section.sh_size / sizeof(Elf64_Sym);

        for (size_t symbolIndex = 0; symbolIndex < symbolCount; ++symbolIndex)
        {
            const auto& symbol = symbols[symbolIndex];
            if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC ||
                symbol.st_name == 0)
            {
                continue;
            }

            const std::string_view currentName(strings + symbol.st_name);
            if (currentName != symbolName)
            {
                continue;
            }

            const uintptr_t address = executableInfo->baseAddress +
                                      static_cast<uintptr_t>(symbol.st_value);
            cache.emplace(std::string(symbolName), address);
            return address;
        }
    }

    return std::nullopt;
}

template <typename Fn>
std::optional<Fn> resolveExecutableFunction(const std::string_view symbolName)
{
    if (const auto address = resolveExecutableSymbolAddress(symbolName))
    {
        return reinterpret_cast<Fn>(*address);
    }
    return std::nullopt;
}

struct DirectProbeApi
{
    using UpdateTokenMapFn = int (*)(UpdateDebugToken*, const std::string&,
                                     TokenMap&);
    using InstallDebugTokenFn = DebugTokenInstallStatus (*)(UpdateDebugToken*,
                                                            const std::string&);
    using EraseDebugTokenFn = int (*)(UpdateDebugToken*);
    using EnumerateNsmEndpointsFn = int (*)(UpdateDebugToken*, NSMEndpoints&);
    using MakeDebugTokenMethodCallFn = std::string (*)(UpdateDebugToken*,
                                                       const std::string&,
                                                       const std::string&,
                                                       const AsyncMethodArg&);
    using LogAsyncErrorFn = void (*)(UpdateDebugToken*, const std::string&,
                                     const std::string&, const std::string&);
    using HandleAsyncCallFn = std::string (*)(UpdateDebugToken*,
                                              const std::string&,
                                              const std::string&,
                                              const AsyncMethodArg&);
    using GetTokenStatusFn = std::string (*)(UpdateDebugToken*,
                                             const std::string&);
    using NsmTokenInstallFn = int (*)(UpdateDebugToken*, TokenMap&);
    using HandleAsyncCallInstallV2Fn = std::string (*)(UpdateDebugToken*,
                                                       const std::string&, int);
    using HandleAsyncCallEraseV2Fn = std::string (*)(UpdateDebugToken*,
                                                     const std::string&);

    UpdateTokenMapFn updateTokenMap = nullptr;
    InstallDebugTokenFn installDebugToken = nullptr;
    EraseDebugTokenFn eraseDebugToken = nullptr;
    EnumerateNsmEndpointsFn enumerateNsmEndpoints = nullptr;
    EnumerateNsmEndpointsFn enumerateNsmEndpointsV2 = nullptr;
    MakeDebugTokenMethodCallFn makeDebugTokenMethodCall = nullptr;
    LogAsyncErrorFn logAsyncError = nullptr;
    HandleAsyncCallFn handleAsyncCall = nullptr;
    GetTokenStatusFn getTokenStatus = nullptr;
    EraseDebugTokenFn nsmTokenErase = nullptr;
    NsmTokenInstallFn nsmTokenInstall = nullptr;
    HandleAsyncCallInstallV2Fn handleAsyncCallInstallV2 = nullptr;
    HandleAsyncCallEraseV2Fn handleAsyncCallEraseV2 = nullptr;
    EraseDebugTokenFn nsmTokenEraseV2 = nullptr;
    NsmTokenInstallFn nsmTokenInstallV2 = nullptr;
};

std::optional<DirectProbeApi> resolveDirectProbeApi()
{
    DirectProbeApi api{};
    api.updateTokenMap =
        resolveExecutableFunction<DirectProbeApi::UpdateTokenMapFn>(
            "_ZN16UpdateDebugToken14updateTokenMapERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERSt8multimapIS5_St6vectorIhSaIhEESt4lessIS5_ESaISt4pairIS6_SB_EEE")
            .value_or(nullptr);
    api.installDebugToken =
        resolveExecutableFunction<DirectProbeApi::InstallDebugTokenFn>(
            "_ZN16UpdateDebugToken17installDebugTokenERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
            .value_or(nullptr);
    api.eraseDebugToken =
        resolveExecutableFunction<DirectProbeApi::EraseDebugTokenFn>(
            "_ZN16UpdateDebugToken15eraseDebugTokenEv")
            .value_or(nullptr);
    api.enumerateNsmEndpoints =
        resolveExecutableFunction<DirectProbeApi::EnumerateNsmEndpointsFn>(
            "_ZN16UpdateDebugToken31enumerateNsmDebugTokenEndpointsERSt6vectorINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESaIS6_EE")
            .value_or(nullptr);
    api.enumerateNsmEndpointsV2 =
        resolveExecutableFunction<DirectProbeApi::EnumerateNsmEndpointsFn>(
            "_ZN16UpdateDebugToken33enumerateNsmDebugTokenEndpointsV2ERSt6vectorINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESaIS6_EE")
            .value_or(nullptr);
    api.makeDebugTokenMethodCall =
        resolveExecutableFunction<DirectProbeApi::MakeDebugTokenMethodCallFn>(
            "_ZN16UpdateDebugToken24makeDebugTokenMethodCallERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_RKSt7variantIJSt9monostateS5_St6vectorIhSaIhEEEE")
            .value_or(nullptr);
    api.logAsyncError =
        resolveExecutableFunction<DirectProbeApi::LogAsyncErrorFn>(
            "_ZN16UpdateDebugToken13logAsyncErrorERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_S7_")
            .value_or(nullptr);
    api.handleAsyncCall =
        resolveExecutableFunction<DirectProbeApi::HandleAsyncCallFn>(
            "_ZN16UpdateDebugToken15handleAsyncCallERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_RKSt7variantIJSt9monostateS5_St6vectorIhSaIhEEEE")
            .value_or(nullptr);
    api.getTokenStatus =
        resolveExecutableFunction<DirectProbeApi::GetTokenStatusFn>(
            "_ZN16UpdateDebugToken14getTokenStatusERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
            .value_or(nullptr);
    api.nsmTokenErase =
        resolveExecutableFunction<DirectProbeApi::EraseDebugTokenFn>(
            "_ZN16UpdateDebugToken13nsmTokenEraseEv")
            .value_or(nullptr);
    api.nsmTokenInstall =
        resolveExecutableFunction<DirectProbeApi::NsmTokenInstallFn>(
            "_ZN16UpdateDebugToken15nsmTokenInstallERSt8multimapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt6vectorIhSaIhEESt4lessIS6_ESaISt4pairIKS6_S9_EEE")
            .value_or(nullptr);
    api.handleAsyncCallInstallV2 =
        resolveExecutableFunction<DirectProbeApi::HandleAsyncCallInstallV2Fn>(
            "_ZN16UpdateDebugToken24handleAsyncCallInstallV2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEi")
            .value_or(nullptr);
    api.handleAsyncCallEraseV2 =
        resolveExecutableFunction<DirectProbeApi::HandleAsyncCallEraseV2Fn>(
            "_ZN16UpdateDebugToken22handleAsyncCallEraseV2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
            .value_or(nullptr);
    api.nsmTokenEraseV2 =
        resolveExecutableFunction<DirectProbeApi::EraseDebugTokenFn>(
            "_ZN16UpdateDebugToken15nsmTokenEraseV2Ev")
            .value_or(nullptr);
    api.nsmTokenInstallV2 =
        resolveExecutableFunction<DirectProbeApi::NsmTokenInstallFn>(
            "_ZN16UpdateDebugToken17nsmTokenInstallV2ERSt8multimapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt6vectorIhSaIhEESt4lessIS6_ESaISt4pairIKS6_S9_EEE")
            .value_or(nullptr);

    if (api.updateTokenMap == nullptr || api.installDebugToken == nullptr ||
        api.eraseDebugToken == nullptr)
    {
        return std::nullopt;
    }

    return api;
}

template <typename Callback>
void swallowProbeExceptions(Callback&& callback)
{
    try
    {
        callback();
    }
    catch (const std::exception&)
    {}
}

void runDirectProbePublicMethods()
{
    auto api = resolveDirectProbeApi();
    if (!api)
    {
        appendMarker("probe_symbol_resolution_failed");
        return;
    }

    std::vector<std::filesystem::path> artifacts;
    auto removeArtifacts = [&artifacts]() {
        for (const auto& artifact : artifacts)
        {
            std::error_code ec;
            std::filesystem::remove(artifact, ec);
        }
    };

    const auto legacyGood = makeLegacyTokenImage();
    const auto legacyInvalidType =
        makeLegacyTokenImage("EDTI",
                             std::vector<uint8_t>(matchingSerialBytes.begin(),
                                                  matchingSerialBytes.end()),
                             1);
    const auto legacyMcu = makeLegacyTokenImage(
        "MCDT",
        std::vector<uint8_t>{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                             0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F});
    const auto legacyTruncated = makeTruncatedLegacyTokenImage();
    const auto tlvGood =
        makeTlvTokenImage({makeSerialRecord(std::vector<uint8_t>(
            matchingSerialBytes.begin(), matchingSerialBytes.end()))});
    const auto tlvMissingSerial =
        makeTlvTokenImage({makeMissingSerialRecord()});
    const auto tlvMixed = makeTlvTokenImage(
        {makeBadIdentifierRecord(1),
         makeSerialRecord(std::vector<uint8_t>(matchingSerialBytes.begin(),
                                               matchingSerialBytes.end()))});
    const auto tlvShortData = makeTlvTokenImage({makeBadIdentifierRecord(32)});

    artifacts = {legacyGood, legacyInvalidType, legacyMcu, legacyTruncated,
                 tlvGood,    tlvMissingSerial,  tlvMixed,  tlvShortData};

    auto bus = sdbusplus::bus::new_default();
    UpdateDebugToken updateDebugToken(bus);

    appendMarker("probe_update_token_map_begin");
    for (const auto& tokenPath : artifacts)
    {
        TokenMap tokens;
        swallowProbeExceptions([&] {
            (void)api->updateTokenMap(&updateDebugToken, tokenPath.string(),
                                      tokens);
        });
    }

    TokenMap missingPathTokens;
    swallowProbeExceptions([&] {
        (void)api->updateTokenMap(&updateDebugToken,
                                  "/tmp/debug-token-binary-preload-missing.bin",
                                  missingPathTokens);
    });
    appendMarker("probe_update_token_map_end");

    appendMarker("probe_install_begin");
    resetRuntimeState();
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(
            &updateDebugToken,
            "/tmp/debug-token-binary-preload-install-missing.bin");
    });

    resetRuntimeState();
    buildScenario("install_enumerate_fail");
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(&updateDebugToken, legacyGood.string());
    });

    resetRuntimeState();
    buildScenario("install_no_endpoints");
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(&updateDebugToken, legacyGood.string());
    });

    resetRuntimeState();
    buildScenario("install_all_missing_tokens");
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(&updateDebugToken, legacyGood.string());
    });

    resetRuntimeState();
    buildScenario("install_mixed");
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(&updateDebugToken, legacyGood.string());
    });

    resetRuntimeState();
    buildScenario("install_signal_success");
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(&updateDebugToken, tlvGood.string());
    });

    resetRuntimeState();
    buildScenario("install_signal_failure");
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(&updateDebugToken, tlvGood.string());
    });

    resetRuntimeState();
    buildScenario("install_property_wrong_type");
    swallowProbeExceptions([&] {
        (void)api->installDebugToken(&updateDebugToken, legacyGood.string());
    });
    appendMarker("probe_install_end");

    appendMarker("probe_erase_begin");
    resetRuntimeState();
    buildScenario("erase_manual");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });

    resetRuntimeState();
    buildScenario("erase_policy_empty_then_enum_fail");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });

    resetRuntimeState();
    buildScenario("erase_policy_multi_then_enum_fail");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });

    resetRuntimeState();
    buildScenario("erase_policy_property_fail_then_enum_fail");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });

    resetRuntimeState();
    buildScenario("erase_auto_no_endpoints");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });

    resetRuntimeState();
    buildScenario("erase_auto_mixed");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });

    resetRuntimeState();
    buildScenario("erase_auto_signal_success");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });

    resetRuntimeState();
    buildScenario("erase_auto_signal_failure");
    swallowProbeExceptions(
        [&] { (void)api->eraseDebugToken(&updateDebugToken); });
    appendMarker("probe_erase_end");

    appendMarker("probe_private_begin");
    const std::string oldEndpointPath =
        "/xyz/openbmc_project/NSM/" + std::string(96, 'n');
    const std::string oldEndpointAlt =
        "/xyz/openbmc_project/NSM/" + std::string(96, 'm');
    const std::string oldInfo(192, 'I');
    TokenMap directTokens;
    directTokens.emplace(matchingSerial, std::vector<uint8_t>(128, 0x5A));

    auto asyncPath = [](std::string_view suffix) {
        return std::string("/com/nvidia/nsmd/AsyncOperation/private-") +
               std::string(suffix);
    };
    const auto emptyArg = AsyncMethodArg{std::monostate{}};
    auto queueTokenStatus =
        [&](std::string_view suffix, const char* asyncStatus,
            const char* tokenStatus, uint32_t timeLeft = 7) {
            pushObjectPath(asyncPath(suffix).c_str());
            pushVariantString(asyncStatus);
            pushVariantTokenStatus(nsmTokenTypeCRDT, tokenStatus,
                                   oldInfo.c_str(), timeLeft);
        };
    auto makeMemfd = [](std::string_view name,
                        const std::vector<uint8_t>& data) {
        const std::string memfdName(name);
        int memfd = memfd_create(memfdName.c_str(), MFD_CLOEXEC);
        if (memfd >= 0)
        {
            [[maybe_unused]] const auto written =
                write(memfd, data.data(), data.size());
            lseek(memfd, 0, SEEK_SET);
        }
        return memfd;
    };
    auto closeMemfd = [](int& memfd) {
        if (memfd >= 0)
        {
            close(memfd);
            memfd = -1;
        }
    };
    const auto callEnumerateLegacy = [&](NSMEndpoints& endpoints) {
        if (api->enumerateNsmEndpoints != nullptr)
        {
            return api->enumerateNsmEndpoints(&updateDebugToken, endpoints);
        }
        return updateDebugToken.enumerateNsmDebugTokenEndpoints(endpoints);
    };
    const auto callEnumerateV2 = [&](NSMEndpoints& endpoints) {
        if (api->enumerateNsmEndpointsV2 != nullptr)
        {
            return api->enumerateNsmEndpointsV2(&updateDebugToken, endpoints);
        }
        return updateDebugToken.enumerateNsmDebugTokenEndpointsV2(endpoints);
    };
    const auto callMakeMethod = [&](const std::string& path,
                                    const std::string& methodName,
                                    const AsyncMethodArg& arg) {
        if (api->makeDebugTokenMethodCall != nullptr)
        {
            return api->makeDebugTokenMethodCall(&updateDebugToken, path,
                                                 methodName, arg);
        }
        return updateDebugToken.makeDebugTokenMethodCall(path, methodName, arg);
    };
    const auto callLogAsyncError = [&](const std::string& path,
                                       const std::string& methodName,
                                       const std::string& status) {
        if (api->logAsyncError != nullptr)
        {
            api->logAsyncError(&updateDebugToken, path, methodName, status);
            return;
        }
        updateDebugToken.logAsyncError(path, methodName, status);
    };
    const auto callHandleAsync = [&](const std::string& path,
                                     const std::string& methodName,
                                     const AsyncMethodArg& arg) {
        if (api->handleAsyncCall != nullptr)
        {
            return api->handleAsyncCall(&updateDebugToken, path, methodName,
                                        arg);
        }
        return updateDebugToken.handleAsyncCall(path, methodName, arg);
    };
    const auto callGetTokenStatus = [&](const std::string& path) {
        if (api->getTokenStatus != nullptr)
        {
            return api->getTokenStatus(&updateDebugToken, path);
        }
        return updateDebugToken.getTokenStatus(path);
    };
    const auto callNsmTokenErase = [&] {
        if (api->nsmTokenErase != nullptr)
        {
            return api->nsmTokenErase(&updateDebugToken);
        }
        return updateDebugToken.nsmTokenErase();
    };
    const auto callNsmTokenInstall = [&](TokenMap& tokens) {
        if (api->nsmTokenInstall != nullptr)
        {
            return api->nsmTokenInstall(&updateDebugToken, tokens);
        }
        return updateDebugToken.nsmTokenInstall(tokens);
    };
    const auto callHandleAsyncInstallV2 = [&](const std::string& path,
                                              int memfd) {
        if (api->handleAsyncCallInstallV2 != nullptr)
        {
            return api->handleAsyncCallInstallV2(&updateDebugToken, path,
                                                 memfd);
        }
        return updateDebugToken.handleAsyncCallInstallV2(path, memfd);
    };
    const auto callHandleAsyncEraseV2 = [&](const std::string& path) {
        if (api->handleAsyncCallEraseV2 != nullptr)
        {
            return api->handleAsyncCallEraseV2(&updateDebugToken, path);
        }
        return updateDebugToken.handleAsyncCallEraseV2(path);
    };
    const auto callNsmTokenEraseV2 = [&] {
        if (api->nsmTokenEraseV2 != nullptr)
        {
            return api->nsmTokenEraseV2(&updateDebugToken);
        }
        return updateDebugToken.nsmTokenEraseV2();
    };
    const auto callNsmTokenInstallV2 = [&](TokenMap& tokens) {
        if (api->nsmTokenInstallV2 != nullptr)
        {
            return api->nsmTokenInstallV2(&updateDebugToken, tokens);
        }
        return updateDebugToken.nsmTokenInstallV2(tokens);
    };

    swallowProbeExceptions([&] {
        NSMEndpoints endpoints;

        resetRuntimeState();
        pushGetSubTreeEntries(
            {{oldEndpointPath, nsmService, nsmDebugTokenIntfName},
             {oldEndpointAlt, nsmService, nsmDebugTokenIntfName}});
        (void)callEnumerateLegacy(endpoints);

        resetRuntimeState();
        endpoints.clear();
        pushError(-ENOENT);
        (void)callEnumerateLegacy(endpoints);

        resetRuntimeState();
        endpoints.clear();
        pushGetSubTreeEntries(
            {{oldEndpointPath, nsmService, nsmDebugTokenActionIntfName}});
        (void)callEnumerateV2(endpoints);

        resetRuntimeState();
        endpoints.clear();
        pushError(-ENOENT);
        (void)callEnumerateV2(endpoints);

        resetRuntimeState();
        pushObjectPath(asyncPath("method-erase").c_str());
        (void)callMakeMethod(oldEndpointPath, "EraseToken", emptyArg);

        resetRuntimeState();
        pushObjectPath(asyncPath("method-status").c_str());
        (void)callMakeMethod(oldEndpointPath, "GetStatus",
                             AsyncMethodArg{std::string(nsmTokenTypeCRDT)});

        resetRuntimeState();
        pushObjectPath(asyncPath("method-install").c_str());
        (void)callMakeMethod(oldEndpointPath, "InstallToken",
                             AsyncMethodArg{std::vector<uint8_t>(24, 0x42)});

        resetRuntimeState();
        pushError(-ENOENT);
        (void)callMakeMethod(oldEndpointPath, "DisableTokens", emptyArg);

        resetRuntimeState();
        pushVariantErrorTuple(debug_token::NotInstalled, "token missing");
        callLogAsyncError(oldEndpointPath, "EraseToken", "Failed");

        resetRuntimeState();
        pushVariantErrorTuple(0x42, "generic failure");
        callLogAsyncError(oldEndpointPath, "InstallToken", "Error");

        resetRuntimeState();
        pushError(-ENOENT);
        callLogAsyncError(oldEndpointPath, "DisableTokens", "Timeout");

        resetRuntimeState();
        pushObjectPath(asyncPath("handle-success").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        (void)callHandleAsync(oldEndpointPath, "DisableTokens", emptyArg);

        resetRuntimeState();
        pushObjectPath(asyncPath("handle-progress").c_str());
        pushVariantString("com.nvidia.Async.Status.InProgress");
        queueSignal(asyncPath("handle-progress"),
                    "com.nvidia.Async.Status.Success", "Status");
        (void)callHandleAsync(oldEndpointPath, "DisableTokens", emptyArg);

        resetRuntimeState();
        pushObjectPath(asyncPath("handle-fail").c_str());
        pushVariantString("com.nvidia.Async.Status.Failed");
        pushVariantErrorTuple(0x42, "disable failed");
        (void)callHandleAsync(oldEndpointPath, "DisableTokens", emptyArg);

        resetRuntimeState();
        pushError(-ENOENT);
        (void)callHandleAsync(oldEndpointPath, "DisableTokens", emptyArg);

        resetRuntimeState();
        queueTokenStatus("token-status", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusDebugSessionActive, 3);
        (void)callGetTokenStatus(oldEndpointPath);

        resetRuntimeState();
        pushError(-ENOENT);
        (void)callGetTokenStatus(oldEndpointPath);

        resetRuntimeState();
        pushError(-ENOENT);
        (void)callNsmTokenErase();

        resetRuntimeState();
        pushGetSubTreeEntries({});
        (void)callNsmTokenErase();

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("erase-none", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusNoTokenApplied, 0);
        (void)callNsmTokenErase();

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("erase-before", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusDebugSessionActive, 3);
        pushObjectPath(asyncPath("erase-disable").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        queueTokenStatus("erase-after", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusNoTokenApplied, 0);
        (void)callNsmTokenErase();

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("erase-fail-before", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusDebugSessionActive, 3);
        pushObjectPath(asyncPath("erase-fail-disable").c_str());
        pushVariantString("com.nvidia.Async.Status.Failed");
        pushVariantErrorTuple(0x42, "disable failed");
        (void)callNsmTokenErase();

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("erase-wrong-before",
                         "com.nvidia.Async.Status.Success",
                         nsmTokenStatusDebugSessionActive, 3);
        pushObjectPath(asyncPath("erase-wrong-disable").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        queueTokenStatus("erase-wrong-after", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusTokenTimeout, 4);
        (void)callNsmTokenErase();

        resetRuntimeState();
        pushError(-ENOENT);
        (void)callNsmTokenInstall(directTokens);

        resetRuntimeState();
        pushGetSubTreeEntries({});
        (void)callNsmTokenInstall(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("install-active", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusDebugSessionActive, 1);
        (void)callNsmTokenInstall(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("install-missing", "com.nvidia.Async.Status.Success",
                         nsmTokenStatusNoTokenApplied, 0);
        pushVariantString("0xDEADBEEF00000000");
        (void)callNsmTokenInstall(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("install-success-before",
                         "com.nvidia.Async.Status.Success",
                         nsmTokenStatusNoTokenApplied, 0);
        pushVariantString(matchingSerial);
        pushObjectPath(asyncPath("install-success").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        queueTokenStatus("install-success-after",
                         "com.nvidia.Async.Status.Success",
                         nsmTokenStatusDebugSessionActive, 0);
        (void)callNsmTokenInstall(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("install-fail-before",
                         "com.nvidia.Async.Status.Success",
                         nsmTokenStatusNoTokenApplied, 0);
        pushVariantString(matchingSerial);
        pushObjectPath(asyncPath("install-fail").c_str());
        pushVariantString("com.nvidia.Async.Status.Failed");
        pushVariantErrorTuple(0x23, "install failed");
        (void)callNsmTokenInstall(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenIntfName);
        queueTokenStatus("install-wrong-before",
                         "com.nvidia.Async.Status.Success",
                         nsmTokenStatusNoTokenApplied, 0);
        pushVariantString(matchingSerial);
        pushObjectPath(asyncPath("install-wrong").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        queueTokenStatus("install-wrong-after",
                         "com.nvidia.Async.Status.Success",
                         nsmTokenStatusNoTokenApplied, 0);
        (void)callNsmTokenInstall(directTokens);

        resetRuntimeState();
        pushObjectPath(asyncPath("install-v2-success").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        {
            int memfd = makeMemfd("private-install-v2-success",
                                  std::vector<uint8_t>(64, 0xCC));
            (void)callHandleAsyncInstallV2(oldEndpointPath, memfd);
            closeMemfd(memfd);
        }

        resetRuntimeState();
        pushObjectPath(asyncPath("install-v2-progress").c_str());
        pushVariantString("com.nvidia.Async.Status.InProgress");
        queueSignal(asyncPath("install-v2-progress"),
                    "com.nvidia.Async.Status.Success", "Status");
        {
            int memfd = makeMemfd("private-install-v2-progress",
                                  std::vector<uint8_t>(64, 0xDD));
            (void)callHandleAsyncInstallV2(oldEndpointPath, memfd);
            closeMemfd(memfd);
        }

        resetRuntimeState();
        pushObjectPath(asyncPath("install-v2-fail").c_str());
        pushVariantString("com.nvidia.Async.Status.Error");
        pushVariantErrorTuple(0x23, "install failed");
        {
            int memfd = makeMemfd("private-install-v2-fail",
                                  std::vector<uint8_t>(64, 0xEE));
            (void)callHandleAsyncInstallV2(oldEndpointPath, memfd);
            closeMemfd(memfd);
        }

        resetRuntimeState();
        pushObjectPath(asyncPath("erase-v2-success").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        (void)callHandleAsyncEraseV2(oldEndpointPath);

        resetRuntimeState();
        pushObjectPath(asyncPath("erase-v2-fail").c_str());
        pushVariantString("com.nvidia.Async.Status.Error");
        pushVariantErrorTuple(debug_token::NotInstalled, "token missing");
        pushVariantErrorTuple(debug_token::NotInstalled, "token missing");
        (void)callHandleAsyncEraseV2(oldEndpointPath);

        resetRuntimeState();
        pushError(-ENOENT);
        (void)callNsmTokenEraseV2();

        resetRuntimeState();
        pushGetSubTreeEntries({});
        (void)callNsmTokenEraseV2();

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenActionIntfName);
        pushObjectPath(asyncPath("erase-v2-top-success").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        (void)callNsmTokenEraseV2();

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenActionIntfName);
        pushObjectPath(asyncPath("erase-v2-top-fail").c_str());
        pushVariantString("com.nvidia.Async.Status.Error");
        pushVariantErrorTuple(debug_token::NotInstalled, "token missing");
        pushVariantErrorTuple(debug_token::NotInstalled, "token missing");
        (void)callNsmTokenEraseV2();

        resetRuntimeState();
        pushError(-ENOENT);
        (void)callNsmTokenInstallV2(directTokens);

        resetRuntimeState();
        pushGetSubTreeEntries({});
        (void)callNsmTokenInstallV2(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantString("0xDEADBEEF00000000");
        (void)callNsmTokenInstallV2(directTokens);

        resetRuntimeState();
        preload_state::failMemfdCreateRemaining = 1;
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantString(matchingSerial);
        (void)callNsmTokenInstallV2(directTokens);

        resetRuntimeState();
        preload_state::shortWriteRemaining = 1;
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantString(matchingSerial);
        (void)callNsmTokenInstallV2(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantString(matchingSerial);
        pushObjectPath(asyncPath("install-v2-top-success").c_str());
        pushVariantString("com.nvidia.Async.Status.Success");
        (void)callNsmTokenInstallV2(directTokens);

        resetRuntimeState();
        pushGetSubTreeOneEndpoint(oldEndpointPath.c_str(), nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantString(matchingSerial);
        pushObjectPath(asyncPath("install-v2-top-fail").c_str());
        pushVariantString("com.nvidia.Async.Status.Error");
        pushVariantErrorTuple(0x23, "install failed");
        (void)callNsmTokenInstallV2(directTokens);
    });
    appendMarker("probe_private_end");

    appendMarker("probe_complete");
    removeArtifacts();
}

int discardReply(sd_bus_message** reply)
{
    if (reply != nullptr && *reply != nullptr)
    {
        sd_bus_message_unref(*reply);
        *reply = nullptr;
    }
    return -EIO;
}

template <typename AppendFn>
int makeMethodReturn(sd_bus_message* request, sd_bus_message** reply,
                     AppendFn&& appendFn)
{
    if (sd_bus_message_new_method_return(request, reply) < 0 ||
        *reply == nullptr)
    {
        return -EIO;
    }

    try
    {
        sdbusplus::message::message response(*reply, std::false_type{});
        appendFn(response);
        if (sd_bus_message_seal(response.get(), 0, 0) < 0)
        {
            return -EIO;
        }
        if (sd_bus_message_rewind(response.get(), true) < 0)
        {
            return -EIO;
        }
        *reply = response.release();
        return 0;
    }
    catch (const std::exception&)
    {
        return discardReply(reply);
    }
}

dbus::GetSubTreeResponse
    buildGetSubTreeResponse(const std::vector<SubTreeEntry>& entries)
{
    dbus::GetSubTreeResponse response;
    for (const auto& [path, service, iface] : entries)
    {
        dbus::MapperServiceMap mapperServiceMap;
        mapperServiceMap.emplace_back(service, dbus::Interfaces{iface});
        response.emplace_back(path, std::move(mapperServiceMap));
    }
    return response;
}

void pushError(int rc = -ENOENT)
{
    preload_state::responses.push_back(
        [rc](sd_bus_message*, sd_bus_message**) { return rc; });
}

void pushGetSubTreeEntries(const std::vector<SubTreeEntry>& entries)
{
    preload_state::responses.push_back(
        [entries](sd_bus_message* request, sd_bus_message** reply) {
            const auto getSubTree = buildGetSubTreeResponse(entries);
            return makeMethodReturn(request, reply,
                                    [&](sdbusplus::message::message& response) {
                                        response.append(getSubTree);
                                    });
        });
}

void pushGetSubTreeOneEndpoint(const char* path, const char* service,
                               const char* iface)
{
    pushGetSubTreeEntries({{path, service, iface}});
}

void pushVariantString(const char* value)
{
    std::string ownedValue = value == nullptr ? "" : value;
    preload_state::responses.push_back(
        [ownedValue = std::move(ownedValue)](sd_bus_message* request,
                                             sd_bus_message** reply) {
            return makeMethodReturn(
                request, reply, [&](sdbusplus::message::message& response) {
                    response.append(std::variant<std::string>{ownedValue});
                });
        });
}

void pushVariantUint32(uint32_t value)
{
    preload_state::responses.push_back(
        [value](sd_bus_message* request, sd_bus_message** reply) {
            return makeMethodReturn(
                request, reply, [&](sdbusplus::message::message& response) {
                    response.append(std::variant<uint32_t>{value});
                });
        });
}

void pushVariantErrorTuple(uint16_t errorCode, const char* errorMessage)
{
    std::string ownedMessage =
        errorMessage == nullptr ? "" : std::string(errorMessage);
    preload_state::responses.push_back(
        [errorCode, ownedMessage = std::move(ownedMessage)](
            sd_bus_message* request, sd_bus_message** reply) {
            auto* bus = getBusFromMsg(request);
            if (bus == nullptr || sd_bus_message_new(bus, reply, 2) < 0)
            {
                return -EIO;
            }

            int rc = sd_bus_message_open_container(*reply, 'v', "v");
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_open_container(*reply, 'v', "(qs)");
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_open_container(*reply, 'r', "qs");
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_append_basic(*reply, 'q', &errorCode);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            const char* errorMessageValue = ownedMessage.c_str();
            rc = sd_bus_message_append_basic(*reply, 's', errorMessageValue);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_close_container(*reply);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_close_container(*reply);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_close_container(*reply);
            if (rc < 0 || sd_bus_message_seal(*reply, 0, 0) < 0)
            {
                return discardReply(reply);
            }
            if (sd_bus_message_rewind(*reply, true) < 0)
            {
                return discardReply(reply);
            }
            return 0;
        });
}

void pushVariantTokenStatus(const char* tokenType, const char* tokenStatus,
                            const char* additionalInfo, uint32_t timeLeft)
{
    std::string ownedTokenType =
        tokenType == nullptr ? "" : std::string(tokenType);
    std::string ownedTokenStatus =
        tokenStatus == nullptr ? "" : std::string(tokenStatus);
    std::string ownedAdditionalInfo =
        additionalInfo == nullptr ? "" : std::string(additionalInfo);
    preload_state::responses.push_back(
        [ownedTokenType = std::move(ownedTokenType),
         ownedTokenStatus = std::move(ownedTokenStatus),
         ownedAdditionalInfo = std::move(ownedAdditionalInfo),
         timeLeft](sd_bus_message* request, sd_bus_message** reply) {
            auto* bus = getBusFromMsg(request);
            if (bus == nullptr || sd_bus_message_new(bus, reply, 2) < 0)
            {
                return -EIO;
            }

            int rc = sd_bus_message_open_container(*reply, 'v', "v");
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_open_container(*reply, 'v', "(sssu)");
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_open_container(*reply, 'r', "sssu");
            if (rc < 0)
            {
                return discardReply(reply);
            }
            const char* tokenTypeValue = ownedTokenType.c_str();
            rc = sd_bus_message_append_basic(*reply, 's', tokenTypeValue);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            const char* tokenStatusValue = ownedTokenStatus.c_str();
            rc = sd_bus_message_append_basic(*reply, 's', tokenStatusValue);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            const char* additionalInfoValue = ownedAdditionalInfo.c_str();
            rc = sd_bus_message_append_basic(*reply, 's', additionalInfoValue);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_append_basic(*reply, 'u', &timeLeft);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_close_container(*reply);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_close_container(*reply);
            if (rc < 0)
            {
                return discardReply(reply);
            }
            rc = sd_bus_message_close_container(*reply);
            if (rc < 0 || sd_bus_message_seal(*reply, 0, 0) < 0)
            {
                return discardReply(reply);
            }
            if (sd_bus_message_rewind(*reply, true) < 0)
            {
                return discardReply(reply);
            }
            return 0;
        });
}

void pushObjectPath(const char* path)
{
    std::string ownedPath = path == nullptr ? "" : std::string(path);
    preload_state::responses.push_back(
        [ownedPath = std::move(ownedPath)](sd_bus_message* request,
                                           sd_bus_message** reply) {
            return makeMethodReturn(
                request, reply, [&](sdbusplus::message::message& response) {
                    response.append(sdbusplus::object_path{ownedPath});
                });
        });
}

sd_bus_message*
    makeAsyncStatusSignalRaw(sd_bus* bus, const std::string& objectPath,
                             const std::string& status,
                             const std::string& propertyName = "Status")
{
    sd_bus_message* rawMsg = nullptr;
    if (sd_bus_message_new_signal(bus, &rawMsg, objectPath.c_str(),
                                  "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged") < 0)
    {
        return nullptr;
    }

    const char* interface = nsmAsyncStatusIntfName;
    if (sd_bus_message_append_basic(rawMsg, 's', interface) < 0 ||
        sd_bus_message_open_container(rawMsg, 'a', "{sv}") < 0)
    {
        sd_bus_message_unref(rawMsg);
        return nullptr;
    }

    const char* propertyNameValue = propertyName.c_str();
    const char* rawStatus = status.c_str();
    if (sd_bus_message_open_container(rawMsg, 'e', "sv") < 0 ||
        sd_bus_message_append_basic(rawMsg, 's', propertyNameValue) < 0 ||
        sd_bus_message_open_container(rawMsg, 'v', "s") < 0 ||
        sd_bus_message_append_basic(rawMsg, 's', rawStatus) < 0 ||
        sd_bus_message_close_container(rawMsg) < 0 ||
        sd_bus_message_close_container(rawMsg) < 0 ||
        sd_bus_message_close_container(rawMsg) < 0 ||
        sd_bus_message_open_container(rawMsg, 'a', "s") < 0 ||
        sd_bus_message_close_container(rawMsg) < 0 ||
        sd_bus_message_seal(rawMsg, 0, 0) < 0)
    {
        sd_bus_message_unref(rawMsg);
        return nullptr;
    }

    sd_bus_message_rewind(rawMsg, true);
    return rawMsg;
}

void queueSignal(const std::string& objectPath, const std::string& status,
                 const std::string& propertyName = "Status")
{
    preload_state::pendingSignals.push_back({.objectPath = objectPath,
                                             .status = status,
                                             .propertyName = propertyName});
}

void pushAutomaticPolicy()
{
    pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                              "com.nvidia.DebugToken", erasePolicyIntfName);
    pushVariantString("Automatic");
}

void buildScenario(const std::string& scenario)
{
    using preload_state::failMemfdCreateRemaining;
    using preload_state::shortWriteRemaining;

    if (scenario == "erase_auto_mixed")
    {
        pushAutomaticPolicy();
        pushGetSubTreeEntries(
            {{"invalid-object-path", nsmService, nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu0", nsmService,
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu1", nsmService,
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu2", nsmService,
              nsmDebugTokenActionIntfName}});

        pushObjectPath("/com/nvidia/nsmd/async/not-installed");
        pushVariantString("com.nvidia.Async.Status.Error");
        pushVariantErrorTuple(0x100F, "token missing");
        pushVariantErrorTuple(0x100F, "token missing");

        pushObjectPath("/com/nvidia/nsmd/async/generic-error");
        pushVariantString("com.nvidia.Async.Status.Failed");
        pushVariantErrorTuple(0x42, "erase failed");
        pushVariantErrorTuple(0x42, "erase failed");

        pushObjectPath("/com/nvidia/nsmd/async/success");
        pushVariantString("com.nvidia.Async.Status.Success");
        return;
    }

    if (scenario == "erase_auto_signal_success")
    {
        pushAutomaticPolicy();
        pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0", nsmService,
                                  nsmDebugTokenActionIntfName);
        pushObjectPath("/com/nvidia/nsmd/AsyncOperation/erase-signal-success");
        pushVariantString("com.nvidia.Async.Status.InProgress");
        queueSignal("/com/nvidia/nsmd/AsyncOperation/erase-signal-success",
                    "com.nvidia.Async.Status.Success");
        return;
    }

    if (scenario == "erase_auto_signal_failure")
    {
        pushAutomaticPolicy();
        pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0", nsmService,
                                  nsmDebugTokenActionIntfName);
        pushObjectPath("/com/nvidia/nsmd/AsyncOperation/erase-signal-failure");
        pushVariantString("com.nvidia.Async.Status.InProgress");
        queueSignal("/com/nvidia/nsmd/AsyncOperation/erase-signal-failure",
                    "com.nvidia.Async.Status.Failed");
        pushVariantErrorTuple(0x42, "erase failed");
        return;
    }

    if (scenario == "erase_manual")
    {
        pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                  "com.nvidia.DebugToken", erasePolicyIntfName);
        pushVariantString("Manual");
        return;
    }

    if (scenario == "erase_auto_no_endpoints")
    {
        pushAutomaticPolicy();
        pushGetSubTreeEntries({});
        return;
    }

    if (scenario == "erase_policy_empty_then_enum_fail")
    {
        pushGetSubTreeEntries({});
        pushError(-ENOENT);
        return;
    }

    if (scenario == "erase_policy_multi_then_enum_fail")
    {
        pushGetSubTreeEntries({{"/com/nvidia/debug_token/policy0",
                                "com.nvidia.DebugToken", erasePolicyIntfName},
                               {"/com/nvidia/debug_token/policy1",
                                "com.nvidia.DebugToken", erasePolicyIntfName}});
        pushError(-ENOENT);
        return;
    }

    if (scenario == "erase_policy_property_fail_then_enum_fail")
    {
        pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                  "com.nvidia.DebugToken", erasePolicyIntfName);
        pushError(-ENOENT);
        pushError(-ENOENT);
        return;
    }

    if (scenario == "install_mixed")
    {
        failMemfdCreateRemaining = 1;
        shortWriteRemaining = 1;

        pushGetSubTreeEntries(
            {{"invalid-object-path", nsmService, nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu0", nsmService,
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu1", nsmService,
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu2", nsmService,
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu3", nsmService,
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu4", nsmService,
              nsmDebugTokenActionIntfName}});

        pushVariantString("0xDEADBEEF00000000");
        pushVariantString(matchingSerial);
        pushVariantString(matchingSerial);
        pushVariantString(matchingSerial);
        pushObjectPath("/com/nvidia/nsmd/async/install-failed");
        pushVariantString("com.nvidia.Async.Status.Error");
        pushVariantErrorTuple(0x23, "install failed");

        pushVariantString(matchingSerial);
        pushObjectPath("/com/nvidia/nsmd/async/install-success");
        pushVariantString("com.nvidia.Async.Status.Success");
        return;
    }

    if (scenario == "install_signal_success")
    {
        pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0", nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantString(matchingSerial);
        pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/install-signal-success");
        pushVariantString("com.nvidia.Async.Status.InProgress");
        queueSignal("/com/nvidia/nsmd/AsyncOperation/install-signal-success",
                    "com.nvidia.Async.Status.Success");
        return;
    }

    if (scenario == "install_signal_failure")
    {
        pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0", nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantString(matchingSerial);
        pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/install-signal-failure");
        pushVariantString("com.nvidia.Async.Status.InProgress");
        queueSignal("/com/nvidia/nsmd/AsyncOperation/install-signal-failure",
                    "com.nvidia.Async.Status.Failed");
        pushVariantErrorTuple(0x23, "install failed");
        return;
    }

    if (scenario == "install_no_endpoints")
    {
        pushGetSubTreeEntries({});
        return;
    }

    if (scenario == "install_all_missing_tokens")
    {
        pushGetSubTreeEntries({{"/xyz/openbmc_project/NSM/gpu0", nsmService,
                                nsmDebugTokenActionIntfName},
                               {"/xyz/openbmc_project/NSM/gpu1", nsmService,
                                nsmDebugTokenActionIntfName}});
        pushVariantString("0xDEADBEEF00000000");
        pushVariantString("0xCAFEBABE00000000");
        return;
    }

    if (scenario == "install_property_wrong_type")
    {
        pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0", nsmService,
                                  nsmDebugTokenActionIntfName);
        pushVariantUint32(0x12345678);
        return;
    }

    if (scenario == "install_enumerate_fail")
    {
        pushError(-ENOENT);
        return;
    }
}

void ensureInitialized()
{
    if (preload_state::initialized)
    {
        return;
    }

    preload_state::initialized = true;
    appendMarker("init");
    if (const char* scenario = std::getenv("DEBUG_TOKEN_PRELOAD_SCENARIO");
        scenario != nullptr)
    {
        buildScenario(scenario);
        if (std::string_view(scenario) == "direct_probe_public_methods")
        {
            runDirectProbePublicMethods();
            resetRuntimeState();
        }
    }
}

} // namespace

extern "C" int sd_bus_call(sd_bus* bus, sd_bus_message* message, uint64_t usec,
                           sd_bus_error* retError, sd_bus_message** reply)
{
    ensureInitialized();
    appendMarker("sd_bus_call");

    if (!preload_state::responses.empty())
    {
        auto fn = std::move(preload_state::responses.front());
        preload_state::responses.pop_front();
        return fn(message, reply);
    }

    auto real =
        loadSymbol<int (*)(sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                           sd_bus_message**)>("sd_bus_call");
    return real(bus, message, usec, retError, reply);
}

extern "C" int sd_bus_add_match(sd_bus* bus, sd_bus_slot** slot,
                                const char* match,
                                sd_bus_message_handler_t callback,
                                void* userdata)
{
    ensureInitialized();

    auto real = loadSymbol<int (*)(sd_bus*, sd_bus_slot**, const char*,
                                   sd_bus_message_handler_t, void*)>(
        "sd_bus_add_match");
    int rc = real(bus, slot, match, callback, userdata);
    if (rc >= 0)
    {
        preload_state::matchCallback = callback;
        preload_state::matchUserdata = userdata;
        preload_state::matchSlot = (slot != nullptr) ? *slot : nullptr;
    }
    return rc;
}

extern "C" int sd_bus_wait(sd_bus* bus, uint64_t timeout_usec)
{
    ensureInitialized();

    if (!preload_state::pendingSignals.empty() &&
        preload_state::matchCallback != nullptr)
    {
        const auto signalState = preload_state::pendingSignals.front();
        preload_state::pendingSignals.pop_front();
        auto* signal = makeAsyncStatusSignalRaw(bus, signalState.objectPath,
                                                signalState.status,
                                                signalState.propertyName);
        if (signal != nullptr)
        {
            preload_state::currentSlot = preload_state::matchSlot;
            auto rc = preload_state::matchCallback(
                signal, preload_state::matchUserdata, nullptr);
            preload_state::currentSlot = nullptr;
            sd_bus_message_unref(signal);
            return rc >= 0 ? 1 : rc;
        }
    }

    auto real = loadSymbol<int (*)(sd_bus*, uint64_t)>("sd_bus_wait");
    return real(bus, timeout_usec);
}

extern "C" void* sd_bus_slot_set_userdata(sd_bus_slot* slot, void* userdata)
{
    ensureInitialized();

    if (slot == preload_state::matchSlot)
    {
        preload_state::slotUserdata = userdata;
    }

    auto real =
        loadSymbol<void* (*)(sd_bus_slot*, void*)>("sd_bus_slot_set_userdata");
    return real(slot, userdata);
}

extern "C" sd_bus_slot* sd_bus_get_current_slot(sd_bus* bus)
{
    ensureInitialized();

    if (preload_state::currentSlot != nullptr)
    {
        return preload_state::currentSlot;
    }

    auto real =
        loadSymbol<sd_bus_slot* (*)(sd_bus*)>("sd_bus_get_current_slot");
    return real(bus);
}

extern "C" void* sd_bus_slot_get_userdata(sd_bus_slot* slot)
{
    ensureInitialized();

    if (slot == preload_state::matchSlot &&
        preload_state::slotUserdata != nullptr)
    {
        return preload_state::slotUserdata;
    }

    auto real = loadSymbol<void* (*)(sd_bus_slot*)>("sd_bus_slot_get_userdata");
    return real(slot);
}

extern "C" int memfd_create(const char* name, unsigned int flags)
{
    ensureInitialized();

    if (preload_state::failMemfdCreateRemaining > 0)
    {
        preload_state::failMemfdCreateRemaining--;
        errno = EMFILE;
        return -1;
    }

    auto real = loadSymbol<int (*)(const char*, unsigned int)>("memfd_create");
    int fd = real(name, flags);
    preload_state::trackedFd = fd;
    return fd;
}

extern "C" ssize_t write(int fd, const void* buf, size_t count)
{
    ensureInitialized();

    if (preload_state::shortWriteRemaining > 0 &&
        fd == preload_state::trackedFd && count > 0)
    {
        preload_state::shortWriteRemaining--;
        errno = ENOSPC;
        return static_cast<ssize_t>(count - 1);
    }

    auto real = loadSymbol<ssize_t (*)(int, const void*, size_t)>("write");
    return real(fd, buf, count);
}

extern "C" int debugTokenPreloadMain(int argc, char** argv, char** envp)
{
    ensureInitialized();
    appendMarker("main_wrapper_enter");
    if (argc < 3)
    {
        appendMarker("argc_lt3");
    }
    const int status = preload_state::realMain(argc, argv, envp);
    if (status != 0)
    {
        appendMarker("main_nonzero");
    }
    return status;
}

extern "C" int __libc_start_main(int (*mainFn)(int, char**, char**), int argc,
                                 char** argv, void (*init)(void),
                                 void (*fini)(void), void (*rtldFini)(void),
                                 void* stackEnd)
{
    auto real = loadSymbol<int (*)(int (*)(int, char**, char**), int, char**,
                                   void (*)(void), void (*)(void),
                                   void (*)(void), void*)>("__libc_start_main");
    preload_state::realMain = mainFn;
    return real(debugTokenPreloadMain, argc, argv, init, fini, rtldFini,
                stackEnd);
}
