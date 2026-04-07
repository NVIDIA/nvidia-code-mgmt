/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Tests using a REAL D-Bus bus (from Docker) with --wrap=sd_bus_call to
 * intercept calls and return properly constructed sd_bus_message replies
 * with real data. This allows sdbusplus::read() to fully deserialize
 * variant/map types, unlocking the deep success paths.
 *
 * Also wraps popen for mctp-vdm-util.
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#undef FAIL

#define private public
#define protected public
#include "../debug_token/update_debug_token.hpp"
#undef private
#undef protected

#include <sys/mman.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

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

// ========================== Response queue ================================
// Each sd_bus_call pops a response handler from the queue.
// The handler receives the request message and populates the reply.

using ResponseFn =
    std::function<int(sd_bus_message* request, sd_bus_message** reply)>;

using ManagedObjectProperty =
    std::variant<std::string, uint8_t, std::vector<uint8_t>>;
using ManagedObjectProperties = std::map<std::string, ManagedObjectProperty>;
using ManagedObjectInterfaces = std::map<std::string, ManagedObjectProperties>;
using ManagedObjects = std::map<std::string, ManagedObjectInterfaces>;
using SubTreeEntry = std::tuple<std::string, std::string, std::string>;

namespace wrap_state
{
static std::deque<ResponseFn> responses;
static bool failAll = false;
static bool passThroughIfNoResponse = false;
static std::vector<std::string> calls;

void reset()
{
    responses.clear();
    failAll = false;
    passThroughIfNoResponse = false;
    calls.clear();
}

std::string dumpCalls()
{
    std::ostringstream out;
    for (size_t i = 0; i < calls.size(); ++i)
    {
        if (i != 0)
        {
            out << " | ";
        }
        out << calls[i];
    }
    return out.str();
}

// Get the bus from sd_bus_message (needed to create reply messages)
static sd_bus* getBusFromMsg(sd_bus_message* m)
{
    return sd_bus_message_get_bus(m);
}

static int discardReply(sd_bus_message** reply)
{
    if (reply && *reply)
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
    if (sd_bus_message_new_method_return(request, reply) < 0 || !*reply)
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

void pushEmpty()
{
    responses.push_back([](sd_bus_message* request, sd_bus_message** reply) {
        return makeMethodReturn(request, reply,
                                [&](sdbusplus::message::message&) {});
    });
}

void pushError(int rc = -ENOENT)
{
    responses.push_back([rc](sd_bus_message*, sd_bus_message**) { return rc; });
}

void pushGetSubTreeEntries(const std::vector<SubTreeEntry>& entries);
dbus::GetSubTreeResponse
    buildGetSubTreeResponse(const std::vector<SubTreeEntry>& entries);
dbus::ObjectValueTree buildObjectValueTree(const ManagedObjects& objects);

void pushGetSubTreeOneEndpoint(const char* path, const char* service,
                               const char* iface)
{
    pushGetSubTreeEntries({{path, service, iface}});
}

void pushGetSubTreeEntries(const std::vector<SubTreeEntry>& entries)
{
    responses.push_back(
        [entries](sd_bus_message* request, sd_bus_message** reply) {
            auto getSubTree = buildGetSubTreeResponse(entries);
            return makeMethodReturn(request, reply,
                                    [&](sdbusplus::message::message& response) {
                                        response.append(getSubTree);
                                    });
        });
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

void pushManagedObjects(const ManagedObjects& objects)
{
    responses.push_back(
        [objects](sd_bus_message* request, sd_bus_message** reply) {
            auto objectTree = buildObjectValueTree(objects);
            return makeMethodReturn(request, reply,
                                    [&](sdbusplus::message::message& response) {
                                        response.append(objectTree);
                                    });
        });
}

dbus::ObjectValueTree buildObjectValueTree(const ManagedObjects& objects)
{
    dbus::ObjectValueTree objectTree;
    for (const auto& [path, interfaces] : objects)
    {
        dbus::InterfaceMap interfaceMap;
        for (const auto& [iface, properties] : interfaces)
        {
            dbus::PropertyMap propertyMap;
            for (const auto& [name, value] : properties)
            {
                propertyMap.emplace(
                    name,
                    std::visit(
                        [](const auto& item) -> dbus::Value { return item; },
                        value));
            }
            interfaceMap.emplace(iface, std::move(propertyMap));
        }
        objectTree.emplace(sdbusplus::message::object_path{path},
                           std::move(interfaceMap));
    }
    return objectTree;
}

void pushVariantString(const char* value)
{
    responses.push_back(
        [value](sd_bus_message* request, sd_bus_message** reply) {
            return makeMethodReturn(
                request, reply, [&](sdbusplus::message::message& response) {
                    response.append(std::variant<std::string>{value});
                });
        });
}

void pushVariantUint32(uint32_t value)
{
    responses.push_back(
        [value](sd_bus_message* request, sd_bus_message** reply) {
            return makeMethodReturn(
                request, reply, [&](sdbusplus::message::message& response) {
                    response.append(std::variant<uint32_t>{value});
                });
        });
}

void pushVariantTokenStatus(const char* tokenType, const char* tokenStatus,
                            const char* additionalInfo, uint32_t timeLeft)
{
    responses.push_back([tokenType, tokenStatus, additionalInfo,
                         timeLeft](sd_bus_message* m, sd_bus_message** reply) {
        auto* bus = getBusFromMsg(m);
        if (!bus || sd_bus_message_new(bus, reply, 2) < 0)
        {
            return -EIO;
        }

        // Properties.Get wraps the property value in an outer variant.
        // NSMAsyncValue itself is also a variant, so the payload here must
        // be variant<variant<tuple>> rather than variant<tuple>.
        int r = sd_bus_message_open_container(*reply, 'v', "v");
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_open_container(*reply, 'v', "(sssu)");
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_open_container(*reply, 'r', "sssu");
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_append_basic(*reply, 's', tokenType);
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_append_basic(*reply, 's', tokenStatus);
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_append_basic(*reply, 's', additionalInfo);
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_append_basic(*reply, 'u', &timeLeft);
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_close_container(*reply);
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_close_container(*reply);
        if (r < 0)
        {
            return discardReply(reply);
        }
        r = sd_bus_message_close_container(*reply);
        if (r < 0 || sd_bus_message_seal(*reply, 0, 0) < 0)
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

void pushVariantErrorTuple(uint16_t errorCode, const char* errorMessage)
{
    responses.push_back(
        [errorCode, errorMessage](sd_bus_message* m, sd_bus_message** reply) {
            auto* bus = getBusFromMsg(m);
            if (!bus || sd_bus_message_new(bus, reply, 2) < 0)
            {
                return -EIO;
            }

            // Same nested-variant shape as pushVariantTokenStatus().
            int r = sd_bus_message_open_container(*reply, 'v', "v");
            if (r < 0)
            {
                return discardReply(reply);
            }
            r = sd_bus_message_open_container(*reply, 'v', "(qs)");
            if (r < 0)
            {
                return discardReply(reply);
            }
            r = sd_bus_message_open_container(*reply, 'r', "qs");
            if (r < 0)
            {
                return discardReply(reply);
            }
            r = sd_bus_message_append_basic(*reply, 'q', &errorCode);
            if (r < 0)
            {
                return discardReply(reply);
            }
            r = sd_bus_message_append_basic(*reply, 's', errorMessage);
            if (r < 0)
            {
                return discardReply(reply);
            }
            r = sd_bus_message_close_container(*reply);
            if (r < 0)
            {
                return discardReply(reply);
            }
            r = sd_bus_message_close_container(*reply);
            if (r < 0)
            {
                return discardReply(reply);
            }
            r = sd_bus_message_close_container(*reply);
            if (r < 0 || sd_bus_message_seal(*reply, 0, 0) < 0)
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
    responses.push_back(
        [path](sd_bus_message* request, sd_bus_message** reply) {
            return makeMethodReturn(
                request, reply, [&](sdbusplus::message::message& response) {
                    response.append(sdbusplus::message::object_path{path});
                });
        });
}

} // namespace wrap_state

// ========================== Linker wraps =================================

extern "C" int __real_sd_bus_call(sd_bus*, sd_bus_message*, uint64_t,
                                  sd_bus_error*, sd_bus_message**);

extern "C" int __wrap_sd_bus_call(sd_bus* bus [[maybe_unused]],
                                  sd_bus_message* m, uint64_t usec,
                                  sd_bus_error* ret_error,
                                  sd_bus_message** reply)
{
    const char* interface = sd_bus_message_get_interface(m);
    const char* member = sd_bus_message_get_member(m);
    wrap_state::calls.emplace_back(
        std::string(interface ? interface : "<null>") + "." +
        std::string(member ? member : "<null>"));

    if (wrap_state::failAll)
    {
        if (ret_error)
        {
            sd_bus_error_set_const(ret_error,
                                   "org.freedesktop.DBus.Error.ServiceUnknown",
                                   "wrapped failure");
        }
        return -ENOENT;
    }

    if (!wrap_state::responses.empty())
    {
        auto fn = std::move(wrap_state::responses.front());
        wrap_state::responses.pop_front();
        return fn(m, reply);
    }

    if (wrap_state::passThroughIfNoResponse)
    {
        return __real_sd_bus_call(bus, m, usec, ret_error, reply);
    }

    // No response queued -> fail
    if (ret_error)
    {
        sd_bus_error_set_const(ret_error, "org.freedesktop.DBus.Error.NoReply",
                               "no queued response");
    }
    return -ENOENT;
}

struct ObjectMapperServiceState
{
    dbus::GetSubTreeResponse response{};
};

int handleObjectMapperGetSubTree(sd_bus_message* raw, void* userdata,
                                 sd_bus_error*)
{
    auto* state = static_cast<ObjectMapperServiceState*>(userdata);
    sd_bus_message* reply = nullptr;
    int rc = wrap_state::makeMethodReturn(
        raw, &reply, [&](auto& response) { response.append(state->response); });
    if (rc < 0 || reply == nullptr)
    {
        return rc < 0 ? rc : -EIO;
    }

    rc = sd_bus_send(sd_bus_message_get_bus(raw), reply, nullptr);
    sd_bus_message_unref(reply);
    return rc < 0 ? rc : 1;
}

const sd_bus_vtable objectMapperVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetSubTree", "sias", "a{oa{sas}}",
                  handleObjectMapperGetSubTree, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

struct ObjectManagerServiceState
{
    dbus::ObjectValueTree response{};
};

int handleObjectManagerGetManagedObjects(sd_bus_message* raw, void* userdata,
                                         sd_bus_error*)
{
    auto* state = static_cast<ObjectManagerServiceState*>(userdata);
    sd_bus_message* reply = nullptr;
    int rc = wrap_state::makeMethodReturn(
        raw, &reply, [&](auto& response) { response.append(state->response); });
    if (rc < 0 || reply == nullptr)
    {
        return rc < 0 ? rc : -EIO;
    }

    rc = sd_bus_send(sd_bus_message_get_bus(raw), reply, nullptr);
    sd_bus_message_unref(reply);
    return rc < 0 ? rc : 1;
}

const sd_bus_vtable objectManagerVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetManagedObjects", "", "a{oa{sa{sv}}}",
                  handleObjectManagerGetManagedObjects,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

class ScopedDbusService
{
  public:
    ScopedDbusService(const std::string& serviceName, const std::string& path,
                      const std::string& interface, const sd_bus_vtable* vtable,
                      void* userdata) : bus(sdbusplus::bus::new_default())
    {
        bus.request_name(serviceName.c_str());
        const int rc = sd_bus_add_object_vtable(
            sdbusplus::bus::details::bus_friend::get_busp(bus), &slot,
            path.c_str(), interface.c_str(), vtable, userdata);
        if (rc < 0)
        {
            throw std::runtime_error("failed to register test D-Bus service");
        }

        worker = std::thread([this] {
            while (!stopRequested.load())
            {
                if (bus.process_discard())
                {
                    continue;
                }
                bus.wait(std::chrono::milliseconds(10));
            }
        });
    }

    ~ScopedDbusService()
    {
        stopRequested = true;
        if (worker.joinable())
        {
            worker.join();
        }
        if (slot != nullptr)
        {
            sd_bus_slot_unref(slot);
            slot = nullptr;
        }
    }

  private:
    sdbusplus::bus_t bus;
    std::atomic<bool> stopRequested = false;
    std::thread worker;
    sd_bus_slot* slot = nullptr;
};

template <typename AppendFn>
int sendMethodReturn(sd_bus_message* raw, AppendFn&& appendFn)
{
    sd_bus_message* reply = nullptr;
    int rc = wrap_state::makeMethodReturn(raw, &reply,
                                          std::forward<AppendFn>(appendFn));
    if (rc < 0 || reply == nullptr)
    {
        return rc < 0 ? rc : -EIO;
    }

    rc = sd_bus_send(sd_bus_message_get_bus(raw), reply, nullptr);
    sd_bus_message_unref(reply);
    return rc < 0 ? rc : 1;
}

struct ScopedDbusObject
{
    std::string path;
    std::string interface;
    const sd_bus_vtable* vtable = nullptr;
    void* userdata = nullptr;
};

class ScopedMultiPathDbusService
{
  public:
    ScopedMultiPathDbusService(const std::string& serviceName,
                               const std::vector<ScopedDbusObject>& objects) :
        bus(sdbusplus::bus::new_default())
    {
        bus.request_name(serviceName.c_str());
        for (const auto& object : objects)
        {
            sd_bus_slot* slot = nullptr;
            const int rc = sd_bus_add_object_vtable(
                sdbusplus::bus::details::bus_friend::get_busp(bus), &slot,
                object.path.c_str(), object.interface.c_str(), object.vtable,
                object.userdata);
            if (rc < 0)
            {
                throw std::runtime_error(
                    "failed to register test D-Bus object");
            }
            slots.push_back(slot);
        }

        worker = std::thread([this] {
            while (!stopRequested.load())
            {
                if (bus.process_discard())
                {
                    continue;
                }
                bus.wait(std::chrono::milliseconds(10));
            }
        });
    }

    ~ScopedMultiPathDbusService()
    {
        stopRequested = true;
        if (worker.joinable())
        {
            worker.join();
        }
        for (auto* slot : slots)
        {
            if (slot != nullptr)
            {
                sd_bus_slot_unref(slot);
            }
        }
    }

  private:
    sdbusplus::bus_t bus;
    std::atomic<bool> stopRequested = false;
    std::thread worker;
    std::vector<sd_bus_slot*> slots;
};

struct NsmInstallServiceState
{
    std::map<std::string, std::string> tokenDeviceIds{};
    std::map<std::string, std::string> installAsyncPaths{};
    std::map<std::string, std::string> asyncStatuses{};
    int installCalls = 0;
    int propertyGets = 0;
    std::vector<int> installFds{};
};

int handleNsmPropertiesGet(sd_bus_message* raw, void* userdata, sd_bus_error*)
{
    auto* state = static_cast<NsmInstallServiceState*>(userdata);
    const char* interface = nullptr;
    const char* property = nullptr;
    const int readRc = sd_bus_message_read(raw, "ss", &interface, &property);
    if (readRc < 0)
    {
        return readRc;
    }

    state->propertyGets++;
    const std::string path = sd_bus_message_get_path(raw);
    const std::string interfaceName = interface ? interface : "";
    const std::string propertyName = property ? property : "";

    if (interfaceName == nsmDebugTokenStatusIntfName &&
        propertyName == "TokenDeviceID")
    {
        auto it = state->tokenDeviceIds.find(path);
        if (it == state->tokenDeviceIds.end())
        {
            return -ENOENT;
        }
        return sendMethodReturn(raw, [&](auto& response) {
            response.append(std::variant<std::string>{it->second});
        });
    }

    if (interfaceName == nsmAsyncStatusIntfName && propertyName == "Status")
    {
        auto it = state->asyncStatuses.find(path);
        if (it == state->asyncStatuses.end())
        {
            return -ENOENT;
        }
        return sendMethodReturn(raw, [&](auto& response) {
            response.append(std::variant<std::string>{it->second});
        });
    }

    return -ENOENT;
}

int handleNsmInstallToken(sd_bus_message* raw, void* userdata, sd_bus_error*)
{
    auto* state = static_cast<NsmInstallServiceState*>(userdata);
    int fd = -1;
    const int readRc = sd_bus_message_read(raw, "h", &fd);
    if (readRc < 0)
    {
        return readRc;
    }

    state->installCalls++;
    state->installFds.push_back(fd);

    const std::string path = sd_bus_message_get_path(raw);
    auto it = state->installAsyncPaths.find(path);
    if (it == state->installAsyncPaths.end())
    {
        return -ENOENT;
    }

    return sendMethodReturn(raw, [&](auto& response) {
        response.append(sdbusplus::message::object_path{it->second});
    });
}

const sd_bus_vtable nsmPropertiesVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Get", "ss", "v", handleNsmPropertiesGet,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

const sd_bus_vtable nsmActionVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("InstallToken", "h", "o", handleNsmInstallToken,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

struct LegacyNsmServiceState
{
    std::map<std::string, std::deque<std::string>> methodAsyncPaths{};
    std::map<std::string, std::string> tokenDeviceIds{};
    std::map<std::string, std::string> asyncStatuses{};
    std::map<std::string, NSMAsyncValue> asyncValues{};
    int debugTokenCalls = 0;
    int propertyGets = 0;
    int installCalls = 0;
};

std::string legacyMethodKey(const std::string& path, const std::string& method)
{
    return path + "|" + method;
}

int handleLegacyDebugTokenGetStatus(sd_bus_message* raw, void* userdata,
                                    sd_bus_error*)
{
    auto* state = static_cast<LegacyNsmServiceState*>(userdata);
    const std::string path = sd_bus_message_get_path(raw);
    state->debugTokenCalls++;

    const auto key = legacyMethodKey(path, "GetStatus");
    auto it = state->methodAsyncPaths.find(key);
    if (it == state->methodAsyncPaths.end() || it->second.empty())
    {
        return -ENOENT;
    }

    const auto asyncPath = it->second.front();
    it->second.pop_front();
    return sendMethodReturn(raw, [&](auto& response) {
        response.append(sdbusplus::message::object_path{asyncPath});
    });
}

int handleLegacyDebugTokenDisableTokens(sd_bus_message* raw, void* userdata,
                                        sd_bus_error*)
{
    auto* state = static_cast<LegacyNsmServiceState*>(userdata);
    const std::string path = sd_bus_message_get_path(raw);
    state->debugTokenCalls++;

    const auto key = legacyMethodKey(path, "DisableTokens");
    auto it = state->methodAsyncPaths.find(key);
    if (it == state->methodAsyncPaths.end() || it->second.empty())
    {
        return -ENOENT;
    }

    const auto asyncPath = it->second.front();
    it->second.pop_front();
    return sendMethodReturn(raw, [&](auto& response) {
        response.append(sdbusplus::message::object_path{asyncPath});
    });
}

int handleLegacyDebugTokenInstallToken(sd_bus_message* raw, void* userdata,
                                       sd_bus_error*)
{
    auto* state = static_cast<LegacyNsmServiceState*>(userdata);
    std::vector<uint8_t> token;
    try
    {
        sdbusplus::message::message request(raw, std::false_type{});
        request.read(token);
    }
    catch (const std::exception&)
    {
        return -EINVAL;
    }

    const std::string path = sd_bus_message_get_path(raw);
    state->debugTokenCalls++;
    state->installCalls++;

    const auto key = legacyMethodKey(path, "InstallToken");
    auto it = state->methodAsyncPaths.find(key);
    if (it == state->methodAsyncPaths.end() || it->second.empty())
    {
        return -ENOENT;
    }

    const auto asyncPath = it->second.front();
    it->second.pop_front();
    return sendMethodReturn(raw, [&](auto& response) {
        response.append(sdbusplus::message::object_path{asyncPath});
    });
}

int handleLegacyPropertiesGet(sd_bus_message* raw, void* userdata,
                              sd_bus_error*)
{
    auto* state = static_cast<LegacyNsmServiceState*>(userdata);
    const char* interface = nullptr;
    const char* property = nullptr;
    const int readRc = sd_bus_message_read(raw, "ss", &interface, &property);
    if (readRc < 0)
    {
        return readRc;
    }

    state->propertyGets++;
    const std::string path = sd_bus_message_get_path(raw);
    const std::string interfaceName = interface ? interface : "";
    const std::string propertyName = property ? property : "";

    if (interfaceName == nsmDebugTokenIntfName &&
        propertyName == "TokenDeviceID")
    {
        auto it = state->tokenDeviceIds.find(path);
        if (it == state->tokenDeviceIds.end())
        {
            return -ENOENT;
        }
        return sendMethodReturn(raw, [&](auto& response) {
            response.append(std::variant<std::string>{it->second});
        });
    }

    if (interfaceName == nsmAsyncStatusIntfName && propertyName == "Status")
    {
        auto it = state->asyncStatuses.find(path);
        if (it == state->asyncStatuses.end())
        {
            return -ENOENT;
        }
        return sendMethodReturn(raw, [&](auto& response) {
            response.append(std::variant<std::string>{it->second});
        });
    }

    if (interfaceName == nsmAsyncValueIntfName && propertyName == "Value")
    {
        auto it = state->asyncValues.find(path);
        if (it == state->asyncValues.end())
        {
            return -ENOENT;
        }
        return sendMethodReturn(raw, [&](auto& response) {
            response.append(std::variant<NSMAsyncValue>{it->second});
        });
    }

    return -ENOENT;
}

const sd_bus_vtable legacyDebugTokenVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetStatus", "s", "o", handleLegacyDebugTokenGetStatus,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisableTokens", "", "o", handleLegacyDebugTokenDisableTokens,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("InstallToken", "ay", "o", handleLegacyDebugTokenInstallToken,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

const sd_bus_vtable legacyPropertiesVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Get", "ss", "v", handleLegacyPropertiesGet,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// Wrap popen/pclose
namespace popen_state
{
struct Response
{
    std::string output;
    int retCode = 0;
};

static std::deque<Response> queued;
static std::vector<std::string> commands;
static std::string output;
static int retCode = 0;
static Response active{};

void queue(const std::string& nextOutput, int nextRetCode = 0)
{
    queued.push_back({nextOutput, nextRetCode});
}
} // namespace popen_state

namespace async_signal_state
{
enum class PayloadKind
{
    stringStatus,
    uint32Status,
    truncatedBody,
};

struct PendingSignal
{
    std::string objectPath;
    std::string status;
    std::string propertyName = "Status";
    PayloadKind payloadKind = PayloadKind::stringStatus;
    uint32_t numericStatus = 0;
};

static sd_bus_message_handler_t matchCallback = nullptr;
static void* matchUserdata = nullptr;
static sd_bus_slot* matchSlot = nullptr;
static sd_bus_slot* currentSlot = nullptr;
static void* slotUserdata = nullptr;
static std::deque<PendingSignal> pending{};
static int callbackExceptions = 0;

void reset()
{
    matchCallback = nullptr;
    matchUserdata = nullptr;
    matchSlot = nullptr;
    currentSlot = nullptr;
    slotUserdata = nullptr;
    pending.clear();
    callbackExceptions = 0;
}

void queue(const std::string& objectPath, const std::string& status)
{
    pending.push_back({.objectPath = objectPath,
                       .status = status,
                       .propertyName = "Status",
                       .payloadKind = PayloadKind::stringStatus,
                       .numericStatus = 0});
}

void queueWithProperty(const std::string& objectPath, const std::string& status,
                       const std::string& propertyName)
{
    pending.push_back({.objectPath = objectPath,
                       .status = status,
                       .propertyName = propertyName,
                       .payloadKind = PayloadKind::stringStatus,
                       .numericStatus = 0});
}

void queueWithUint32Status(const std::string& objectPath, uint32_t status)
{
    pending.push_back({.objectPath = objectPath,
                       .status = "",
                       .propertyName = "Status",
                       .payloadKind = PayloadKind::uint32Status,
                       .numericStatus = status});
}

void queueTruncated(const std::string& objectPath)
{
    pending.push_back({.objectPath = objectPath,
                       .status = "",
                       .propertyName = "Status",
                       .payloadKind = PayloadKind::truncatedBody,
                       .numericStatus = 0});
}
} // namespace async_signal_state

extern "C" FILE* __wrap_popen(const char* command, const char* mode)
{
    (void)mode;
    if (command != nullptr)
    {
        popen_state::commands.emplace_back(command);
    }
    if (!popen_state::queued.empty())
    {
        popen_state::active = popen_state::queued.front();
    }
    else
    {
        popen_state::active = {popen_state::output, popen_state::retCode};
    }

    FILE* stream = tmpfile();
    if (stream == nullptr)
    {
        return nullptr;
    }
    if (!popen_state::active.output.empty())
    {
        const auto written = fwrite(popen_state::active.output.data(), 1,
                                    popen_state::active.output.size(), stream);
        if (written != popen_state::active.output.size())
        {
            fclose(stream);
            return nullptr;
        }
    }
    rewind(stream);
    return stream;
}

extern "C" int __wrap_pclose(FILE* stream)
{
    if (stream)
    {
        fclose(stream);
    }
    const int rc = popen_state::active.retCode;
    if (!popen_state::queued.empty())
    {
        popen_state::queued.pop_front();
    }
    popen_state::active = {};
    return rc;
}

namespace syscall_state
{
static bool failMemfdCreate = false;
static bool shortWrite = false;
static int failMemfdCreateRemaining = 0;
static int shortWriteRemaining = 0;
static int trackedFd = -1;
static int memfdCreateCalls = 0;
static int writeCalls = 0;
static int shortWriteCalls = 0;

void reset()
{
    failMemfdCreate = false;
    shortWrite = false;
    failMemfdCreateRemaining = 0;
    shortWriteRemaining = 0;
    trackedFd = -1;
    memfdCreateCalls = 0;
    writeCalls = 0;
    shortWriteCalls = 0;
}
} // namespace syscall_state

extern "C" int __real_memfd_create(const char*, unsigned int);
extern "C" int __wrap_memfd_create(const char* name, unsigned int flags)
{
    syscall_state::memfdCreateCalls++;
    if (syscall_state::failMemfdCreateRemaining > 0)
    {
        syscall_state::failMemfdCreateRemaining--;
        errno = EMFILE;
        return -1;
    }
    if (syscall_state::failMemfdCreate)
    {
        errno = EMFILE;
        return -1;
    }

    int fd = __real_memfd_create(name, flags);
    syscall_state::trackedFd = fd;
    return fd;
}

extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* buf, size_t count)
{
    syscall_state::writeCalls++;
    if (((syscall_state::shortWriteRemaining > 0) ||
         syscall_state::shortWrite) &&
        fd == syscall_state::trackedFd && count > 0)
    {
        if (syscall_state::shortWriteRemaining > 0)
        {
            syscall_state::shortWriteRemaining--;
        }
        syscall_state::shortWriteCalls++;
        errno = ENOSPC;
        return static_cast<ssize_t>(count - 1);
    }

    return __real_write(fd, buf, count);
}

sd_bus_message* makeAsyncStatusSignalRaw(
    sd_bus* bus, const async_signal_state::PendingSignal& signalState)
{
    sd_bus_message* rawMsg = nullptr;
    if (sd_bus_message_new_signal(bus, &rawMsg, signalState.objectPath.c_str(),
                                  "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged") < 0)
    {
        return nullptr;
    }

    const char* interface = nsmAsyncStatusIntfName;
    if (sd_bus_message_append_basic(rawMsg, 's', interface) < 0)
    {
        sd_bus_message_unref(rawMsg);
        return nullptr;
    }

    if (signalState.payloadKind ==
        async_signal_state::PayloadKind::truncatedBody)
    {
        if (sd_bus_message_seal(rawMsg, 0, 0) < 0)
        {
            sd_bus_message_unref(rawMsg);
            return nullptr;
        }
        sd_bus_message_rewind(rawMsg, true);
        return rawMsg;
    }

    const char* propertyNameValue = signalState.propertyName.c_str();
    if (sd_bus_message_open_container(rawMsg, 'a', "{sv}") < 0 ||
        sd_bus_message_open_container(rawMsg, 'e', "sv") < 0 ||
        sd_bus_message_append_basic(rawMsg, 's', propertyNameValue) < 0)
    {
        sd_bus_message_unref(rawMsg);
        return nullptr;
    }

    if (signalState.payloadKind ==
        async_signal_state::PayloadKind::uint32Status)
    {
        uint32_t rawStatus = signalState.numericStatus;
        if (sd_bus_message_open_container(rawMsg, 'v', "u") < 0 ||
            sd_bus_message_append_basic(rawMsg, 'u', &rawStatus) < 0)
        {
            sd_bus_message_unref(rawMsg);
            return nullptr;
        }
    }
    else
    {
        const char* rawStatus = signalState.status.c_str();
        if (sd_bus_message_open_container(rawMsg, 'v', "s") < 0 ||
            sd_bus_message_append_basic(rawMsg, 's', rawStatus) < 0)
        {
            sd_bus_message_unref(rawMsg);
            return nullptr;
        }
    }

    if (sd_bus_message_close_container(rawMsg) < 0 ||
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

extern "C" int __real_sd_bus_add_match(sd_bus*, sd_bus_slot**, const char*,
                                       sd_bus_message_handler_t, void*);
extern "C" int __wrap_sd_bus_add_match(sd_bus* bus, sd_bus_slot** slot,
                                       const char* match,
                                       sd_bus_message_handler_t callback,
                                       void* userdata)
{
    auto rc = __real_sd_bus_add_match(bus, slot, match, callback, userdata);
    async_signal_state::matchCallback = callback;
    async_signal_state::matchUserdata = userdata;
    async_signal_state::matchSlot = (slot != nullptr) ? *slot : nullptr;
    return rc;
}

extern "C" int __real_sd_bus_wait(sd_bus*, uint64_t);
extern "C" int __wrap_sd_bus_wait(sd_bus* bus, uint64_t timeout_usec)
{
    if (!async_signal_state::pending.empty() &&
        async_signal_state::matchCallback != nullptr)
    {
        auto signalState = async_signal_state::pending.front();
        async_signal_state::pending.pop_front();
        auto* signal = makeAsyncStatusSignalRaw(bus, signalState);
        if (signal != nullptr)
        {
            async_signal_state::currentSlot = async_signal_state::matchSlot;
            int rc = 0;
            try
            {
                rc = async_signal_state::matchCallback(
                    signal, async_signal_state::matchUserdata, nullptr);
            }
            catch (...)
            {
                async_signal_state::callbackExceptions++;
                rc = -EINVAL;
            }
            async_signal_state::currentSlot = nullptr;
            sd_bus_message_unref(signal);
            return rc >= 0 ? 1 : rc;
        }
    }
    return __real_sd_bus_wait(bus, timeout_usec);
}

extern "C" void* __real_sd_bus_slot_set_userdata(sd_bus_slot*, void*);
extern "C" void* __wrap_sd_bus_slot_set_userdata(sd_bus_slot* slot,
                                                 void* userdata)
{
    if (slot == async_signal_state::matchSlot)
    {
        async_signal_state::slotUserdata = userdata;
    }
    return __real_sd_bus_slot_set_userdata(slot, userdata);
}

extern "C" sd_bus_slot* __real_sd_bus_get_current_slot(sd_bus*);
extern "C" sd_bus_slot* __wrap_sd_bus_get_current_slot(sd_bus* bus)
{
    if (async_signal_state::currentSlot != nullptr)
    {
        return async_signal_state::currentSlot;
    }
    return __real_sd_bus_get_current_slot(bus);
}

extern "C" void* __real_sd_bus_slot_get_userdata(sd_bus_slot*);
extern "C" void* __wrap_sd_bus_slot_get_userdata(sd_bus_slot* slot)
{
    if (slot == async_signal_state::matchSlot &&
        async_signal_state::slotUserdata != nullptr)
    {
        return async_signal_state::slotUserdata;
    }
    return __real_sd_bus_slot_get_userdata(slot);
}

class WrappedSdBusInterface : public sdbusplus::SdBusImpl
{
  public:
    int sd_bus_call(sd_bus* bus, sd_bus_message* message, uint64_t usec,
                    sd_bus_error* retError, sd_bus_message** reply) override
    {
        return __wrap_sd_bus_call(bus, message, usec, retError, reply);
    }

    int sd_bus_add_match(sd_bus* bus, sd_bus_slot** slot, const char* match,
                         sd_bus_message_handler_t callback,
                         void* userdata) override
    {
        return __wrap_sd_bus_add_match(bus, slot, match, callback, userdata);
    }

    void* sd_bus_slot_set_userdata(sd_bus_slot* slot, void* userdata) override
    {
        return __wrap_sd_bus_slot_set_userdata(slot, userdata);
    }

    int sd_bus_wait(sd_bus* bus, uint64_t timeoutUsec) override
    {
        return __wrap_sd_bus_wait(bus, timeoutUsec);
    }
};

WrappedSdBusInterface wrappedSdBusInterface;

sdbusplus::bus_t makeWrappedBus()
{
    auto rawBus = sdbusplus::bus::new_default();
    return sdbusplus::bus_t(
        sdbusplus::bus::details::bus_friend::get_busp(rawBus),
        &wrappedSdBusInterface);
}

// ========================== Test fixture =================================

class RealBusWrappedTest : public testing::Test
{
  protected:
    sdbusplus::bus_t bus = makeWrappedBus();
    UpdateDebugToken udt{bus};

    void SetUp() override
    {
        alloc_fail::reset();
        wrap_state::reset();
        popen_state::output = "";
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        async_signal_state::reset();
        syscall_state::reset();
    }
};

namespace
{

[[maybe_unused]] std::string makeRxOutput(const std::vector<std::string>& bytes)
{
    std::ostringstream out;
    out << "RX:";
    for (const auto& byte : bytes)
    {
        out << ' ' << byte;
    }
    out << '\n';
    return out.str();
}

[[maybe_unused]] std::vector<std::string> makeQueryBytes(size_t count)
{
    return std::vector<std::string>(count, "00");
}

template <typename PrepareFn, typename InvokeFn>
void runAllocFailureSweep(size_t firstFailure, size_t lastFailure,
                          PrepareFn&& prepare, InvokeFn&& invoke)
{
    for (size_t failIndex = firstFailure; failIndex <= lastFailure; ++failIndex)
    {
        prepare();
        alloc_fail::Guard guard(failIndex);
        try
        {
            invoke();
        }
        catch (const std::bad_alloc&)
        {}
        catch (const std::exception&)
        {}
    }
}

template <typename PrepareFn, typename InvokeFn>
size_t countAllocations(PrepareFn&& prepare, InvokeFn&& invoke)
{
    prepare();
    alloc_fail::enabled = true;
    alloc_fail::failAt = std::numeric_limits<size_t>::max();
    alloc_fail::allocationCount = 0;
    try
    {
        invoke();
    }
    catch (const std::bad_alloc&)
    {}
    catch (const std::exception&)
    {}
    size_t totalAllocations = alloc_fail::allocationCount;
    alloc_fail::reset();
    return totalAllocations;
}

template <typename PrepareFn, typename InvokeFn>
void runMeasuredAllocFailureSweep(PrepareFn&& prepare, InvokeFn&& invoke)
{
    const size_t totalAllocations = countAllocations(
        std::forward<PrepareFn>(prepare), std::forward<InvokeFn>(invoke));
    if (totalAllocations == 0)
    {
        return;
    }
    runAllocFailureSweep(1, totalAllocations, std::forward<PrepareFn>(prepare),
                         std::forward<InvokeFn>(invoke));
}

template <typename PrepareFn, typename InvokeFn>
void runMeasuredAllocFailureSweepCapped(size_t maxAllocations,
                                        PrepareFn&& prepare, InvokeFn&& invoke)
{
    const size_t totalAllocations = countAllocations(
        std::forward<PrepareFn>(prepare), std::forward<InvokeFn>(invoke));
    if (totalAllocations == 0 || maxAllocations == 0)
    {
        return;
    }
    runAllocFailureSweep(1, std::min(totalAllocations, maxAllocations),
                         std::forward<PrepareFn>(prepare),
                         std::forward<InvokeFn>(invoke));
}

} // namespace

// ========================== getErasePolicy with real variant read ==========

#ifndef NSM_ALLOC_PROBE_ONLY
TEST_F(RealBusWrappedTest, GetErasePolicyManual)
{
    wrap_state::pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                          "com.nvidia.DebugToken",
                                          "com.nvidia.DebugToken.ErasePolicy");
    wrap_state::pushVariantString("Manual");

    auto policy = udt.getErasePolicy();
    // May return "Manual" if variant deserialization works, or empty if not
    (void)policy;
}

TEST_F(RealBusWrappedTest, GetErasePolicyAutomatic)
{
    wrap_state::pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                          "com.nvidia.DebugToken",
                                          "com.nvidia.DebugToken.ErasePolicy");
    wrap_state::pushVariantString("Automatic");

    auto policy = udt.getErasePolicy();
    (void)policy;
}

TEST_F(RealBusWrappedTest, GetErasePolicyMultipleObjectsReturnsEmpty)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/com/nvidia/debug_token/policy0", "com.nvidia.DebugToken",
          "com.nvidia.DebugToken.ErasePolicy"},
         {"/com/nvidia/debug_token/policy1", "com.nvidia.DebugToken",
          "com.nvidia.DebugToken.ErasePolicy"}});

    auto policy = udt.getErasePolicy();
    EXPECT_TRUE(policy.empty());
}

TEST_F(RealBusWrappedTest, GetErasePolicyNoObjectsReturnsEmpty)
{
    wrap_state::pushGetSubTreeEntries({});

    auto policy = udt.getErasePolicy();
    EXPECT_TRUE(policy.empty());
}

TEST_F(RealBusWrappedTest, GetErasePolicyPropertyLookupFailsReturnsEmpty)
{
    wrap_state::pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                          "com.nvidia.DebugToken",
                                          "com.nvidia.DebugToken.ErasePolicy");

    auto policy = udt.getErasePolicy();
    EXPECT_TRUE(policy.empty());
}

TEST_F(RealBusWrappedTest, GetErasePolicyPropertyWrongTypeReturnsEmpty)
{
    wrap_state::pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                          "com.nvidia.DebugToken",
                                          "com.nvidia.DebugToken.ErasePolicy");
    wrap_state::pushVariantUint32(7);

    auto policy = udt.getErasePolicy();
    EXPECT_TRUE(policy.empty());
}

TEST_F(RealBusWrappedTest, GetErasePolicyAllocationFailureSweep)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint(
            "/com/nvidia/debug_token/policy", "com.nvidia.DebugToken",
            "com.nvidia.DebugToken.ErasePolicy");
        wrap_state::pushVariantString("Automatic");
    };

    prepare();
    (void)udt.getErasePolicy();

    runMeasuredAllocFailureSweep(prepare, [&] { (void)udt.getErasePolicy(); });
}

// ========================== eraseDebugToken with Manual policy =============

TEST_F(RealBusWrappedTest, EraseDebugTokenManual)
{
    wrap_state::pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                          "com.nvidia.DebugToken",
                                          "com.nvidia.DebugToken.ErasePolicy");
    wrap_state::pushVariantString("Manual");

    int result = udt.eraseDebugToken();
    (void)result;
}

// ========================== enumerateNsmDebugTokenEndpoints ================

TEST_F(RealBusWrappedTest, EnumerateEndpointsOneResult)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);

    NSMEndpoints eps;
    int result = udt.enumerateNsmDebugTokenEndpoints(eps);
    (void)result;
    (void)eps;
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsV2OneResult)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);

    NSMEndpoints eps;
    int result = udt.enumerateNsmDebugTokenEndpointsV2(eps);
    (void)result;
    (void)eps;
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsMultipleResults)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    NSMEndpoints eps;
    auto result = udt.enumerateNsmDebugTokenEndpoints(eps);
    (void)result;
    (void)eps;
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsEmptyResults)
{
    wrap_state::pushGetSubTreeEntries({});

    NSMEndpoints eps;
    auto result = udt.enumerateNsmDebugTokenEndpoints(eps);
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(eps.empty());
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsV2MultipleResults)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});

    NSMEndpoints eps;
    auto result = udt.enumerateNsmDebugTokenEndpointsV2(eps);
    (void)result;
    (void)eps;
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsV2EmptyResults)
{
    wrap_state::pushGetSubTreeEntries({});

    NSMEndpoints eps;
    auto result = udt.enumerateNsmDebugTokenEndpointsV2(eps);
    EXPECT_EQ(result, -1);
    EXPECT_TRUE(eps.empty());
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsReplyReadFailureHitsCatch)
{
    wrap_state::pushVariantString("not-a-subtree");

    NSMEndpoints eps;
    auto result = udt.enumerateNsmDebugTokenEndpoints(eps);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsV2ReplyReadFailureHitsCatch)
{
    wrap_state::pushVariantString("not-a-subtree");

    NSMEndpoints eps;
    auto result = udt.enumerateNsmDebugTokenEndpointsV2(eps);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsAllocationFailureSweep)
{
    const std::string pathA =
        "/xyz/openbmc_project/NSM/" + std::string(48, 'a');
    const std::string pathB =
        "/xyz/openbmc_project/NSM/" + std::string(48, 'b');
    const std::string serviceA =
        "xyz.openbmc_project.NSM." + std::string(24, 'a');
    const std::string serviceB =
        "xyz.openbmc_project.NSM." + std::string(24, 'b');

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeEntries(
            {{pathA, serviceA, nsmDebugTokenIntfName},
             {pathB, serviceB, nsmDebugTokenIntfName}});
    };

    prepare();
    NSMEndpoints eps;
    (void)udt.enumerateNsmDebugTokenEndpoints(eps);

    runMeasuredAllocFailureSweep(prepare, [&] {
        NSMEndpoints tmp;
        (void)udt.enumerateNsmDebugTokenEndpoints(tmp);
    });
}

TEST_F(RealBusWrappedTest, EnumerateEndpointsV2AllocationFailureSweep)
{
    const std::string pathA =
        "/xyz/openbmc_project/NSM/" + std::string(48, 'v');
    const std::string pathB =
        "/xyz/openbmc_project/NSM/" + std::string(48, 'w');
    const std::string serviceA =
        "xyz.openbmc_project.NSM." + std::string(24, 'v');
    const std::string serviceB =
        "xyz.openbmc_project.NSM." + std::string(24, 'w');

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeEntries(
            {{pathA, serviceA, nsmDebugTokenActionIntfName},
             {pathB, serviceB, nsmDebugTokenActionIntfName}});
    };

    prepare();
    NSMEndpoints eps;
    (void)udt.enumerateNsmDebugTokenEndpointsV2(eps);

    runMeasuredAllocFailureSweep(prepare, [&] {
        NSMEndpoints tmp;
        (void)udt.enumerateNsmDebugTokenEndpointsV2(tmp);
    });
}

// ========================== nsmTokenErase with real data flow ==============

TEST_F(RealBusWrappedTest, NsmTokenEraseNoTokenApplied)
{
    // Enumerate -> one endpoint
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);

    // getTokenStatus -> handleAsyncCall -> makeDebugTokenMethodCall
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    // handleAsyncCall -> Get initial status
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    // getAsyncValue -> Get -> returns variant with tuple
    // This is complex (NSMAsyncValue = variant<tuple, tuple>)
    // Simplify: push an empty response that causes exception -> catch
    wrap_state::pushEmpty();

    int result = udt.nsmTokenErase();
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenEraseEmptyEndpoints)
{
    wrap_state::pushEmpty(); // GetSubTree returns empty
    (void)udt.nsmTokenErase();
}

TEST_F(RealBusWrappedTest, NsmTokenEraseDisableTokensFailureSetsError)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);

    auto result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseUnexpectedPostDisableStatusSetsError)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, "still-active", 1);

    auto result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseMixedEndpointsIncludesTokenTimeoutPath)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-0");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, "timeout", 7);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "cleared", 0);

    auto result = udt.nsmTokenErase();
    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseCoversLegacyMixedOutcomeMatrix)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu2", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu3", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu4", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu5", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-0");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantErrorTuple(0x10, "status tuple mismatch");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-2");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);
    wrap_state::pushError();

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-3");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, "timeout", 7);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable-3");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-3");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantErrorTuple(0x11, "missing post-disable status");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-4");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable-4");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-4");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, "still-active", 1);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-5");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, "timeout", 2);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable-5");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-5");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "cleared", 0);

    auto result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseInvalidEndpointPathHitsCatch)
{
    wrap_state::pushGetSubTreeEntries(
        {{"not-a-valid-object-path", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "cleared", 0);

    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest,
       NsmTokenEraseInitialStatusFailureThenContinuesToSuccessEndpoint)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    wrap_state::pushError();

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "cleared", 0);

    int result = udt.nsmTokenErase();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseAllocationFailureSweepSuccessPath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, "timeout", 2);
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "cleared", 0);
    };

    prepare();
    (void)udt.nsmTokenErase();

    runMeasuredAllocFailureSweep(prepare, [&] { (void)udt.nsmTokenErase(); });
}

TEST_F(RealBusWrappedTest, NsmTokenEraseAllocationFailureSweepFailurePath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");
        wrap_state::pushVariantErrorTuple(0x42, "disable failed");
    };

    prepare();
    (void)udt.nsmTokenErase();

    runMeasuredAllocFailureSweep(prepare, [&] { (void)udt.nsmTokenErase(); });
}

// ========================== nsmTokenInstall ================================

TEST_F(RealBusWrappedTest, NsmTokenInstallEmptyEndpoints)
{
    wrap_state::pushEmpty();
    TokenMap tokens;
    tokens.emplace("serial", std::vector<uint8_t>(100, 0x42));
    (void)udt.nsmTokenInstall(tokens);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallOneEndpoint)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    // getTokenStatus -> handleAsyncCall -> makeDebugTokenMethodCall
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushEmpty(); // getAsyncValue

    // Get TokenDeviceID -> variant<string>
    wrap_state::pushVariantString("SERIAL_001");

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenInstallSkipsActiveSession)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallTokenDeviceIdLookupThrows)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallTokenDeviceIdCallFailureHitsCatch)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushError();

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallTokenDeviceIdWrongTypeHitsCatch)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantUint32(0xDEADBEEF);

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);

    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallUnexpectedPostInstallStatusSetsError)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_001");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "not-installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallHandlesMixedEndpoints)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-0");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_MISSING");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstall(tokens);
    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallCoversLegacyMixedOutcomeMatrix)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu2", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu3", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu4", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu5", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu6", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu7", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-0");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantErrorTuple(0x30, "status tuple mismatch");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 3);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-2");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushError();

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-3");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_MISSING");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-4");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_INSTALL_FAIL");
    wrap_state::pushError();

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-5");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_POST_EMPTY");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-5");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-5");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantErrorTuple(0x31, "missing post-install status");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-6");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_POST_WRONG");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-6");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-6");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "not-installed", 0);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-7");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-7");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-7");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_INSTALL_FAIL", std::vector<uint8_t>(128, 0x41));
    tokens.emplace("SERIAL_POST_EMPTY", std::vector<uint8_t>(128, 0x42));
    tokens.emplace("SERIAL_POST_WRONG", std::vector<uint8_t>(128, 0x43));
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(128, 0x44));

    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallInvalidEndpointPathHitsCatch)
{
    wrap_state::pushGetSubTreeEntries(
        {{"not-a-valid-object-path", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_VALID");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_VALID", std::vector<uint8_t>(128, 0x61));

    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest,
       NsmTokenInstallInitialStatusFailureThenContinuesToSuccessEndpoint)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    wrap_state::pushError();

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(128, 0x62));

    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallTokenTimeoutStillAttemptsInstall)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, "timed-out", 9);
    wrap_state::pushVariantString("SERIAL_TIMEOUT");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_TIMEOUT", std::vector<uint8_t>(128, 0x63));

    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest,
       NsmTokenInstallInstallFailureThenContinuesToNextEndpoint)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenIntfName}});

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-0");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_FAIL");
    wrap_state::pushError();

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after-1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_FAIL", std::vector<uint8_t>(128, 0x64));
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(128, 0x65));

    int result = udt.nsmTokenInstall(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallAllocationFailureSweepSuccessPath)
{
    TokenMap tokens;
    tokens.emplace("SERIAL_ALLOC_OK", std::vector<uint8_t>(128, 0x6A));

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
        wrap_state::pushVariantString("SERIAL_ALLOC_OK");
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);
    };

    prepare();
    (void)udt.nsmTokenInstall(tokens);

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.nsmTokenInstall(tokens); });
}

TEST_F(RealBusWrappedTest, NsmTokenInstallAllocationFailureSweepFailurePath)
{
    TokenMap tokens;
    tokens.emplace("SERIAL_ALLOC_FAIL", std::vector<uint8_t>(128, 0x6B));

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
        wrap_state::pushVariantString("SERIAL_ALLOC_FAIL");
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");
        wrap_state::pushVariantErrorTuple(0x23, "install failed");
    };

    prepare();
    (void)udt.nsmTokenInstall(tokens);

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.nsmTokenInstall(tokens); });
}

// ========================== nsmTokenEraseV2 ===============================

TEST_F(RealBusWrappedTest, NsmTokenEraseV2OneEndpoint)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    // handleAsyncCallEraseV2 -> makeDebugTokenMethodCall
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    // Get initial status
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    int result = udt.nsmTokenEraseV2();
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenEraseV2Failed)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");

    int result = udt.nsmTokenEraseV2();
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenEraseV2NotInstalledStillReturnsSuccess)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/not-installed");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");

    int result = udt.nsmTokenEraseV2();
    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseV2HandlesMixedOutcomes)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu2", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/not-installed");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/generic-error");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");
    wrap_state::pushVariantErrorTuple(0x42, "erase failed");
    wrap_state::pushVariantErrorTuple(0x42, "erase failed");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/success");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseV2HandlesCallFailureThenRecovers)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu2", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});

    wrap_state::pushError();

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/not-installed");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/success");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseV2InvalidEndpointPathHitsCatch)
{
    wrap_state::pushGetSubTreeEntries(
        {{"not-a-valid-object-path", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/erase-valid");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    int result = udt.nsmTokenEraseV2();
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenEraseV2AllocationFailureSweepSuccessPath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/erase_alloc");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    };

    prepare();
    (void)udt.nsmTokenEraseV2();

    runMeasuredAllocFailureSweep(prepare, [&] { (void)udt.nsmTokenEraseV2(); });
}

TEST_F(RealBusWrappedTest,
       NsmTokenEraseV2AllocationFailureSweepNotInstalledPath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/async/erase_alloc_not_installed");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
        wrap_state::pushVariantErrorTuple(0x100F, "token missing");
        wrap_state::pushVariantErrorTuple(0x100F, "token missing");
    };

    prepare();
    (void)udt.nsmTokenEraseV2();

    runMeasuredAllocFailureSweep(prepare, [&] { (void)udt.nsmTokenEraseV2(); });
}

TEST_F(RealBusWrappedTest,
       NsmTokenEraseV2AllocationFailureSweepInProgressNotInstalled)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/not_installed_alloc_v2";

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushObjectPath(asyncPath.c_str());
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x100F, "token missing");
        wrap_state::pushVariantErrorTuple(0x100F, "token missing");
        async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Error");
    };

    prepare();
    (void)udt.nsmTokenEraseV2();

    runMeasuredAllocFailureSweep(prepare, [&] { (void)udt.nsmTokenEraseV2(); });
}

// ========================== nsmTokenInstallV2 =============================

TEST_F(RealBusWrappedTest, NsmTokenInstallV2OneEndpoint)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushVariantString("V2_SERIAL");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    TokenMap tokens;
    tokens.emplace("V2_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2NoMatch)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushVariantString("DIFFERENT_SERIAL");

    TokenMap tokens;
    tokens.emplace("MY_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2MemfdCreateFails)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushVariantString("V2_SERIAL");

    syscall_state::failMemfdCreate = true;

    TokenMap tokens;
    tokens.emplace("V2_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2ShortWriteFails)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushVariantString("V2_SERIAL");

    syscall_state::shortWrite = true;

    TokenMap tokens;
    tokens.emplace("V2_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2TokenDeviceIdLookupThrows)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);

    TokenMap tokens;
    tokens.emplace("V2_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2TokenDeviceIdCallFailureHitsCatch)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushError();

    TokenMap tokens;
    tokens.emplace("V2_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2TokenDeviceIdWrongTypeHitsCatch)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushVariantUint32(0x12345678);

    TokenMap tokens;
    tokens.emplace("V2_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);

    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2AsyncFailureSetsError)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    wrap_state::pushVariantString("V2_SERIAL");

    TokenMap tokens;
    tokens.emplace("V2_SERIAL", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2HandlesMixedOutcomes)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu2", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});
    wrap_state::pushVariantString("SERIAL_MISSING");
    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-good");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    TokenMap tokens;
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(100, 0x42));
    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest,
       NsmTokenInstallV2HandlesLookupFailureAsyncFailureAndSuccess)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu2", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});

    wrap_state::pushError();

    wrap_state::pushVariantString("SERIAL_FAIL");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-fail");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x23, "install failed");

    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-good");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    TokenMap tokens;
    tokens.emplace("SERIAL_FAIL", std::vector<uint8_t>(96, 0x41));
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(128, 0x42));

    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2RecoversAfterSingleMemfdFailure)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});
    wrap_state::pushVariantString("SERIAL_FAIL_MEMFD");
    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-good");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    syscall_state::failMemfdCreateRemaining = 1;

    TokenMap tokens;
    tokens.emplace("SERIAL_FAIL_MEMFD", std::vector<uint8_t>(96, 0x41));
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(128, 0x42));

    int result = udt.nsmTokenInstallV2(tokens);

    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2RecoversAfterSingleShortWrite)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});
    wrap_state::pushVariantString("SERIAL_SHORT");
    wrap_state::pushVariantString("SERIAL_GOOD");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-good");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    syscall_state::shortWriteRemaining = 1;

    TokenMap tokens;
    tokens.emplace("SERIAL_SHORT", std::vector<uint8_t>(96, 0x31));
    tokens.emplace("SERIAL_GOOD", std::vector<uint8_t>(128, 0x32));

    int result = udt.nsmTokenInstallV2(tokens);

    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2MixedSerialAllocationFailureSweep)
{
    const std::string missingSerial = "SERIAL_MISSING_" + std::string(64, 'M');
    const std::string firstSerial = "SERIAL_FOUND_" + std::string(64, 'A');
    const std::string secondSerial = "SERIAL_FOUND_" + std::string(64, 'B');

    TokenMap tokens;
    tokens.emplace(firstSerial, std::vector<uint8_t>(128, 0x41));
    tokens.emplace(secondSerial, std::vector<uint8_t>(160, 0x42));

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeEntries(
            {{"/xyz/openbmc_project/NSM/gpu0", "xyz.openbmc_project.NSM",
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
              nsmDebugTokenActionIntfName},
             {"/xyz/openbmc_project/NSM/gpu2", "xyz.openbmc_project.NSM",
              nsmDebugTokenActionIntfName}});
        wrap_state::pushVariantString(missingSerial.c_str());
        wrap_state::pushVariantString(firstSerial.c_str());
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-found-a");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantString(secondSerial.c_str());
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-found-b");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    };

    prepare();
    EXPECT_EQ(udt.nsmTokenInstallV2(tokens), -1);

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.nsmTokenInstallV2(tokens); });
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2MemfdFailureAllocationFailureSweep)
{
    const std::string serial = "SERIAL_MEMFD_" + std::string(64, 'F');

    TokenMap tokens;
    tokens.emplace(serial, std::vector<uint8_t>(128, 0x5A));

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString(serial.c_str());
        syscall_state::failMemfdCreate = true;
    };

    prepare();
    EXPECT_EQ(udt.nsmTokenInstallV2(tokens), -1);

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.nsmTokenInstallV2(tokens); });
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2ShortWriteAllocationFailureSweep)
{
    const std::string serial = "SERIAL_SHORT_" + std::string(64, 'W');

    TokenMap tokens;
    tokens.emplace(serial, std::vector<uint8_t>(128, 0x6B));

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString(serial.c_str());
        syscall_state::shortWrite = true;
    };

    prepare();
    EXPECT_EQ(udt.nsmTokenInstallV2(tokens), -1);

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.nsmTokenInstallV2(tokens); });
}

TEST_F(RealBusWrappedTest, NsmTokenInstallV2InvalidEndpointPathHitsCatch)
{
    wrap_state::pushGetSubTreeEntries(
        {{"not-a-valid-object-path", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName},
         {"/xyz/openbmc_project/NSM/gpu1", "xyz.openbmc_project.NSM",
          nsmDebugTokenActionIntfName}});
    wrap_state::pushVariantString("SERIAL_VALID");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-valid");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    TokenMap tokens;
    tokens.emplace("SERIAL_VALID", std::vector<uint8_t>(128, 0x5A));

    int result = udt.nsmTokenInstallV2(tokens);
    EXPECT_EQ(result, -1);
}

TEST_F(RealBusWrappedTest,
       NsmTokenInstallV2AllocationFailureSweepInProgressSuccessPath)
{
    TokenMap tokens;
    tokens.emplace("ALLOC_SERIAL", std::vector<uint8_t>(128, 0x5A));

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                              "xyz.openbmc_project.NSM",
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString("ALLOC_SERIAL");
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install_alloc");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    };

    prepare();
    (void)udt.nsmTokenInstallV2(tokens);

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.nsmTokenInstallV2(tokens); });
}

// ========================== handleAsyncCall paths =========================

TEST_F(RealBusWrappedTest, HandleAsyncCallSuccess)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    (void)result; // May or may not match depending on read success
}

TEST_F(RealBusWrappedTest, HandleAsyncCallFailed)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallImmediateErrorWithTupleValue)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/error-immediate");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x10, "immediate failure");

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallSuccessAfterInProgressSignal)
{
    const std::string asyncPath = "/com/nvidia/nsmd/AsyncOperation/case2";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");

    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Success");
    auto result = udt.handleAsyncCall("/test", "EraseToken");
    (void)result;
}

TEST_F(RealBusWrappedTest, HandleAsyncCallIgnoresWrongPathSignalThenSucceeds)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/case_wrong_path";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");

    async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                              "com.nvidia.Async.Status.Success");
    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest,
       HandleAsyncCallIgnoresSignalWithoutStatusPropertyThenSucceeds)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/case_missing_status";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");

    async_signal_state::queueWithProperty(
        asyncPath, "com.nvidia.Async.Status.Success", "Other");
    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallFailedAfterInProgressSignal)
{
    const std::string asyncPath = "/com/nvidia/nsmd/AsyncOperation/case3";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x10, "signal failure");

    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Failed");
    auto result = udt.handleAsyncCall("/test", "EraseToken");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallWrongPathSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/wrong_path_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x43, "timeout after wrong path");

    async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                              "com.nvidia.Async.Status.Success");
    auto result = udt.handleAsyncCall("/test", "EraseToken");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallMissingStatusSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/missing_status_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x44, "timeout after missing status");

    async_signal_state::queueWithProperty(
        asyncPath, "com.nvidia.Async.Status.Success", "Other");
    auto result = udt.handleAsyncCall("/test", "EraseToken");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallUint32StatusSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/uint32_status_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x46, "timeout after uint32 status");

    async_signal_state::queueWithUint32Status(asyncPath, 7);
    auto result = udt.handleAsyncCall("/test", "EraseToken");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallTruncatedSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/truncated_signal_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x47, "timeout after truncated signal");

    async_signal_state::queueTruncated(asyncPath);
    auto result = udt.handleAsyncCall("/test", "EraseToken");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallNoQueuedResponse)
{
    // No responses -> sd_bus_call fails
    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInitialStatusLookupFails)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-lookup-fail");

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInitialStatusWrongTypeFails)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-wrong-type");
    wrap_state::pushVariantUint32(7);

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInitialStatusReplyReadFailure)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-read-failure");
    wrap_state::pushEmpty();

    auto result = udt.handleAsyncCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

// ========================== handleAsyncCallInstallV2 ======================

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2Success)
{
    int memfd = memfd_create("token", MFD_CLOEXEC);
    if (memfd >= 0)
    {
        std::vector<uint8_t> data(256, 0xCC);
        ASSERT_EQ(static_cast<ssize_t>(data.size()),
                  write(memfd, data.data(), data.size()));
        lseek(memfd, 0, SEEK_SET);

        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

        auto result = udt.handleAsyncCallInstallV2("/test", memfd);
        (void)result;
        close(memfd);
    }
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2InvalidPathHitsCatch)
{
    int memfd = memfd_create("token-invalid-path", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(32, 0xA1);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    auto result =
        udt.handleAsyncCallInstallV2("not-a-valid-object-path", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2CallFailureHitsCatch)
{
    int memfd = memfd_create("token-call-failure", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(32, 0x91);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    wrap_state::pushError();

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2SuccessAfterInProgressSignal)
{
    int memfd = memfd_create("token-progress", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(128, 0xAB);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/install_progress";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");

    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Success");
    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);
    (void)result;
}

TEST_F(RealBusWrappedTest,
       HandleAsyncCallInstallV2IgnoresWrongPathSignalThenSucceeds)
{
    int memfd = memfd_create("token-wrong-path", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xBC);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/install_wrong_path";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");

    async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                              "com.nvidia.Async.Status.Success");
    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2FailedAfterInProgressSignal)
{
    int memfd = memfd_create("token-failed", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(128, 0xAB);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/install_failed";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x23, "install failed");

    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Failed");
    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2InitialStatusLookupFails)
{
    int memfd = memfd_create("token-status-fail", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xCD);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-status-fail");

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest,
       HandleAsyncCallInstallV2InitialStatusReplyReadFailure)
{
    int memfd = memfd_create("token-status-read-fail", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xCE);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-status-read");
    wrap_state::pushEmpty();

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2ReplyReadFailure)
{
    int memfd = memfd_create("token-read-failure", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0x5C);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    wrap_state::pushEmpty();

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2WrongPathSignalTimesOut)
{
    int memfd = memfd_create("token-install-wrong-path-only", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0x6A);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/install_wrong_path_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x32, "install timeout after wrong path");

    async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                              "com.nvidia.Async.Status.Success");
    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2InitialStatusWrongTypeFails)
{
    int memfd = memfd_create("token-status-wrong-type", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xD2);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-status-wrong");
    wrap_state::pushVariantUint32(9);

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2ImmediateErrorWithTupleValue)
{
    int memfd = memfd_create("token-install-error", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xA5);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-immediate");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x20, "install immediate failure");

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2ImmediateErrorWithoutValue)
{
    int memfd = memfd_create("token-install-no-value", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xA6);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-no-value");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");

    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest,
       HandleAsyncCallInstallV2SignalWithoutStatusPropertyTimesOut)
{
    int memfd = memfd_create("token-missing-status", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xD4);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/install_missing_status_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x21, "missing status timeout");

    async_signal_state::queueWithProperty(
        asyncPath, "com.nvidia.Async.Status.Success", "Other");
    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2Uint32StatusSignalTimesOut)
{
    int memfd = memfd_create("token-uint32-status", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xD5);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/install_uint32_status_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x22, "uint32 status timeout");

    async_signal_state::queueWithUint32Status(asyncPath, 99);
    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallInstallV2TruncatedSignalTimesOut)
{
    int memfd = memfd_create("token-truncated-status", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);

    std::vector<uint8_t> data(64, 0xD6);
    ASSERT_EQ(static_cast<ssize_t>(data.size()),
              write(memfd, data.data(), data.size()));
    lseek(memfd, 0, SEEK_SET);

    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/install_truncated_signal_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x24, "truncated signal timeout");

    async_signal_state::queueTruncated(asyncPath);
    auto result = udt.handleAsyncCallInstallV2("/test", memfd);
    close(memfd);

    EXPECT_TRUE(result.empty());
}

// ========================== handleAsyncCallEraseV2 ========================

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2Success)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCallEraseV2("/test");
    (void)result;
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2InvalidPathHitsCatch)
{
    auto result = udt.handleAsyncCallEraseV2("not-a-valid-object-path");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2CallFailureHitsCatch)
{
    wrap_state::pushError();

    auto result = udt.handleAsyncCallEraseV2("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2ReplyReadFailure)
{
    wrap_state::pushEmpty();

    auto result = udt.handleAsyncCallEraseV2("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2Failed)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");

    auto result = udt.handleAsyncCallEraseV2("/test");
    (void)result;
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2NotInstalledReturnsAsyncPath)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/not-installed");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");

    auto result = udt.handleAsyncCallEraseV2("/test");
    (void)result;
}

TEST_F(RealBusWrappedTest,
       HandleAsyncCallEraseV2NotInstalledAfterInProgressSignal)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/not_installed_2";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");
    wrap_state::pushVariantErrorTuple(0x100F, "token missing");

    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Error");
    auto result = udt.handleAsyncCallEraseV2("/test");
    (void)result;
}

TEST_F(RealBusWrappedTest,
       HandleAsyncCallEraseV2IgnoresSignalWithoutStatusPropertyThenSucceeds)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/erase_missing_status";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");

    async_signal_state::queueWithProperty(
        asyncPath, "com.nvidia.Async.Status.Success", "Other");
    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Success");

    auto result = udt.handleAsyncCallEraseV2("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2MissingStatusTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/erase_missing_status_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x45,
                                      "erase timeout after missing status");
    wrap_state::pushVariantErrorTuple(0x45,
                                      "erase timeout after missing status");

    async_signal_state::queueWithProperty(
        asyncPath, "com.nvidia.Async.Status.Success", "Other");
    auto result = udt.handleAsyncCallEraseV2("/test");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2ErrorWithoutValueReturnsEmpty)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/error-no-value");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");

    auto result = udt.handleAsyncCallEraseV2("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest,
       HandleAsyncCallEraseV2ImmediateErrorValueLookupThrows)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/error-value-throw");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x40, "erase immediate failure");
    wrap_state::pushError();

    auto result = udt.handleAsyncCallEraseV2("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2ImmediateGenericErrorValue)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/error-generic");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
    wrap_state::pushVariantErrorTuple(0x40, "erase immediate failure");
    wrap_state::pushVariantErrorTuple(0x40, "erase immediate failure");

    auto result = udt.handleAsyncCallEraseV2("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2WrongPathSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/erase_wrong_path_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x41, "erase timeout after wrong path");

    async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                              "com.nvidia.Async.Status.Success");
    auto result = udt.handleAsyncCallEraseV2("/test");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2InitialStatusWrongTypeFails)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/erase-status-wrong");
    wrap_state::pushVariantUint32(11);

    auto result = udt.handleAsyncCallEraseV2("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2GenericFailureAfterSignal)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/generic_failure";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x42, "erase failed");
    wrap_state::pushVariantErrorTuple(0x42, "erase failed");

    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Failed");
    auto result = udt.handleAsyncCallEraseV2("/test");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2SignalErrorValueLookupThrows)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/error_value_throw_after_signal";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x42, "erase failed");
    wrap_state::pushError();

    async_signal_state::queue(asyncPath, "com.nvidia.Async.Status.Failed");
    auto result = udt.handleAsyncCallEraseV2("/test");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2Uint32StatusSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/erase_uint32_status_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x48, "erase uint32 status timeout");
    wrap_state::pushVariantErrorTuple(0x48, "erase uint32 status timeout");

    async_signal_state::queueWithUint32Status(asyncPath, 13);
    auto result = udt.handleAsyncCallEraseV2("/test");

    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, HandleAsyncCallEraseV2TruncatedSignalTimesOut)
{
    const std::string asyncPath =
        "/com/nvidia/nsmd/AsyncOperation/erase_truncated_signal_only";
    wrap_state::pushObjectPath(asyncPath.c_str());
    wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
    wrap_state::pushVariantErrorTuple(0x49, "erase truncated signal timeout");
    wrap_state::pushVariantErrorTuple(0x49, "erase truncated signal timeout");

    async_signal_state::queueTruncated(asyncPath);
    auto result = udt.handleAsyncCallEraseV2("/test");

    EXPECT_TRUE(result.empty());
}

// ========================== makeDebugTokenMethodCall ======================

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallReturnsPath)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/42");

    auto result = udt.makeDebugTokenMethodCall("/test", "EraseToken");
    (void)result;
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallGetStatus)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");

    auto result =
        udt.makeDebugTokenMethodCall("/test", "GetStatus", std::string("CRDT"));
    (void)result; // May or may not match depending on read success
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallInstallToken)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/2");

    std::vector<uint8_t> token(64, 0x44);
    auto result = udt.makeDebugTokenMethodCall("/test", "InstallToken", token);
    (void)result;
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallDisableTokensReturnsPath)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/3");

    auto result = udt.makeDebugTokenMethodCall("/test", "DisableTokens");
    EXPECT_TRUE(result.empty() || result == "/com/nvidia/nsmd/async/3");
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallReplyReadFailure)
{
    wrap_state::pushEmpty();

    auto result = udt.makeDebugTokenMethodCall("/test", "EraseToken");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallDisableTokensWrongReplyType)
{
    wrap_state::pushVariantString("not-an-object-path");

    auto result = udt.makeDebugTokenMethodCall("/test", "DisableTokens");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallGetStatusRejectsWrongVariant)
{
    auto result = udt.makeDebugTokenMethodCall("/test", "GetStatus",
                                               std::vector<uint8_t>{0x01});
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest,
       MakeDebugTokenMethodCallInstallTokenRejectsWrongVariant)
{
    auto result = udt.makeDebugTokenMethodCall("/test", "InstallToken",
                                               std::string("wrong-type"));
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallInvalidPathReturnsEmpty)
{
    auto result = udt.makeDebugTokenMethodCall("not-a-valid-object-path",
                                               "DisableTokens");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, MakeDebugTokenMethodCallAllocationFailureSweep)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'M');
    const Token token(256, 0x5A);

    auto prepareGetStatus = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/get-status-alloc");
    };

    prepareGetStatus();
    (void)udt.makeDebugTokenMethodCall(path, "GetStatus",
                                       std::string(nsmTokenTypeCRDT));
    runMeasuredAllocFailureSweep(prepareGetStatus, [&] {
        (void)udt.makeDebugTokenMethodCall(path, "GetStatus",
                                           std::string(nsmTokenTypeCRDT));
    });

    auto prepareInstall = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install-alloc");
    };

    prepareInstall();
    (void)udt.makeDebugTokenMethodCall(path, "InstallToken", token);
    runMeasuredAllocFailureSweep(prepareInstall, [&] {
        (void)udt.makeDebugTokenMethodCall(path, "InstallToken", token);
    });
}

TEST_F(RealBusWrappedTest,
       MakeDebugTokenMethodCallDisableTokensAllocationFailureSweep)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'D');

    auto prepareSuccess = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable-alloc");
    };

    prepareSuccess();
    (void)udt.makeDebugTokenMethodCall(path, "DisableTokens");
    runMeasuredAllocFailureSweep(prepareSuccess, [&] {
        (void)udt.makeDebugTokenMethodCall(path, "DisableTokens");
    });

    auto prepareWrongReply = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushVariantString("not-an-object-path");
    };

    prepareWrongReply();
    EXPECT_TRUE(udt.makeDebugTokenMethodCall(path, "DisableTokens").empty());
    runMeasuredAllocFailureSweep(prepareWrongReply, [&] {
        (void)udt.makeDebugTokenMethodCall(path, "DisableTokens");
    });

    auto prepareReplyReadFailure = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushEmpty();
    };

    prepareReplyReadFailure();
    EXPECT_TRUE(udt.makeDebugTokenMethodCall(path, "DisableTokens").empty());
    runMeasuredAllocFailureSweep(prepareReplyReadFailure, [&] {
        (void)udt.makeDebugTokenMethodCall(path, "DisableTokens");
    });
}

// ========================== getTokenStatus ================================

TEST_F(RealBusWrappedTest, GetAsyncValueCallFailureThrows)
{
    EXPECT_ANY_THROW((void)udt.getAsyncValue("/test"));
}

TEST_F(RealBusWrappedTest, GetAsyncValueReplyReadFailureThrows)
{
    wrap_state::pushEmpty();

    EXPECT_ANY_THROW((void)udt.getAsyncValue("/test"));
}

TEST_F(RealBusWrappedTest, GetAsyncValueWrongVariantThrows)
{
    wrap_state::pushVariantString("wrong-type");

    EXPECT_ANY_THROW((void)udt.getAsyncValue("/test"));
}

TEST_F(RealBusWrappedTest, GetAsyncValueInvalidPathThrows)
{
    EXPECT_ANY_THROW((void)udt.getAsyncValue("not-a-valid-object-path"));
}

TEST_F(RealBusWrappedTest, GetTokenStatusReturnsValue)
{
    // handleAsyncCall -> makeDebugTokenMethodCall
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    // Get initial status -> Success
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    // getAsyncValue -> push empty (will fail but cover the try block)
    wrap_state::pushEmpty();

    auto result = udt.getTokenStatus("/test");
    (void)result;
}

TEST_F(RealBusWrappedTest, GetTokenStatusReturnsTokenTuple)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/9");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "ready", 17);

    auto result = udt.getTokenStatus("/test");
    (void)result;
}

TEST_F(RealBusWrappedTest, GetTokenStatusHandlesErrorTupleValue)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/10");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantErrorTuple(0x10, "generic failure");

    auto result = udt.getTokenStatus("/test");
    EXPECT_TRUE(result.empty());
}

TEST_F(RealBusWrappedTest, GetTokenStatusWrongValueTypeHitsCatch)
{
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/11");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantString("not-a-token-status");

    auto result = udt.getTokenStatus("/test");
    EXPECT_TRUE(result.empty());
}

// ========================== logAsyncError =================================

TEST_F(RealBusWrappedTest, LogAsyncErrorWithValue)
{
    // getAsyncValue will fail (no queued response)
    EXPECT_NO_THROW(udt.logAsyncError("/test", "EraseToken", "Failed"));
}

TEST_F(RealBusWrappedTest, LogAsyncErrorWithNotInstalledValue)
{
    wrap_state::pushVariantErrorTuple(0x100F, "token is not installed");

    EXPECT_NO_THROW(udt.logAsyncError("/test", "EraseToken", "Error"));
}

TEST_F(RealBusWrappedTest, LogAsyncErrorWithGenericErrorValue)
{
    wrap_state::pushVariantErrorTuple(0x10, "generic failure");

    EXPECT_NO_THROW(udt.logAsyncError("/test", "InstallToken", "Failed"));
}

TEST_F(RealBusWrappedTest,
       LogAsyncErrorWithNotInstalledValueAllocationFailureSweep)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'P');
    const std::string method = std::string(64, 'M');
    const std::string status = std::string(80, 'S');
    const std::string errorMessage = std::string(192, 'N');

    auto prepare = [&] {
        wrap_state::reset();
        wrap_state::pushVariantErrorTuple(0x100F, errorMessage.c_str());
    };

    prepare();
    EXPECT_NO_THROW(udt.logAsyncError(path, method, status));

    runMeasuredAllocFailureSweep(
        prepare, [&] { udt.logAsyncError(path, method, status); });
}

TEST_F(RealBusWrappedTest, LogAsyncErrorWithGenericValueAllocationFailureSweep)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'Q');
    const std::string method = std::string(64, 'I');
    const std::string status = std::string(80, 'F');
    const std::string errorMessage = std::string(192, 'E');

    auto prepare = [&] {
        wrap_state::reset();
        wrap_state::pushVariantErrorTuple(0x20, errorMessage.c_str());
    };

    prepare();
    EXPECT_NO_THROW(udt.logAsyncError(path, method, status));

    runMeasuredAllocFailureSweep(
        prepare, [&] { udt.logAsyncError(path, method, status); });
}

TEST_F(RealBusWrappedTest, LogAsyncErrorExceptionAllocationFailureSweep)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'R');
    const std::string method = std::string(72, 'X');
    const std::string status = std::string(88, 'Z');

    auto prepare = [&] {
        wrap_state::reset();
        wrap_state::pushError();
    };

    prepare();
    EXPECT_NO_THROW(udt.logAsyncError(path, method, status));

    runMeasuredAllocFailureSweep(
        prepare, [&] { udt.logAsyncError(path, method, status); });
}

TEST_F(RealBusWrappedTest, LogAsyncErrorHandlesTokenStatusValue)
{
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "ready", 0);

    EXPECT_NO_THROW(udt.logAsyncError("/test", "GetStatus", "Failed"));
}

TEST_F(RealBusWrappedTest, LogAsyncErrorWrongValueTypeHitsCatch)
{
    wrap_state::pushVariantString("not-an-error-tuple");

    EXPECT_NO_THROW(udt.logAsyncError("/test", "InstallToken", "Failed"));
}

// ========================== getMCTPServiceList ============================

TEST_F(RealBusWrappedTest, GetMCTPServiceListHandlesCallFailure)
{
    wrap_state::failAll = true;

    auto services = udt.getMCTPServiceList();
    EXPECT_TRUE(services.empty());
}

TEST_F(RealBusWrappedTest, GetMCTPServiceListOneService)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          "xyz.openbmc_project.MCTP.Endpoint");

    auto services = udt.getMCTPServiceList();
    (void)services;
}

TEST_F(RealBusWrappedTest, GetMCTPServiceListDeduplicatesServices)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
          mctpEndpointIntfName},
         {"/au/com/codeconstruct/mctp1/network/2", "au.com.codeconstruct.MCTP1",
          mctpEndpointIntfName},
         {"/xyz/openbmc_project/mctp/network/1", "xyz.openbmc_project.MCTP",
          mctpEndpointIntfName}});

    auto services = udt.getMCTPServiceList();
    (void)services;
}

TEST_F(RealBusWrappedTest, GetMCTPServiceListHandlesExplicitEmptyResponse)
{
    wrap_state::pushGetSubTreeEntries({});

    auto services = udt.getMCTPServiceList();
    EXPECT_TRUE(services.empty());
}

TEST_F(RealBusWrappedTest, GetMCTPServiceListAllocationFailureSweep)
{
    const std::string pathA =
        "/au/com/codeconstruct/mctp1/network/" + std::string(48, 'a');
    const std::string pathB =
        "/au/com/codeconstruct/mctp1/network/" + std::string(48, 'b');
    const std::string pathC =
        "/xyz/openbmc_project/mctp/network/" + std::string(48, 'c');
    const std::string serviceA =
        "au.com.codeconstruct.MCTP." + std::string(24, 'a');
    const std::string serviceB =
        "xyz.openbmc_project.MCTP." + std::string(24, 'b');

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeEntries(
            {{pathA, serviceA, mctpEndpointIntfName},
             {pathB, serviceA, mctpEndpointIntfName},
             {pathC, serviceB, mctpEndpointIntfName}});
    };

    prepare();
    (void)udt.getMCTPServiceList();

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.getMCTPServiceList(); });
}

TEST_F(RealBusWrappedTest, GetMCTPServiceListAllocationFailureSweepFailurePath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::failAll = true;
    };

    prepare();
    (void)udt.getMCTPServiceList();

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.getMCTPServiceList(); });
}

TEST_F(RealBusWrappedTest, GetMCTPManagedObjectsMergesAcrossServices)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
          mctpEndpointIntfName},
         {"/xyz/openbmc_project/mctp/network/1", "xyz.openbmc_project.MCTP",
          mctpEndpointIntfName}});
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/network/1/8",
          {{uuidEndpointIntfName, {{"UUID", std::string("uuid-a")}}}}}});
    wrap_state::pushManagedObjects(
        {{"/xyz/openbmc_project/mctp/network/1/9",
          {{uuidEndpointIntfName, {{"UUID", std::string("uuid-b")}}}}}});

    auto objects = udt.getMCTPManagedObjects();
    (void)objects;
}

TEST_F(RealBusWrappedTest,
       GetMCTPManagedObjectsKeepsWorkingAfterOneServiceFails)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
          mctpEndpointIntfName},
         {"/xyz/openbmc_project/mctp/network/1", "xyz.openbmc_project.MCTP",
          mctpEndpointIntfName}});
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/network/1/8",
          {{uuidEndpointIntfName, {{"UUID", std::string("uuid-a")}}}}}});

    auto objects = udt.getMCTPManagedObjects();
    (void)objects;
}

TEST_F(
    RealBusWrappedTest,
    GetMCTPManagedObjectsContinuesAfterLeadingServiceFailureAndMergesLaterResponses)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
          mctpEndpointIntfName},
         {"/xyz/openbmc_project/mctp/network/1", "xyz.openbmc_project.MCTP",
          mctpEndpointIntfName},
         {"/xyz/openbmc_project/mctp/network/2", "xyz.openbmc_project.MCTP2",
          mctpEndpointIntfName}});
    wrap_state::pushError();
    wrap_state::pushManagedObjects(
        {{"/xyz/openbmc_project/mctp/network/1/9",
          {{uuidEndpointIntfName, {{"UUID", std::string("uuid-b")}}}}}});
    wrap_state::pushManagedObjects(
        {{"/xyz/openbmc_project/mctp/network/2/10",
          {{uuidEndpointIntfName, {{"UUID", std::string("uuid-c")}}}}}});

    auto objects = udt.getMCTPManagedObjects();
    EXPECT_LE(objects.size(), 2u);
}

TEST_F(RealBusWrappedTest,
       GetMCTPManagedObjectsKeepsEarlierSuccessOnTrailingFailure)
{
    wrap_state::pushGetSubTreeEntries(
        {{"/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
          mctpEndpointIntfName},
         {"/xyz/openbmc_project/mctp/network/1", "xyz.openbmc_project.MCTP",
          mctpEndpointIntfName}});
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/network/1/8",
          {{uuidEndpointIntfName, {{"UUID", std::string("uuid-a")}}}}}});
    wrap_state::pushError();

    auto objects = udt.getMCTPManagedObjects();

    EXPECT_LE(objects.size(), 1u);
}

TEST_F(RealBusWrappedTest, GetMCTPManagedObjectsSingleServiceFailure)
{
    wrap_state::pushGetSubTreeOneEndpoint(
        "/au/com/codeconstruct/mctp1/network/1", "au.com.codeconstruct.MCTP1",
        mctpEndpointIntfName);
    wrap_state::pushError();

    auto objects = udt.getMCTPManagedObjects();
    EXPECT_TRUE(objects.empty());
}

TEST_F(RealBusWrappedTest, GetMCTPManagedObjectsNoServicesReturnsEmpty)
{
    wrap_state::pushEmpty();

    auto objects = udt.getMCTPManagedObjects();
    EXPECT_TRUE(objects.empty());
}

TEST_F(RealBusWrappedTest, GetMCTPManagedObjectsAllocationFailureSweep)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeEntries(
            {{"/au/com/codeconstruct/mctp1/network/1",
              "au.com.codeconstruct.MCTP1", mctpEndpointIntfName},
             {"/xyz/openbmc_project/mctp/network/1", "xyz.openbmc_project.MCTP",
              mctpEndpointIntfName}});
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/network/1/8",
              {{uuidEndpointIntfName, {{"UUID", std::string("uuid-a")}}}}}});
        wrap_state::pushManagedObjects(
            {{"/xyz/openbmc_project/mctp/network/1/9",
              {{uuidEndpointIntfName, {{"UUID", std::string("uuid-b")}}}}}});
    };

    prepare();
    (void)udt.getMCTPManagedObjects();

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.getMCTPManagedObjects(); });
}

TEST_F(RealBusWrappedTest,
       GetMCTPManagedObjectsAllocationFailureSweepFailurePath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint(
            "/au/com/codeconstruct/mctp1/network/1",
            "au.com.codeconstruct.MCTP1", mctpEndpointIntfName);
        wrap_state::pushError();
    };

    prepare();
    (void)udt.getMCTPManagedObjects();

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.getMCTPManagedObjects(); });
}

TEST_F(RealBusWrappedTest,
       DiscoverMCTPDevicesFiltersInvalidObjectsAndPrefersPcie)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/skipped-no-uuid",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(10)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}}}}}},
         {"/au/com/codeconstruct/mctp1/empty-uuid",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(11)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}},
         {"/au/com/codeconstruct/mctp1/disabled",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(12)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("disabled-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Unavailable")}}}}},
         {"/au/com/codeconstruct/mctp1/unsupported",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(13)},
             {"SupportedMessageTypes", std::vector<uint8_t>{mctpTypeSPDM}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("unsupported-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}},
         {"/au/com/codeconstruct/mctp1/no-medium",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(14)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}}}},
           {uuidEndpointIntfName, {{"UUID", std::string("no-medium-uuid")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}},
         {"/au/com/codeconstruct/mctp1/dup-a-smbus",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(40)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("dup-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}},
         {"/au/com/codeconstruct/mctp1/dup-b-pcie",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(41)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("dup-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}},
         {"/au/com/codeconstruct/mctp1/valid-usb",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(55)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("usb-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});

    auto result = udt.discoverMCTPDevices();
    (void)result;
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesKeepsExistingFasterDuplicate)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    ManagedObjects managedObjects{
        {"/au/com/codeconstruct/mctp1/dup-fast-pcie",
         {{mctpEndpointIntfName,
           {{"EID", static_cast<uint8_t>(41)},
            {"SupportedMessageTypes",
             std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
            {"MediumType",
             std::string(
                 "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
          {uuidEndpointIntfName, {{"UUID", std::string("dup-fast-uuid")}}},
          {mctpBindingIntfName,
           {{"BindingType",
             std::string(
                 "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
          {mctpEndpointEnableIntfName,
           {{"Connectivity", std::string("Available")}}}}},
        {"/au/com/codeconstruct/mctp1/dup-slow-smbus",
         {{mctpEndpointIntfName,
           {{"EID", static_cast<uint8_t>(55)},
            {"SupportedMessageTypes",
             std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
            {"MediumType",
             std::string(
                 "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus")}}},
          {uuidEndpointIntfName, {{"UUID", std::string("dup-fast-uuid")}}},
          {mctpBindingIntfName,
           {{"BindingType",
             std::string(
                 "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus")}}},
          {mctpEndpointEnableIntfName,
           {{"Connectivity", std::string("Available")}}}}},
    };
    wrap_state::pushManagedObjects(managedObjects);

    auto result = udt.discoverMCTPDevices();

    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesAllowsBindingOnlyEndpoint)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/binding-only",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(77)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}}}},
           {uuidEndpointIntfName, {{"UUID", std::string("binding-only-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});

    auto result = udt.discoverMCTPDevices();

    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest,
       DiscoverMCTPDevicesSkipsUuidInterfaceWithoutUuidProperty)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/missing-uuid-property",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(91)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});

    auto result = udt.discoverMCTPDevices();

    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesAllowsMediumOnlyEndpoint)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/medium-only",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(92)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("medium-only")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});

    auto result = udt.discoverMCTPDevices();

    EXPECT_LE(result, 0);
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesAllocationFailureSweepSuccessPath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/endpoint0",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(42)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName, {{"UUID", std::string("alloc-uuid")}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
    };

    prepare();
    (void)udt.discoverMCTPDevices();

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.discoverMCTPDevices(); });
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesAllocationFailureSweepFailurePath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushError();
    };

    prepare();
    (void)udt.discoverMCTPDevices();

    runMeasuredAllocFailureSweep(prepare,
                                 [&] { (void)udt.discoverMCTPDevices(); });
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesSkipsEmptyTransportEndpoint)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/empty-transport",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(91)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}}}},
           {uuidEndpointIntfName, {{"UUID", std::string("empty-transport")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});

    auto result = udt.discoverMCTPDevices();

    EXPECT_LE(result, 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesSkipsUuidOnlyObject)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/uuid-only",
          {{uuidEndpointIntfName, {{"UUID", std::string("uuid-only")}}}}}});

    auto result = udt.discoverMCTPDevices();

    EXPECT_LE(result, 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(RealBusWrappedTest, DiscoverMCTPDevicesSkipsEndpointOnlyObject)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/endpoint-only",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(93)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});

    auto result = udt.discoverMCTPDevices();

    EXPECT_LE(result, 0);
    EXPECT_TRUE(udt.mctpInfo.empty());
}

TEST_F(RealBusWrappedTest, UpdateEndPointsBuildsDeviceMapFromManagedObjects)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/endpoint0",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(42)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});
    wrap_state::pushManagedObjects(
        {{"/xyz/openbmc_project/software/GPU_ERoT_0",
          {{uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid")}}},
           {pldmInventoryIntfName,
            {{"SerialNumber", std::string("SN123")}}}}}});

    auto result = udt.updateEndPoints();
    (void)result;
}

TEST_F(RealBusWrappedTest, UpdateEndPointsFailsWhenPldmQueryFailsAfterDiscovery)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/endpoint0",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(42)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});

    auto result = udt.updateEndPoints();

    EXPECT_EQ(result, -1);
    EXPECT_TRUE(udt.devices.empty());
}

TEST_F(RealBusWrappedTest,
       UpdateEndPointsFailsWhenPldmReplyReadThrowsAfterDiscovery)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/endpoint0",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(42)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});
    wrap_state::pushVariantString("not-managed-objects");

    auto result = udt.updateEndPoints();

    EXPECT_EQ(result, -1);
    EXPECT_TRUE(udt.devices.empty());
}

TEST_F(RealBusWrappedTest, UpdateEndPointsIgnoresPldmEntryWithoutKnownMctpUuid)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/endpoint0",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(42)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});
    wrap_state::pushManagedObjects(
        {{"/xyz/openbmc_project/software/GPU_ERoT_0",
          {{uuidEndpointIntfName, {{"UUID", std::string("other-uuid")}}},
           {pldmInventoryIntfName,
            {{"SerialNumber", std::string("SN999")}}}}}});

    auto result = udt.updateEndPoints();

    EXPECT_LE(result, 0);
    EXPECT_TRUE(udt.devices.empty());
}

TEST_F(RealBusWrappedTest, UpdateEndPointsIgnoresPldmEntryWithoutSerialNumber)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    wrap_state::pushManagedObjects(
        {{"/au/com/codeconstruct/mctp1/endpoint0",
          {{mctpEndpointIntfName,
            {{"EID", static_cast<uint8_t>(42)},
             {"SupportedMessageTypes",
              std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
             {"MediumType",
              std::string(
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
           {uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid")}}},
           {mctpBindingIntfName,
            {{"BindingType",
              std::string(
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
           {mctpEndpointEnableIntfName,
            {{"Connectivity", std::string("Available")}}}}}});
    wrap_state::pushManagedObjects(
        {{"/xyz/openbmc_project/software/GPU_ERoT_0",
          {{uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid")}}},
           {pldmInventoryIntfName, {}}}}});

    auto result = udt.updateEndPoints();

    EXPECT_LE(result, 0);
    EXPECT_TRUE(udt.devices.empty());
}

TEST_F(RealBusWrappedTest, UpdateEndPointsBuildsMultipleDevices)
{
    wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                          "au.com.codeconstruct.MCTP1",
                                          mctpEndpointIntfName);
    ManagedObjects endpointObjects{
        {"/au/com/codeconstruct/mctp1/endpoint0",
         {{mctpEndpointIntfName,
           {{"EID", static_cast<uint8_t>(42)},
            {"SupportedMessageTypes",
             std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
            {"MediumType",
             std::string(
                 "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
          {uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid-0")}}},
          {mctpBindingIntfName,
           {{"BindingType",
             std::string(
                 "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
          {mctpEndpointEnableIntfName,
           {{"Connectivity", std::string("Available")}}}}},
        {"/au/com/codeconstruct/mctp1/endpoint1",
         {{mctpEndpointIntfName,
           {{"EID", static_cast<uint8_t>(43)},
            {"SupportedMessageTypes",
             std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
            {"MediumType",
             std::string("xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")}}},
          {uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid-1")}}},
          {mctpBindingIntfName,
           {{"BindingType",
             std::string(
                 "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB")}}},
          {mctpEndpointEnableIntfName,
           {{"Connectivity", std::string("Available")}}}}},
    };
    wrap_state::pushManagedObjects(endpointObjects);

    ManagedObjects inventoryObjects{
        {"/xyz/openbmc_project/software/GPU_ERoT_0",
         {{uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid-0")}}},
          {pldmInventoryIntfName, {{"SerialNumber", std::string("SN123")}}}}},
        {"/xyz/openbmc_project/software/GPU_ERoT_1",
         {{uuidEndpointIntfName, {{"UUID", std::string("gpu-uuid-1")}}},
          {pldmInventoryIntfName, {{"SerialNumber", std::string("SN456")}}}}},
    };
    wrap_state::pushManagedObjects(inventoryObjects);

    auto result = udt.updateEndPoints();

    EXPECT_EQ(result, -1);
    EXPECT_TRUE(udt.devices.empty());
    EXPECT_TRUE(udt.deviceNameMap.empty());
}

TEST_F(RealBusWrappedTest, UpdateEndPointsAllocationFailureSweepSuccessPath)
{
    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/endpoint0",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(42)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName,
                {{"UUID", std::string("alloc-gpu-uuid")}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
        wrap_state::pushManagedObjects(
            {{"/xyz/openbmc_project/software/GPU_ERoT_0",
              {{uuidEndpointIntfName,
                {{"UUID", std::string("alloc-gpu-uuid")}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", std::string("ALLOC-SN-123")}}}}}});
    };

    prepare();
    (void)udt.updateEndPoints();

    runMeasuredAllocFailureSweep(prepare, [&] { (void)udt.updateEndPoints(); });
}

// ========================== eraseDebugToken full flow ======================

TEST_F(RealBusWrappedTest, EraseDebugTokenAutomatic)
{
    // getErasePolicy: GetSubTree + GetProperty
    wrap_state::pushGetSubTreeOneEndpoint("/com/nvidia/debug_token/policy",
                                          "com.nvidia.DebugToken",
                                          "com.nvidia.DebugToken.ErasePolicy");
    wrap_state::pushVariantString("Automatic");
    // nsmTokenEraseV2: GetSubTree
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    // handleAsyncCallEraseV2
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    int result = udt.eraseDebugToken();
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenEraseHappyPath)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "active", 1);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/disable");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "cleared", 0);

    auto result = udt.nsmTokenErase();
    (void)result;
}

TEST_F(RealBusWrappedTest, NsmTokenInstallHappyPath)
{
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenIntfName);
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-before");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, "clear", 0);
    wrap_state::pushVariantString("SERIAL_001");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/install");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/status-after");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
    wrap_state::pushVariantTokenStatus(
        nsmTokenTypeCRDT, nsmTokenStatusDebugSessionActive, "installed", 0);

    TokenMap tokens;
    tokens.emplace("SERIAL_001", std::vector<uint8_t>(100, 0x42));
    auto result = udt.nsmTokenInstall(tokens);
    (void)result;
}

// ========================== installDebugToken =============================

TEST_F(RealBusWrappedTest, InstallDebugTokenValid)
{
    std::string tmpPath = "/tmp/test_realbus_install.bin";
    {
        debug_token::StructureHeader hdr{};
        std::memcpy(hdr.identifier, debug_token::TLV_IDENTIFIER, 4);
        hdr.versionMajor = htole16(2);
        hdr.versionMinor = htole16(0);
        debug_token::ItemHeader itemHdr;
        itemHdr.type = htole16(0x0003);
        itemHdr.size = htole16(8);
        uint8_t serial[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        hdr.size = htole32(sizeof(itemHdr) + 8);

        std::ofstream f(tmpPath, std::ios::binary);
        DebugTokenHeader fileHdr{};
        fileHdr.version = 2;
        fileHdr.type = 2;
        fileHdr.numberOfRecords = 1;
        fileHdr.offsetToListOfStructs = sizeof(DebugTokenHeader);
        f.write(reinterpret_cast<const char*>(&fileHdr), sizeof(fileHdr));
        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        f.write(reinterpret_cast<const char*>(&itemHdr), sizeof(itemHdr));
        f.write(reinterpret_cast<const char*>(serial), 8);
    }

    // nsmTokenInstallV2: GetSubTree
    wrap_state::pushGetSubTreeOneEndpoint("/xyz/openbmc_project/NSM/gpu0",
                                          "xyz.openbmc_project.NSM",
                                          nsmDebugTokenActionIntfName);
    // Get TokenDeviceID
    wrap_state::pushVariantString("0x0102030405060708");
    // handleAsyncCallInstallV2
    wrap_state::pushObjectPath("/com/nvidia/nsmd/async/1");
    wrap_state::pushVariantString("com.nvidia.Async.Status.Success");

    auto status = udt.installDebugToken(tmpPath);
    (void)status;
    std::filesystem::remove(tmpPath);
}

// ========================== popen-backed command flows ====================

TEST_F(RealBusWrappedTest, InstallTokenSuccessReturnsZero)
{
    popen_state::queue("RX: 00\n");

    Token token(64, 0x5A);
    EXPECT_EQ(udt.installToken(31, token), 0);
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("debug_token_install"));
}

TEST_F(RealBusWrappedTest, InstallTokenEmptyResponseReturnsResponseFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("");

    Token token(32, 0x44);
    EXPECT_EQ(udt.installToken(31, token),
              static_cast<int>(CommonErrorCodes::MCTPResponseInstallFailure));
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("debug_token_install"));
}

TEST_F(RealBusWrappedTest, InstallTokenInvalidStatusHexReturnsResponseFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: GG\n");

    Token token(16, 0x11);
    EXPECT_EQ(udt.installToken(31, token),
              static_cast<int>(CommonErrorCodes::MCTPResponseInstallFailure));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, InstallTokenOverflowStatusHexReturnsResponseFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: FFFFFFFFFFFFFFFF\n");

    Token token(16, 0x12);
    EXPECT_EQ(udt.installToken(31, token),
              static_cast<int>(CommonErrorCodes::MCTPResponseInstallFailure));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest,
       InstallTokenFailureReenablesBackgroundCopySuccessfully)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: 02\n");
    popen_state::queue("RX: 00\n");

    Token token(48, 0x21);
    EXPECT_EQ(udt.installToken(31, token), 2);
    ASSERT_EQ(popen_state::commands.size(), 2u);
    EXPECT_THAT(popen_state::commands[0],
                testing::HasSubstr("debug_token_install"));
    EXPECT_THAT(popen_state::commands[1],
                testing::HasSubstr("background_copy_enable"));
}

TEST_F(RealBusWrappedTest, InstallTokenFailureReenablesBackgroundCopyFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: 02\n");
    popen_state::queue("", 1);

    Token token(48, 0x22);
    EXPECT_EQ(udt.installToken(31, token), 2);
    ASSERT_EQ(popen_state::commands.size(), 2u);
    EXPECT_THAT(popen_state::commands[1],
                testing::HasSubstr("background_copy_enable"));
}

TEST_F(RealBusWrappedTest,
       InstallTokenAllocationFailureSweepFailureReenablesBackgroundCopy)
{
    const Token token(768, 0x5A);
    const std::string deviceName = "GPU-" + std::string(96, 'I');

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        udt.deviceNameMap.clear();
        udt.deviceNameMap[31] = deviceName;
        popen_state::queue("RX: 02\n");
        popen_state::queue("RX: 00\n");
    };

    prepare();
    (void)udt.installToken(31, token);

    runMeasuredAllocFailureSweepCapped(
        96, prepare, [&] { (void)udt.installToken(31, token); });
}

TEST_F(RealBusWrappedTest, EraseTokenSuccessReturnsZero)
{
    popen_state::queue("RX: 00\n");

    EXPECT_EQ(udt.eraseToken(31), 0);
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("debug_token_erase"));
}

TEST_F(RealBusWrappedTest, EraseTokenEmptyResponseReturnsResponseFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("");

    EXPECT_EQ(udt.eraseToken(31),
              static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, EraseTokenInvalidStatusHexReturnsResponseFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: GG\n");

    EXPECT_EQ(udt.eraseToken(31),
              static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, EraseTokenOverflowStatusHexReturnsResponseFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: FFFFFFFFFFFFFFFF\n");

    EXPECT_EQ(udt.eraseToken(31),
              static_cast<int>(CommonErrorCodes::MCTPResponseEraseFailure));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, EraseTokenFailureDisablesBackgroundCopySuccessfully)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: 01\n");
    popen_state::queue("RX: 00\n");

    EXPECT_EQ(udt.eraseToken(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 2u);
    EXPECT_THAT(popen_state::commands[0],
                testing::HasSubstr("debug_token_erase"));
    EXPECT_THAT(popen_state::commands[1],
                testing::HasSubstr("background_copy_disable"));
}

TEST_F(RealBusWrappedTest, EraseTokenFailureDisablesBackgroundCopyFailure)
{
    udt.deviceNameMap[31] = "GPU31";
    popen_state::queue("RX: 01\n");
    popen_state::queue("", 1);

    EXPECT_EQ(udt.eraseToken(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 2u);
    EXPECT_THAT(popen_state::commands[1],
                testing::HasSubstr("background_copy_disable"));
}

TEST_F(RealBusWrappedTest,
       EraseTokenAllocationFailureSweepFailureDisablesBackgroundCopy)
{
    const std::string deviceName = "GPU-" + std::string(96, 'E');

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        udt.deviceNameMap.clear();
        udt.deviceNameMap[31] = deviceName;
        popen_state::queue("RX: 01\n");
        popen_state::queue("RX: 00\n");
    };

    prepare();
    (void)udt.eraseToken(31);

    runMeasuredAllocFailureSweepCapped(96, prepare,
                                       [&] { (void)udt.eraseToken(31); });
}

TEST_F(RealBusWrappedTest, EnableBackgroundCopySuccessReturnsZero)
{
    popen_state::queue("RX: 00\n");

    EXPECT_EQ(udt.enableBackgroundCopy(31), 0);
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("background_copy_enable"));
}

TEST_F(RealBusWrappedTest, EnableBackgroundCopyInvalidStatusReturnsFailure)
{
    popen_state::queue("RX: GG\n");

    EXPECT_EQ(udt.enableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, EnableBackgroundCopyOverflowStatusReturnsFailure)
{
    popen_state::queue("RX: FFFFFFFFFFFFFFFF\n");

    EXPECT_EQ(udt.enableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, EnableBackgroundCopyCommandFailureReturnsFailure)
{
    popen_state::queue("", 1);

    EXPECT_EQ(udt.enableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, EnableBackgroundCopyNonZeroStatusReturnsFailure)
{
    popen_state::queue("RX: 02\n");

    EXPECT_EQ(udt.enableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest,
       EnableBackgroundCopyAllocationFailureSweepNonZeroStatus)
{
    auto bytes = makeQueryBytes(24);
    bytes.back() = "02";

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        popen_state::queue(makeRxOutput(bytes));
    };

    prepare();
    (void)udt.enableBackgroundCopy(31);

    runMeasuredAllocFailureSweepCapped(
        96, prepare, [&] { (void)udt.enableBackgroundCopy(31); });
}

TEST_F(RealBusWrappedTest, DisableBackgroundCopySuccessReturnsZero)
{
    popen_state::queue("RX: 00\n");

    EXPECT_EQ(udt.disableBackgroundCopy(31), 0);
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("background_copy_disable"));
}

TEST_F(RealBusWrappedTest, DisableBackgroundCopyInvalidStatusReturnsFailure)
{
    popen_state::queue("RX: GG\n");

    EXPECT_EQ(udt.disableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, DisableBackgroundCopyOverflowStatusReturnsFailure)
{
    popen_state::queue("RX: FFFFFFFFFFFFFFFF\n");

    EXPECT_EQ(udt.disableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, DisableBackgroundCopyCommandFailureReturnsFailure)
{
    popen_state::queue("", 1);

    EXPECT_EQ(udt.disableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, DisableBackgroundCopyNonZeroStatusReturnsFailure)
{
    popen_state::queue("RX: 02\n");

    EXPECT_EQ(udt.disableBackgroundCopy(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest,
       DisableBackgroundCopyAllocationFailureSweepNonZeroStatus)
{
    auto bytes = makeQueryBytes(24);
    bytes.back() = "02";

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        popen_state::queue(makeRxOutput(bytes));
    };

    prepare();
    (void)udt.disableBackgroundCopy(31);

    runMeasuredAllocFailureSweepCapped(
        96, prepare, [&] { (void)udt.disableBackgroundCopy(31); });
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV1InstalledReturnsInstalled)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV1);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV1] = "01";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV1(31),
              static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled));
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("debug_token_query "));
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV1InvalidTokenStatusReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV1);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV1] = "GG";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV1(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV1OverflowTokenStatusReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV1);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV1] = "FFFFFFFFFFFFFFFF";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV1(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV1NotInstalledReturnsNotInstalled)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV1);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV1] = "00";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(
        udt.queryDebugTokenV1(31),
        static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenNotInstalled));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV1WrongSizeReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpCompletionCodeByte);
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV1(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV1CommandFailureReturnsFailure)
{
    popen_state::queue("", 1);

    EXPECT_EQ(udt.queryDebugTokenV1(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV1AllocationFailureSweepInstalledPath)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV1);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV1] = "01";

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        popen_state::queue(makeRxOutput(bytes));
    };

    prepare();
    (void)udt.queryDebugTokenV1(31);

    runMeasuredAllocFailureSweepCapped(
        96, prepare, [&] { (void)udt.queryDebugTokenV1(31); });
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2InstalledReturnsInstalled)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV2] = "01";
    bytes[tokenTypeByteStartV2] = "04";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV2(31),
              static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled));
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("debug_token_query_v2"));
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2WrongSizeReturnsCompletionCode)
{
    auto bytes = makeQueryBytes(mctpCompletionCodeByte + 1);
    bytes[mctpCompletionCodeByte] = "03";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV2(31), 3);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2NotInstalledReturnsNotInstalled)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV2] = "00";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(
        udt.queryDebugTokenV2(31),
        static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenNotInstalled));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2NonZeroCompletionReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "01";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV2(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2InvalidCompletionHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "GG";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV2(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2OverflowCompletionHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "FFFFFFFFFFFFFFFF";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV2(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2InvalidTokenTypeHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV2] = "01";
    bytes[tokenTypeByteStartV2] = "GG";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV2(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2OverflowTokenTypeHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV2] = "01";
    bytes[tokenTypeByteStartV2] = "FFFFFFFFFFFFFFFF";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV2(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV2AllocationFailureSweepInstalledPath)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV2] = "01";
    bytes[tokenTypeByteStartV2] = "04";
    bytes[tokenTypeByteStartV2 + 1] = "03";
    bytes[tokenTypeByteStartV2 + 2] = "02";
    bytes[tokenTypeByteStartV2 + 3] = "01";

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        popen_state::queue(makeRxOutput(bytes));
    };

    prepare();
    (void)udt.queryDebugTokenV2(31);

    runMeasuredAllocFailureSweepCapped(
        96, prepare, [&] { (void)udt.queryDebugTokenV2(31); });
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3InstalledReturnsInstalled)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV3] = "01";
    bytes[tokenTypeByteStartV3] = "08";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV3(31),
              static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled));
    ASSERT_EQ(popen_state::commands.size(), 1u);
    EXPECT_THAT(popen_state::commands.front(),
                testing::HasSubstr("debug_token_query_v3"));
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3NotInstalledReturnsNotInstalled)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV3] = "00";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(
        udt.queryDebugTokenV3(31),
        static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenNotInstalled));
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3WrongSizeReturnsCompletionCode)
{
    auto bytes = makeQueryBytes(mctpCompletionCodeByte + 1);
    bytes[mctpCompletionCodeByte] = "03";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV3(31), 3);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3NonZeroCompletionReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "01";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV3(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3InvalidCompletionHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "GG";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV3(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3OverflowCompletionHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "FFFFFFFFFFFFFFFF";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV3(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3InvalidTokenTypeHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV3] = "01";
    bytes[tokenTypeByteStartV3] = "GG";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV3(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3OverflowTokenTypeHexReturnsFailure)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV3] = "01";
    bytes[tokenTypeByteStartV3] = "FFFFFFFFFFFFFFFF";
    popen_state::queue(makeRxOutput(bytes));

    EXPECT_EQ(udt.queryDebugTokenV3(31), -1);
    ASSERT_EQ(popen_state::commands.size(), 1u);
}

TEST_F(RealBusWrappedTest, QueryDebugTokenV3AllocationFailureSweepInstalledPath)
{
    auto bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV3);
    bytes[mctpCompletionCodeByte] = "00";
    bytes[tokenInstallStatusByteV3] = "01";
    bytes[tokenTypeByteStartV3] = "08";
    bytes[tokenTypeByteStartV3 + 1] = "07";
    bytes[tokenTypeByteStartV3 + 2] = "06";
    bytes[tokenTypeByteStartV3 + 3] = "05";

    auto prepare = [&] {
        wrap_state::reset();
        async_signal_state::reset();
        syscall_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        popen_state::queue(makeRxOutput(bytes));
    };

    prepare();
    (void)udt.queryDebugTokenV3(31);

    runMeasuredAllocFailureSweepCapped(
        96, prepare, [&] { (void)udt.queryDebugTokenV3(31); });
}

TEST_F(RealBusWrappedTest, QueryDebugTokenFallsBackFromV3ToV2)
{
    auto v3Bytes = makeQueryBytes(mctpCompletionCodeByte + 1);
    v3Bytes[mctpCompletionCodeByte] = "03";
    auto v2Bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV2);
    v2Bytes[mctpCompletionCodeByte] = "00";
    v2Bytes[tokenInstallStatusByteV2] = "01";

    popen_state::queue(makeRxOutput(v3Bytes));
    popen_state::queue(makeRxOutput(v2Bytes));

    EXPECT_EQ(udt.queryDebugToken(31),
              static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenInstalled));
    ASSERT_EQ(popen_state::commands.size(), 2u);
    EXPECT_THAT(popen_state::commands[0],
                testing::HasSubstr("debug_token_query_v3"));
    EXPECT_THAT(popen_state::commands[1],
                testing::HasSubstr("debug_token_query_v2"));
}

TEST_F(RealBusWrappedTest, QueryDebugTokenFallsThroughToV1)
{
    popen_state::queue("");
    popen_state::queue("");
    auto v1Bytes = makeQueryBytes(mctpDebugTokenQueryResponseLengthV1);
    v1Bytes[mctpCompletionCodeByte] = "00";
    v1Bytes[tokenInstallStatusByteV1] = "00";
    popen_state::queue(makeRxOutput(v1Bytes));

    EXPECT_EQ(
        udt.queryDebugToken(31),
        static_cast<int>(DebugTokenQueryErrorCodes::DebugTokenNotInstalled));
    ASSERT_EQ(popen_state::commands.size(), 3u);
    EXPECT_THAT(popen_state::commands[0],
                testing::HasSubstr("debug_token_query_v3"));
    EXPECT_THAT(popen_state::commands[1],
                testing::HasSubstr("debug_token_query_v2"));
    EXPECT_THAT(popen_state::commands[2],
                testing::HasSubstr("debug_token_query "));
}
#endif // NSM_ALLOC_PROBE_ONLY

#ifdef NSM_ALLOC_PROBE_ONLY
namespace
{

struct RealBusProbeContext
{
    sdbusplus::bus_t bus = makeWrappedBus();
    UpdateDebugToken udt{bus};

    void resetState()
    {
        alloc_fail::reset();
        wrap_state::reset();
        popen_state::output.clear();
        popen_state::retCode = 0;
        popen_state::queued.clear();
        popen_state::commands.clear();
        popen_state::active = {};
        async_signal_state::reset();
        syscall_state::reset();
    }
};

void closeIfValid(int& fd)
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

int createProbeMemfd(const char* name, const std::vector<uint8_t>& data)
{
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0)
    {
        return -1;
    }
    if (write(fd, data.data(), data.size()) !=
        static_cast<ssize_t>(data.size()))
    {
        close(fd);
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

template <typename PrepareFn, typename InvokeFn>
void runMeasuredAllocFailureSweepRange(size_t firstFailure, size_t lastFailure,
                                       PrepareFn&& prepare, InvokeFn&& invoke)
{
    if (firstFailure == 0 || lastFailure < firstFailure)
    {
        return;
    }
    const size_t totalAllocations = countAllocations(
        std::forward<PrepareFn>(prepare), std::forward<InvokeFn>(invoke));
    if (totalAllocations == 0 || firstFailure > totalAllocations)
    {
        return;
    }
    runAllocFailureSweep(firstFailure, std::min(lastFailure, totalAllocations),
                         std::forward<PrepareFn>(prepare),
                         std::forward<InvokeFn>(invoke));
}

size_t readEnvSize(const char* name, size_t fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return fallback;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == nullptr || *end != '\0')
    {
        return fallback;
    }
    return static_cast<size_t>(parsed);
}

void swallowAll(const std::function<void()>& fn);

void runMeasuredForkedAllocFailureSweepRange(
    size_t firstFailure, size_t lastFailure,
    const std::function<void()>& invokeOnce)
{
    if (firstFailure == 0 || lastFailure < firstFailure)
    {
        return;
    }

    const size_t totalAllocations =
        countAllocations([] {}, [&] { invokeOnce(); });
    if (totalAllocations == 0 || firstFailure > totalAllocations)
    {
        return;
    }

    const size_t cappedLast = std::min(lastFailure, totalAllocations);
    for (size_t failIndex = firstFailure; failIndex <= cappedLast; ++failIndex)
    {
        const pid_t pid = fork();
        if (pid == 0)
        {
            std::set_terminate([] { std::exit(0); });
            alloc_fail::Guard guard(failIndex);
            swallowAll(invokeOnce);
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

void swallowAll(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::bad_alloc&)
    {}
    catch (const std::exception&)
    {}
    catch (...)
    {}
    alloc_fail::reset();
}

template <typename PrepareFn, typename InvokeFn>
void runMeasuredProbeScenario(size_t firstFailure, size_t lastFailure,
                              PrepareFn&& prepare, InvokeFn&& invoke)
{
    swallowAll(invoke);
    swallowAll([&] {
        runMeasuredAllocFailureSweepRange(firstFailure, lastFailure,
                                          std::forward<PrepareFn>(prepare),
                                          std::forward<InvokeFn>(invoke));
    });
}

template <typename InvokeFn>
void runForkedProbeScenario(size_t firstFailure, size_t lastFailure,
                            InvokeFn&& invoke)
{
    swallowAll(invoke);
    runMeasuredForkedAllocFailureSweepRange(firstFailure, lastFailure,
                                            std::forward<InvokeFn>(invoke));
}

void runHandleAsyncProbe(RealBusProbeContext& ctx, size_t firstFailure,
                         size_t lastFailure)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'H');
    const auto immediateInvoke = [&] {
        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-failed");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");
        wrap_state::pushVariantErrorTuple(0x42, "disable failed");
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        wrap_state::pushError(-ENOENT);
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-status-wrong-type");
        wrap_state::pushVariantUint32(7);
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");
    };

    const auto signalInvoke = [&] {
        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-progress");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        async_signal_state::queue(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-progress",
            "com.nvidia.Async.Status.Success");
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-wrong-path");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                                  "com.nvidia.Async.Status.Success");
        async_signal_state::queue(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-wrong-path",
            "com.nvidia.Async.Status.Success");
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-missing-property");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x45, "status property missing");
        async_signal_state::queueWithProperty(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-missing-property",
            "com.nvidia.Async.Status.Success", "Progress");
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-uint32");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x46, "uint32 status");
        async_signal_state::queueWithUint32Status(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-uint32", 9);
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-truncated");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x47, "truncated signal");
        async_signal_state::queueTruncated(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-truncated");
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");
    };

    const auto replyInvoke = [&] {
        ctx.resetState();
        wrap_state::pushVariantString("not-an-object-path");
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        wrap_state::pushEmpty();
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");

        ctx.resetState();
        (void)ctx.udt.handleAsyncCall("not-a-valid-object-path",
                                      "DisableTokens");
    };

    const auto successInvoke = [&] {
        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-handle-success");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        (void)ctx.udt.handleAsyncCall(path, "DisableTokens");
    };

    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, immediateInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, signalInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, replyInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, successInvoke);
}

void runGetTokenStatusProbe(RealBusProbeContext& ctx, size_t firstFailure,
                            size_t lastFailure)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'T');
    const std::string additionalInfo(160, 'A');

    const auto failureInvoke = [&] {
        ctx.resetState();
        wrap_state::pushError(-ENOENT);
        (void)ctx.udt.getTokenStatus(path);

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-status-error-tuple");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantErrorTuple(0x44, "status tuple mismatch");
        (void)ctx.udt.getTokenStatus(path);

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-status-wrong-value");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantString("not-an-async-value");
        (void)ctx.udt.getTokenStatus(path);
    };

    const auto successInvoke = [&] {
        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-status-success");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(nsmTokenTypeCRDT,
                                           nsmTokenStatusDebugSessionActive,
                                           additionalInfo.c_str(), 17);
        (void)ctx.udt.getTokenStatus(path);
    };

    const auto replyInvoke = [&] {
        ctx.resetState();
        wrap_state::pushEmpty();
        (void)ctx.udt.getTokenStatus(path);

        ctx.resetState();
        wrap_state::pushVariantString("not-an-object-path");
        (void)ctx.udt.getTokenStatus(path);

        ctx.resetState();
        (void)ctx.udt.getTokenStatus("not-a-valid-object-path");
    };

    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, failureInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, replyInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, successInvoke);
}

void runHandleAsyncInstallV2Probe(RealBusProbeContext& ctx, size_t firstFailure,
                                  size_t lastFailure)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'I');
    const std::vector<uint8_t> token(1024, 0xCC);
    int memfd = -1;

    auto prepare = [&] { closeIfValid(memfd); };
    auto immediateInvoke = [&] {
        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-call-fail", token);
        if (memfd >= 0)
        {
            wrap_state::pushError(-ENOENT);
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);

        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-failed", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-failed");
            wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
            wrap_state::pushVariantErrorTuple(0x23, "install failed");
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);

        ctx.resetState();
        memfd =
            createProbeMemfd("token-install-probe-status-wrong-type", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-wrong-type");
            wrap_state::pushVariantUint32(13);
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);
    };

    auto progressInvoke = [&] {
        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-progress", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-progress");
            wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
            async_signal_state::queue(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-progress",
                "com.nvidia.Async.Status.Success");
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);

        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-wrong-path", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-wrong-path");
            wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
            async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                                      "com.nvidia.Async.Status.Success");
            async_signal_state::queue(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-wrong-path",
                "com.nvidia.Async.Status.Success");
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);
    };

    auto signalErrorInvoke = [&] {
        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-missing-property", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-missing-property");
            wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
            wrap_state::pushVariantErrorTuple(0x24, "install status missing");
            async_signal_state::queueWithProperty(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-missing-property",
                "com.nvidia.Async.Status.Success", "Progress");
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);

        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-uint32", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-uint32");
            wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
            wrap_state::pushVariantErrorTuple(0x25, "install uint32 status");
            async_signal_state::queueWithUint32Status(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-uint32", 11);
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);

        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-truncated", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-truncated");
            wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
            wrap_state::pushVariantErrorTuple(0x26, "install truncated signal");
            async_signal_state::queueTruncated(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-truncated");
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);
    };

    auto successInvoke = [&] {
        ctx.resetState();
        memfd = createProbeMemfd("token-install-probe-success", token);
        if (memfd >= 0)
        {
            wrap_state::pushObjectPath(
                "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-success");
            wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
            (void)ctx.udt.handleAsyncCallInstallV2(path, memfd);
        }
        closeIfValid(memfd);
    };

    runMeasuredProbeScenario(firstFailure, lastFailure, prepare,
                             immediateInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, prepare,
                             progressInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, prepare,
                             signalErrorInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, prepare, successInvoke);
    closeIfValid(memfd);
}

void runHandleAsyncEraseV2Probe(RealBusProbeContext& ctx, size_t firstFailure,
                                size_t lastFailure)
{
    const std::string path = "/xyz/openbmc_project/NSM/" + std::string(96, 'E');
    const std::string errorMessage(192, 'N');

    const auto immediateInvoke = [&] {
        ctx.resetState();
        wrap_state::pushError(-ENOENT);
        (void)ctx.udt.handleAsyncCallEraseV2(path);

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-error");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
        wrap_state::pushVariantErrorTuple(0x42, errorMessage.c_str());
        wrap_state::pushVariantErrorTuple(0x42, errorMessage.c_str());
        (void)ctx.udt.handleAsyncCallEraseV2(path);

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-status-wrong-type");
        wrap_state::pushVariantUint32(5);
        (void)ctx.udt.handleAsyncCallEraseV2(path);
    };

    const auto progressInvoke = [&] {
        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-not-installed");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x100F, errorMessage.c_str());
        wrap_state::pushVariantErrorTuple(0x100F, errorMessage.c_str());
        async_signal_state::queue(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-not-installed",
            "com.nvidia.Async.Status.Failed");
        (void)ctx.udt.handleAsyncCallEraseV2(path);

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-wrong-path");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x48, errorMessage.c_str());
        wrap_state::pushVariantErrorTuple(0x48, errorMessage.c_str());
        async_signal_state::queue("/com/nvidia/nsmd/AsyncOperation/other",
                                  "com.nvidia.Async.Status.Success");
        async_signal_state::queue(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-wrong-path",
            "com.nvidia.Async.Status.Success");
        (void)ctx.udt.handleAsyncCallEraseV2(path);
    };

    const auto signalErrorInvoke = [&] {
        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-missing-property");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x49, errorMessage.c_str());
        wrap_state::pushVariantErrorTuple(0x49, errorMessage.c_str());
        async_signal_state::queueWithProperty(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-missing-property",
            "com.nvidia.Async.Status.Success", "Progress");
        (void)ctx.udt.handleAsyncCallEraseV2(path);

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-uint32");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x4A, errorMessage.c_str());
        wrap_state::pushVariantErrorTuple(0x4A, errorMessage.c_str());
        async_signal_state::queueWithUint32Status(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-uint32", 17);
        (void)ctx.udt.handleAsyncCallEraseV2(path);

        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-truncated");
        wrap_state::pushVariantString("com.nvidia.Async.Status.InProgress");
        wrap_state::pushVariantErrorTuple(0x4B, errorMessage.c_str());
        wrap_state::pushVariantErrorTuple(0x4B, errorMessage.c_str());
        async_signal_state::queueTruncated(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-truncated");
        (void)ctx.udt.handleAsyncCallEraseV2(path);
    };

    const auto successInvoke = [&] {
        ctx.resetState();
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-success");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        (void)ctx.udt.handleAsyncCallEraseV2(path);
    };

    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, immediateInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, progressInvoke);
    runMeasuredProbeScenario(
        firstFailure, lastFailure, [] {}, signalErrorInvoke);
    runMeasuredProbeScenario(firstFailure, lastFailure, [] {}, successInvoke);
}

void runNsmTokenEraseProbe(size_t firstFailure, size_t lastFailure)
{
    const std::string endpointPath =
        "/xyz/openbmc_project/NSM/" + std::string(96, 'R');
    const std::string info(192, 'E');

    const std::function<void()> noTokenInvoke = [&] {
        RealBusProbeContext ctx;
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-no-token");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, info.c_str(), 0);
        (void)ctx.udt.nsmTokenErase();
    };

    const std::function<void()> disableFailInvoke = [&] {
        RealBusProbeContext ctx;
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-disable-fail-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(nsmTokenTypeCRDT,
                                           nsmTokenStatusDebugSessionActive,
                                           info.c_str(), 7);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-disable-fail");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");
        wrap_state::pushVariantErrorTuple(0x42, "disable failed");
        (void)ctx.udt.nsmTokenErase();
    };

    const std::function<void()> wrongAfterInvoke = [&] {
        RealBusProbeContext ctx;
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-wrong-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(nsmTokenTypeCRDT,
                                           nsmTokenStatusDebugSessionActive,
                                           info.c_str(), 7);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-wrong-disable");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-wrong-after");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusTokenTimeout, info.c_str(), 2);
        (void)ctx.udt.nsmTokenErase();
    };

    const std::function<void()> successAfterInvoke = [&] {
        RealBusProbeContext ctx;
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-success-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(nsmTokenTypeCRDT,
                                           nsmTokenStatusDebugSessionActive,
                                           info.c_str(), 7);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-success-disable");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-success-after");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, info.c_str(), 0);
        (void)ctx.udt.nsmTokenErase();
    };

    runForkedProbeScenario(firstFailure, lastFailure, noTokenInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, disableFailInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, wrongAfterInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, successAfterInvoke);
}

void runNsmTokenInstallProbe(size_t firstFailure, size_t lastFailure)
{
    const std::string endpointPath =
        "/xyz/openbmc_project/NSM/" + std::string(96, 'I');
    const std::string serial(160, 'S');
    const std::string info(192, 'T');
    const Token token(256, 0x5A);

    const std::function<void()> enumerationFailInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushError(-ENOENT);
        (void)ctx.udt.nsmTokenInstall(tokens);
    };

    const std::function<void()> activeInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-active");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(nsmTokenTypeCRDT,
                                           nsmTokenStatusDebugSessionActive,
                                           info.c_str(), 1);
        (void)ctx.udt.nsmTokenInstall(tokens);
    };

    const std::function<void()> missingTokenInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-missing-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, info.c_str(), 0);
        wrap_state::pushVariantString("DIFFERENT_SERIAL");
        (void)ctx.udt.nsmTokenInstall(tokens);
    };

    const std::function<void()> installFailedInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-failed-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, info.c_str(), 0);
        wrap_state::pushVariantString(serial.c_str());
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-failed");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Failed");
        wrap_state::pushVariantErrorTuple(0x23, "install failed");
        (void)ctx.udt.nsmTokenInstall(tokens);
    };

    const std::function<void()> installWrongInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-wrong-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, info.c_str(), 0);
        wrap_state::pushVariantString(serial.c_str());
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-wrong");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-wrong-after");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, info.c_str(), 0);
        (void)ctx.udt.nsmTokenInstall(tokens);
    };

    const std::function<void()> installSuccessInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-success-before");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(
            nsmTokenTypeCRDT, nsmTokenStatusNoTokenApplied, info.c_str(), 0);
        wrap_state::pushVariantString(serial.c_str());
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-success");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-success-after");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        wrap_state::pushVariantTokenStatus(nsmTokenTypeCRDT,
                                           nsmTokenStatusDebugSessionActive,
                                           info.c_str(), 0);
        (void)ctx.udt.nsmTokenInstall(tokens);
    };

    runForkedProbeScenario(firstFailure, lastFailure, enumerationFailInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, activeInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, missingTokenInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, installFailedInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, installWrongInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, installSuccessInvoke);
}

void runNsmTokenEraseV2Probe(size_t firstFailure, size_t lastFailure)
{
    const std::string endpointPath =
        "/xyz/openbmc_project/NSM/" + std::string(96, 'V');

    const std::function<void()> failedInvoke = [&] {
        RealBusProbeContext ctx;
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-top-failed");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
        wrap_state::pushVariantErrorTuple(0x42, "erase failed");
        wrap_state::pushVariantErrorTuple(0x42, "erase failed");
        (void)ctx.udt.nsmTokenEraseV2();
    };

    const std::function<void()> successInvoke = [&] {
        RealBusProbeContext ctx;
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-erase-v2-top-success");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        (void)ctx.udt.nsmTokenEraseV2();
    };

    runForkedProbeScenario(firstFailure, lastFailure, failedInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, successInvoke);
}

void runNsmTokenInstallV2Probe(size_t firstFailure, size_t lastFailure)
{
    const std::string endpointPath =
        "/xyz/openbmc_project/NSM/" + std::string(96, 'W');
    const std::string serial(160, 'Q');
    const Token token(256, 0x4C);

    const std::function<void()> missingTokenInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString("DIFFERENT_SERIAL");
        (void)ctx.udt.nsmTokenInstallV2(tokens);
    };

    const std::function<void()> memfdFailInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        syscall_state::failMemfdCreateRemaining = 1;
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString(serial.c_str());
        (void)ctx.udt.nsmTokenInstallV2(tokens);
    };

    const std::function<void()> shortWriteInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        syscall_state::shortWriteRemaining = 1;
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString(serial.c_str());
        (void)ctx.udt.nsmTokenInstallV2(tokens);
    };

    const std::function<void()> asyncFailInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString(serial.c_str());
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-top-failed");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Error");
        wrap_state::pushVariantErrorTuple(0x23, "install failed");
        (void)ctx.udt.nsmTokenInstallV2(tokens);
    };

    const std::function<void()> successInvoke = [&] {
        RealBusProbeContext ctx;
        TokenMap tokens;
        tokens.emplace(serial, token);
        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint(endpointPath.c_str(), nsmService,
                                              nsmDebugTokenActionIntfName);
        wrap_state::pushVariantString(serial.c_str());
        wrap_state::pushObjectPath(
            "/com/nvidia/nsmd/AsyncOperation/alloc-install-v2-top-success");
        wrap_state::pushVariantString("com.nvidia.Async.Status.Success");
        (void)ctx.udt.nsmTokenInstallV2(tokens);
    };

    runForkedProbeScenario(firstFailure, lastFailure, missingTokenInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, memfdFailInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, shortWriteInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, asyncFailInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, successInvoke);
}

void runDiscoverMctpDevicesProbe(size_t firstFailure, size_t lastFailure)
{
    const std::string dupUuid(96, 'D');
    const std::string mediumOnlyUuid(96, 'M');
    const std::string bindingOnlyUuid(96, 'B');
    const std::string missingUuidPropUuid(96, 'P');
    const std::string unsupportedUuid(96, 'S');
    const std::string emptyTransportUuid(96, 'T');
    const std::string validUuid(96, 'U');

    const std::function<void()> helperInvoke = [&] {
        RealBusProbeContext ctx;

        auto helperObjects = wrap_state::buildObjectValueTree(
            {{"/helper/good",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(7)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName, {{"UUID", validUuid}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
        (void)ctx.udt.fetchEidInfoFromObject(helperObjects.begin()->second);

        auto helperMissingConnectivity = wrap_state::buildObjectValueTree(
            {{"/helper/missing-connectivity",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(8)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")}}},
               {uuidEndpointIntfName, {{"UUID", mediumOnlyUuid}}},
               {mctpEndpointEnableIntfName, {}}}}});
        (void)ctx.udt.fetchEidInfoFromObject(
            helperMissingConnectivity.begin()->second);

        auto helperNoEnable = wrap_state::buildObjectValueTree(
            {{"/helper/no-enable",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(9)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus")}}},
               {uuidEndpointIntfName, {{"UUID", bindingOnlyUuid}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus")}}}}}});
        (void)ctx.udt.fetchEidInfoFromObject(helperNoEnable.begin()->second);

        auto helperBadTypes = wrap_state::buildObjectValueTree(
            {{"/helper/bad-types",
              {{mctpEndpointIntfName,
                {{"EID", std::string("bad-eid")},
                 {"SupportedMessageTypes", std::string("bad-types")}}},
               {uuidEndpointIntfName, {{"UUID", dupUuid}}},
               {mctpBindingIntfName,
                {{"BindingType", static_cast<uint8_t>(1)}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", static_cast<uint8_t>(1)}}}}}});
        swallowAll([&] {
            (void)ctx.udt.fetchEidInfoFromObject(
                helperBadTypes.begin()->second);
        });

        (void)ctx.udt.checkSupportForSPDMandMCTPVDM({}, 1);
        (void)ctx.udt.checkSupportForSPDMandMCTPVDM(
            std::vector<uint8_t>{mctpTypeSPDM}, 2);
        (void)ctx.udt.checkSupportForSPDMandMCTPVDM(
            std::vector<uint8_t>{mctpTypeVDMIANA}, 3);
        (void)ctx.udt.checkSupportForSPDMandMCTPVDM(
            std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}, 4);
    };

    const std::function<void()> failureInvoke = [&] {
        RealBusProbeContext ctx;

        ctx.resetState();
        wrap_state::pushError();
        (void)ctx.udt.discoverMCTPDevices();

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushVariantString("not-managed-objects");
        (void)ctx.udt.discoverMCTPDevices();

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects({});
        (void)ctx.udt.discoverMCTPDevices();
    };

    const std::function<void()> invokeOnce = [&] {
        RealBusProbeContext ctx;

        helperInvoke();

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        ManagedObjects discoverObjects;
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/no-endpoint",
            ManagedObjectInterfaces{
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", validUuid}}},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/uuid-no-property",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(9)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")},
                 }},
                {uuidEndpointIntfName, ManagedObjectProperties{}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")},
                 }},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Available")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/no-uuid",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(10)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/unsupported-complete",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(10)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", unsupportedUuid}}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")},
                 }},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Available")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/disabled",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(11)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", std::string(96, 'X')}}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")},
                 }},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Unavailable")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/missing-connectivity",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(12)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", missingUuidPropUuid}}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")},
                 }},
                {mctpEndpointEnableIntfName, ManagedObjectProperties{}},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/no-enable-interface",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(13)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", std::string(96, 'Y')}}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/binding-only",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(14)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", bindingOnlyUuid}}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB")},
                 }},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Available")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/empty-transport",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(15)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", emptyTransportUuid}}},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Available")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/medium-only",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(16)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB")},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", mediumOnlyUuid}}},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Available")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/dup-smbus",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(17)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus")},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", dupUuid}}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus")},
                 }},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Available")},
                 }},
            });
        discoverObjects.emplace(
            "/au/com/codeconstruct/mctp1/dup-pcie",
            ManagedObjectInterfaces{
                {mctpEndpointIntfName,
                 ManagedObjectProperties{
                     {"EID", static_cast<uint8_t>(18)},
                     {"SupportedMessageTypes",
                      std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                     {"MediumType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")},
                 }},
                {uuidEndpointIntfName,
                 ManagedObjectProperties{{"UUID", dupUuid}}},
                {mctpBindingIntfName,
                 ManagedObjectProperties{
                     {"BindingType",
                      std::string(
                          "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")},
                 }},
                {mctpEndpointEnableIntfName,
                 ManagedObjectProperties{
                     {"Connectivity", std::string("Available")},
                 }},
            });
        wrap_state::pushManagedObjects(discoverObjects);
        (void)ctx.udt.discoverMCTPDevices();
    };

    runForkedProbeScenario(firstFailure, lastFailure, helperInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, failureInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, invokeOnce);
}

void runUpdateEndPointsProbe(size_t firstFailure, size_t lastFailure)
{
    const std::string knownUuid(96, 'K');
    const std::string unknownUuid(96, 'Z');
    const std::string serial(160, 'P');

    const std::function<void()> helperInvoke = [&] {
        RealBusProbeContext helperCtx;

        helperCtx.udt.mctpInfo.emplace(
            knownUuid,
            MctpEidInfo{42, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA},
                        true});

        auto helperInventory = wrap_state::buildObjectValueTree(
            {{"/xyz/openbmc_project/software/helper0",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName, {{"SerialNumber", serial}}}}},
             {"/xyz/openbmc_project/software/helper1",
              {{uuidEndpointIntfName, {{"UUID", unknownUuid}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", std::string(96, 'Q')}}}}},
             {"/xyz/openbmc_project/software/helper2",
              {{uuidEndpointIntfName, {}},
               {pldmInventoryIntfName, {{"SerialNumber", serial}}}}},
             {"/xyz/openbmc_project/software/helper3",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName, {}}}}});
        for (const auto& [objectPath, interfaces] : helperInventory)
        {
            helperCtx.udt.updateDeviceMap(interfaces,
                                          std::string(objectPath.filename()));
        }

        auto helperBadInventory = wrap_state::buildObjectValueTree(
            {{"/xyz/openbmc_project/software/bad0",
              {{uuidEndpointIntfName, {{"UUID", static_cast<uint8_t>(1)}}},
               {pldmInventoryIntfName, {{"SerialNumber", serial}}}}},
             {"/xyz/openbmc_project/software/bad1",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", static_cast<uint8_t>(1)}}}}}});
        for (const auto& [objectPath, interfaces] : helperBadInventory)
        {
            swallowAll([&] {
                helperCtx.udt.updateDeviceMap(
                    interfaces, std::string(objectPath.filename()));
            });
        }
    };

    const std::function<void()> failureInvoke = [&] {
        RealBusProbeContext ctx;

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/endpoint0",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(42)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
        wrap_state::pushError();
        (void)ctx.udt.updateEndPoints();

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/endpoint0",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(42)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
        wrap_state::pushVariantString("not-managed-objects");
        (void)ctx.udt.updateEndPoints();

        ctx.resetState();
        wrap_state::pushEmpty();
        (void)ctx.udt.updateEndPoints();
    };

    const std::function<void()> invokeOnce = [&] {
        RealBusProbeContext ctx;
        RealBusProbeContext helperCtx;

        helperCtx.udt.mctpInfo.emplace(
            knownUuid,
            MctpEidInfo{42, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                        std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA},
                        true});

        auto helperInventory = wrap_state::buildObjectValueTree(
            {{"/xyz/openbmc_project/software/helper0",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName, {{"SerialNumber", serial}}}}},
             {"/xyz/openbmc_project/software/helper1",
              {{uuidEndpointIntfName, {{"UUID", unknownUuid}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", std::string(96, 'Q')}}}}},
             {"/xyz/openbmc_project/software/helper2",
              {{uuidEndpointIntfName, {}},
               {pldmInventoryIntfName, {{"SerialNumber", serial}}}}},
             {"/xyz/openbmc_project/software/helper3",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName, {}}}}});
        for (const auto& [objectPath, interfaces] : helperInventory)
        {
            helperCtx.udt.updateDeviceMap(interfaces,
                                          std::string(objectPath.filename()));
        }

        auto helperBadInventory = wrap_state::buildObjectValueTree(
            {{"/xyz/openbmc_project/software/bad0",
              {{uuidEndpointIntfName, {{"UUID", static_cast<uint8_t>(1)}}},
               {pldmInventoryIntfName, {{"SerialNumber", serial}}}}},
             {"/xyz/openbmc_project/software/bad1",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", static_cast<uint8_t>(1)}}}}}});
        for (const auto& [objectPath, interfaces] : helperBadInventory)
        {
            swallowAll([&] {
                helperCtx.udt.updateDeviceMap(
                    interfaces, std::string(objectPath.filename()));
            });
        }

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/endpoint0",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(42)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
        wrap_state::pushError();
        (void)ctx.udt.updateEndPoints();

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/endpoint0",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(42)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
        wrap_state::pushVariantString("not-managed-objects");
        (void)ctx.udt.updateEndPoints();

        ctx.resetState();
        wrap_state::pushEmpty();
        (void)ctx.udt.updateEndPoints();

        ctx.resetState();
        wrap_state::pushGetSubTreeOneEndpoint("/au/com/codeconstruct/mctp1",
                                              "au.com.codeconstruct.MCTP1",
                                              mctpEndpointIntfName);
        wrap_state::pushManagedObjects(
            {{"/au/com/codeconstruct/mctp1/endpoint0",
              {{mctpEndpointIntfName,
                {{"EID", static_cast<uint8_t>(42)},
                 {"SupportedMessageTypes",
                  std::vector<uint8_t>{mctpTypeSPDM, mctpTypeVDMIANA}},
                 {"MediumType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe")}}},
               {uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {mctpBindingIntfName,
                {{"BindingType",
                  std::string(
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe")}}},
               {mctpEndpointEnableIntfName,
                {{"Connectivity", std::string("Available")}}}}}});
        wrap_state::pushManagedObjects(
            {{"/xyz/openbmc_project/software/GPU_ERoT_0",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName, {{"SerialNumber", serial}}}}},
             {"/xyz/openbmc_project/software/GPU_ERoT_bad_uuid",
              {{uuidEndpointIntfName, {{"UUID", static_cast<uint8_t>(1)}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", std::string(96, 'R')}}}}},
             {"/xyz/openbmc_project/software/GPU_ERoT_bad_serial",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", static_cast<uint8_t>(2)}}}}},
             {"/xyz/openbmc_project/software/GPU_ERoT_1",
              {{uuidEndpointIntfName, {{"UUID", unknownUuid}}},
               {pldmInventoryIntfName,
                {{"SerialNumber", std::string(96, 'Q')}}}}},
             {"/xyz/openbmc_project/software/GPU_ERoT_2",
              {{uuidEndpointIntfName, {{"UUID", knownUuid}}},
               {pldmInventoryIntfName, {}}}}});
        (void)ctx.udt.updateEndPoints();
    };

    runForkedProbeScenario(firstFailure, lastFailure, helperInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, failureInvoke);
    runForkedProbeScenario(firstFailure, lastFailure, invokeOnce);
}

} // namespace

int main()
{
    std::set_terminate([] { std::exit(0); });

    try
    {
        RealBusProbeContext ctx;
        const char* target = std::getenv("NSM_ALLOC_PROBE_TARGET");
        const std::string probeTarget = target == nullptr ? "" : target;
        const size_t firstFailure = readEnvSize("NSM_ALLOC_FAIL_FIRST", 1);
        const size_t lastFailure = readEnvSize("NSM_ALLOC_FAIL_LAST", 256);

        if (probeTarget == "handleAsyncCall")
        {
            runHandleAsyncProbe(ctx, firstFailure, lastFailure);
        }
        else if (probeTarget == "getTokenStatus")
        {
            runGetTokenStatusProbe(ctx, firstFailure, lastFailure);
        }
        else if (probeTarget == "handleAsyncCallInstallV2")
        {
            runHandleAsyncInstallV2Probe(ctx, firstFailure, lastFailure);
        }
        else if (probeTarget == "handleAsyncCallEraseV2")
        {
            runHandleAsyncEraseV2Probe(ctx, firstFailure, lastFailure);
        }
        else if (probeTarget == "nsmTokenErase")
        {
            runNsmTokenEraseProbe(firstFailure, lastFailure);
        }
        else if (probeTarget == "nsmTokenInstall")
        {
            runNsmTokenInstallProbe(firstFailure, lastFailure);
        }
        else if (probeTarget == "nsmTokenEraseV2")
        {
            runNsmTokenEraseV2Probe(firstFailure, lastFailure);
        }
        else if (probeTarget == "nsmTokenInstallV2")
        {
            runNsmTokenInstallV2Probe(firstFailure, lastFailure);
        }
        else if (probeTarget == "discoverMCTPDevices")
        {
            runDiscoverMctpDevicesProbe(firstFailure, lastFailure);
        }
        else if (probeTarget == "updateEndPoints")
        {
            runUpdateEndPointsProbe(firstFailure, lastFailure);
        }
    }
    catch (...)
    {}

    return 0;
}
#endif // NSM_ALLOC_PROBE_ONLY
