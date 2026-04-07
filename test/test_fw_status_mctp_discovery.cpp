#include <sys/epoll.h>
#include <sys/socket.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#pragma GCC push_options
#pragma GCC optimize("O0")
#define private public
#define protected public
#include "../fw-status/mctp_util/mctp_endpoint_discovery.cpp"
#include "../fw-status/mctp_util/mctp_vdm_helper.cpp"
#include "../fw-status/mctp_util/socket_handler.cpp"
#undef protected
#undef private
#pragma GCC pop_options

namespace
{

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

struct SyscallState
{
    int pipeFds[2] = {-1, -1};
    ssize_t sendtoResult = 0;
    int socketResult = -1;
    int getsockoptResult = 0;
    int bindResult = 0;
    ssize_t recvPeekResult = -1;
    std::vector<uint8_t> recvfromData{};
    ssize_t recvfromResult = -1;
    uint8_t recvfromType = mctp_vdm::MessageType;
    int setEnabledResult = 0;
};

SyscallState fakeSys{};

struct BusCallState
{
    bool failGetSubTree = false;
    bool passThroughAll = false;
    int getSubTreeCallCount = 0;
    int managedObjectsCallCount = 0;
    mctp_vdm::GetSubTreeResponse getSubTreeResponse{};
    std::deque<std::optional<dbus::ObjectValueTree>> managedObjectResponses{};
    std::map<std::string, dbus::ObjectValueTree> managedObjects{};
    std::set<std::string> failManagedObjects{};

    void reset()
    {
        failGetSubTree = false;
        passThroughAll = false;
        getSubTreeCallCount = 0;
        managedObjectsCallCount = 0;
        getSubTreeResponse.clear();
        managedObjectResponses.clear();
        managedObjects.clear();
        failManagedObjects.clear();
    }
};

BusCallState fakeBusCalls{};

void resetSyscalls()
{
    if (fakeSys.pipeFds[0] >= 0)
    {
        close(fakeSys.pipeFds[0]);
    }
    if (fakeSys.pipeFds[1] >= 0)
    {
        close(fakeSys.pipeFds[1]);
    }

    fakeSys = {};
    ASSERT_EQ(pipe(fakeSys.pipeFds), 0);
    fakeSys.sendtoResult = 0;
    fakeSys.socketResult = fakeSys.pipeFds[0];
    fakeSys.recvPeekResult = -1;
    fakeSys.recvfromResult = -1;
    fakeSys.recvfromType = mctp_vdm::MessageType;
    fakeSys.setEnabledResult = 0;
}

mctp::Request makeRuntimeRequest(uint8_t eid, uint8_t instanceId, uint8_t type,
                                 uint8_t command)
{
    mctp::Request request(sizeof(mctp_vdm::Message));
    auto* msg = reinterpret_cast<mctp_vdm::Message*>(request.data());
    msg->hdr = {};
    msg->hdr.request = 1;
    msg->hdr.instanceId = instanceId;
    msg->hdr.msgType = type;
    msg->hdr.commandCode = command;
    msg->payload[0] = eid;
    fakeSys.sendtoResult = static_cast<ssize_t>(request.size());
    return request;
}

mctp_vdm::requester::Coroutine completedCoroutine(uint8_t value)
{
    co_return value;
}

mctp_vdm::requester::Coroutine suspendedCoroutine(uint8_t value)
{
    co_await std::suspend_always{};
    co_return value;
}

mctp_vdm::requester::Coroutine childCoroutine(std::vector<uint8_t>& trace)
{
    trace.push_back(1);
    co_return 21;
}

mctp_vdm::requester::Coroutine parentCoroutine(std::vector<uint8_t>& trace)
{
    trace.push_back(2);
    auto childValue = co_await childCoroutine(trace);
    trace.push_back(childValue);
    co_return static_cast<uint8_t>(childValue + 1);
}

class StopBranchTimer : public mctp_vdm::requester::RequestRetryTimer
{
  public:
    explicit StopBranchTimer(sdeventplus::Event& event) :
        RequestRetryTimer(event, 0, std::chrono::milliseconds(1))
    {}

    using RequestRetryTimer::stop;

  private:
    int send() const override
    {
        return 0;
    }
};

class ZeroRetrySuccessTimer : public mctp_vdm::requester::RequestRetryTimer
{
  public:
    explicit ZeroRetrySuccessTimer(sdeventplus::Event& event) :
        RequestRetryTimer(event, 0, std::chrono::milliseconds(1))
    {}

    int sendCount = 0;

  private:
    int send() const override
    {
        ++const_cast<ZeroRetrySuccessTimer*>(this)->sendCount;
        return 0;
    }
};

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
        sd_bus_message_unref(*reply);
        *reply = nullptr;
        return -EIO;
    }
}

int makeGetSubTreeReply(sd_bus_message* request, sd_bus_message** reply,
                        const mctp_vdm::GetSubTreeResponse& responseData)
{
    return makeMethodReturn(
        request, reply, [&](auto& response) { response.append(responseData); });
}

struct DiscoveryCapture : public mctp_vdm::MctpDiscoveryHandlerIntf
{
    int callCount = 0;
    mctp::Infos infos{};

    void handleMctpEndpoints(const mctp::Infos& mctpInfos) override
    {
        ++callCount;
        infos = mctpInfos;
    }
};

class FWStatusMctpDiscoveryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        resetSyscalls();
        fakeBusCalls.reset();
    }

    void TearDown() override
    {
        if (fakeSys.pipeFds[0] >= 0)
        {
            close(fakeSys.pipeFds[0]);
            fakeSys.pipeFds[0] = -1;
        }
        if (fakeSys.pipeFds[1] >= 0)
        {
            close(fakeSys.pipeFds[1]);
            fakeSys.pipeFds[1] = -1;
        }
    }
};

dbus::InterfaceMap makeMctpInterfaces(uint8_t eid, const std::string& uuid,
                                      const std::vector<uint8_t>& address,
                                      const std::vector<uint8_t>& msgTypes,
                                      const std::string& medium)
{
    return {
        {mctp::UUIDInterface, {{"UUID", uuid}}},
        {std::string(mctp_vdm::MctpDiscovery::unixSocketIntfName),
         {{"Type", static_cast<uint8_t>(SOCK_DGRAM)},
          {"Protocol", static_cast<size_t>(0)},
          {"Address", address}}},
        {"xyz.openbmc_project.MCTP.Endpoint",
         {{"EID", static_cast<size_t>(eid)},
          {"SupportedMessageTypes", msgTypes},
          {"MediumType", medium},
          {"NetworkId", static_cast<size_t>(1)}}},
    };
}

std::vector<uint8_t> makeResponseBytes(uint8_t instanceId, uint8_t msgType,
                                       uint8_t commandCode,
                                       std::initializer_list<uint8_t> payload)
{
    std::vector<uint8_t> bytes(sizeof(mctp_vdm::MsgHeader) + payload.size());
    auto* response = reinterpret_cast<mctp_vdm::Message*>(bytes.data());
    response->hdr = {};
    response->hdr.request = 0;
    response->hdr.instanceId = instanceId;
    response->hdr.msgType = msgType;
    response->hdr.commandCode = commandCode;
    std::copy(payload.begin(), payload.end(), response->payload);
    return bytes;
}

} // namespace

extern "C" int __real_sd_bus_call(sd_bus*, sd_bus_message*, uint64_t,
                                  sd_bus_error*, sd_bus_message**);

extern "C" ssize_t __wrap_sendto(int, const void*, size_t len, int,
                                 const struct sockaddr*, socklen_t)
{
    if (fakeSys.sendtoResult < 0)
    {
        errno = EIO;
        return -1;
    }
    return fakeSys.sendtoResult ? fakeSys.sendtoResult
                                : static_cast<ssize_t>(len);
}

extern "C" int __wrap_socket(int, int, int)
{
    if (fakeSys.socketResult < 0)
    {
        errno = EMFILE;
        return -1;
    }
    return fakeSys.socketResult;
}

extern "C" int __wrap_getsockopt(int, int, int, void* optval, socklen_t* optlen)
{
    if (fakeSys.getsockoptResult < 0)
    {
        errno = ENOTSOCK;
        return -1;
    }
    if (optval && optlen && *optlen == sizeof(int))
    {
        *reinterpret_cast<int*>(optval) = 256;
    }
    return 0;
}

extern "C" int __wrap_bind(int, const struct sockaddr*, socklen_t)
{
    if (fakeSys.bindResult < 0)
    {
        errno = EADDRINUSE;
        return -1;
    }
    return 0;
}

extern "C" ssize_t __wrap_recv(int, void*, size_t, int)
{
    if (fakeSys.recvPeekResult >= 0)
    {
        return fakeSys.recvPeekResult;
    }
    errno = EAGAIN;
    return -1;
}

extern "C" int __real_sd_event_source_set_enabled(sd_event_source*, int);

extern "C" int __wrap_sd_event_source_set_enabled(sd_event_source* source,
                                                  int enabled)
{
    if (fakeSys.setEnabledResult < 0)
    {
        return fakeSys.setEnabledResult;
    }
    return __real_sd_event_source_set_enabled(source, enabled);
}

extern "C" ssize_t __wrap_recvfrom(int, void* buf, size_t len, int,
                                   struct sockaddr* addr, socklen_t*)
{
    if (fakeSys.recvfromResult < 0)
    {
        errno = EIO;
        return -1;
    }

    if (addr)
    {
        auto* mctpAddr = reinterpret_cast<sockaddr_mctp*>(addr);
        std::memset(mctpAddr, 0, sizeof(*mctpAddr));
        mctpAddr->smctp_type = fakeSys.recvfromType;
        mctpAddr->smctp_addr.s_addr = 9;
    }

    const auto count =
        std::min(len, static_cast<size_t>(fakeSys.recvfromData.size()));
    std::memcpy(buf, fakeSys.recvfromData.data(), count);
    return fakeSys.recvfromResult;
}

extern "C" int __wrap_sd_bus_call(sd_bus*, sd_bus_message* request, uint64_t,
                                  sd_bus_error* ret_error,
                                  sd_bus_message** reply)
{
    const char* interface = sd_bus_message_get_interface(request);
    const char* member = sd_bus_message_get_member(request);
    const char* destination = sd_bus_message_get_destination(request);

    const bool isGetSubTree =
        (member != nullptr && std::string_view(member) == "GetSubTree") ||
        (sd_bus_message_is_method_call(request, mapper::Interface,
                                       "GetSubTree") > 0) ||
        ((interface != nullptr) &&
         std::string_view(interface) == mapper::Interface &&
         destination != nullptr &&
         std::string_view(destination) == mapper::Service);

    if (fakeBusCalls.passThroughAll)
    {
        return __real_sd_bus_call(sd_bus_message_get_bus(request), request, 0,
                                  ret_error, reply);
    }

    if (isGetSubTree)
    {
        ++fakeBusCalls.getSubTreeCallCount;
        if (fakeBusCalls.failGetSubTree)
        {
            return -EIO;
        }
        return makeGetSubTreeReply(request, reply,
                                   fakeBusCalls.getSubTreeResponse);
    }

    if ((member != nullptr &&
         std::string_view(member) == "GetManagedObjects") ||
        (sd_bus_message_is_method_call(request,
                                       "org.freedesktop.DBus.ObjectManager",
                                       "GetManagedObjects") > 0) ||
        ((interface != nullptr) &&
         std::string_view(interface) == "org.freedesktop.DBus.ObjectManager"))
    {
        ++fakeBusCalls.managedObjectsCallCount;
        if (!fakeBusCalls.managedObjectResponses.empty())
        {
            auto responseData =
                std::move(fakeBusCalls.managedObjectResponses.front());
            fakeBusCalls.managedObjectResponses.pop_front();
            if (!responseData.has_value())
            {
                return -EIO;
            }
            return makeMethodReturn(request, reply, [&](auto& response) {
                response.append(*responseData);
            });
        }

        const std::string service = destination ? destination : "";
        if (fakeBusCalls.failManagedObjects.contains(service))
        {
            return -EIO;
        }
        return makeMethodReturn(request, reply, [&](auto& response) {
            response.append(fakeBusCalls.managedObjects[service]);
        });
    }

    return __real_sd_bus_call(sd_bus_message_get_bus(request), request, 0,
                              ret_error, reply);
}

TEST_F(FWStatusMctpDiscoveryTest, DiscoveryCoversConstructorPopulateAndSignal)
{
    auto bus = sdbusplus::bus::new_bus();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture});
    EXPECT_GE(capture.callCount, 1);

    mctp::Infos infos;
    const auto validInterfaces =
        makeMctpInterfaces(9, "uuid-1", {1, 2, 3}, {mctp_vdm::MessageType},
                           "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
    discovery.populateMctpInfo(validInterfaces, infos);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(std::get<0>(infos.front()), 9);
    EXPECT_EQ(socketHandler.eidToSockMap.count(9), 1u);

    const auto missingAddress =
        makeMctpInterfaces(10, "uuid-2", {}, {mctp_vdm::MessageType},
                           "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");
    discovery.populateMctpInfo(missingAddress, infos);
    EXPECT_EQ(infos.size(), 1u);

    auto brokenInterfaces = validInterfaces;
    brokenInterfaces["xyz.openbmc_project.MCTP.Endpoint"]
                    ["SupportedMessageTypes"] = std::string("not-a-vector");
    discovery.populateMctpInfo(brokenInterfaces, infos);
    EXPECT_EQ(infos.size(), 1u);

    auto msg = bus.new_method_call(
        "xyz.openbmc_project.FakeService", "/xyz/openbmc_project/fake",
        "xyz.openbmc_project.FakeInterface", "FakeMethod");
    msg.append(
        sdbusplus::message::object_path("/xyz/openbmc_project/mctp/device0"),
        validInterfaces);
    EXPECT_ANY_THROW(discovery.discoverEndpoints(msg));

    discovery.handleMctpEndpoints({});
    EXPECT_GE(capture.callCount, 2);
}

TEST_F(FWStatusMctpDiscoveryTest, HelperCoversSuccessFailureAndNoOpHandler)
{
    auto bus = sdbusplus::bus::new_bus();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    socketHandler.registerMctpEndpoint(9, SOCK_DGRAM, 0, {1, 2, 3});

    MCTPVdmHelper helper(bus, requester, socketHandler, ids);
    helper.handleMctpEndpoints({});

    const mctp_vdm::Message* responseMsg = nullptr;
    size_t responseLen = 0;
    auto success = helper.queryBootStatus(9, responseMsg, responseLen);
    ASSERT_NE(socketHandler.io, nullptr);
    fakeSys.recvfromData =
        makeResponseBytes(0, nvidiaMsgType, 0x05, {0x00, 0xAA, 0x55});
    fakeSys.recvPeekResult = fakeSys.recvfromData.size();
    fakeSys.recvfromResult = fakeSys.recvfromData.size();
    socketHandler.handleReceivedMsg(*socketHandler.io, fakeSys.pipeFds[0],
                                    EPOLLIN);
    ASSERT_TRUE(success.handle.done());
    EXPECT_NE(responseMsg, nullptr);
    EXPECT_EQ(responseLen, 3u);
    EXPECT_EQ(success.handle.promise().data, 0u);
    EXPECT_EQ(socketManager.getSocket(9), -1);

    responseMsg = nullptr;
    responseLen = 0;
    fakeSys.sendtoResult = -1;
    auto failure = helper.queryBootStatus(9, responseMsg, responseLen);
    ASSERT_TRUE(failure.handle.done());
    EXPECT_EQ(responseMsg, nullptr);
    EXPECT_EQ(responseLen, 0u);
}

TEST_F(FWStatusMctpDiscoveryTest,
       HelperAndSocketHandlerCoverNoSocketAndDeactivateBranches)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    MCTPVdmHelper helper(bus, requester, socketHandler, ids);

    const mctp_vdm::Message* responseMsg = nullptr;
    size_t responseLen = 0;
    auto noSocket = helper.queryBootStatus(42, responseMsg, responseLen);
    EXPECT_EQ(responseMsg, nullptr);
    EXPECT_EQ(responseLen, 0u);

    const std::vector<uint8_t> sharedPath{1, 2, 3};
    socketHandler.socketInfoMap.emplace(
        sharedPath, std::tuple<std::unique_ptr<utils::CustomFD>,
                               std::unique_ptr<sdeventplus::source::IO>>{});
    socketHandler.eidToSockMap.emplace(
        9, std::make_tuple(SOCK_DGRAM, 0, sharedPath));
    socketHandler.eidToSockMap.emplace(
        10, std::make_tuple(SOCK_DGRAM, 0, sharedPath));
    socketManager.registerEndpoint(9, 11);
    socketManager.registerEndpoint(10, 11);

    socketHandler.deactivateSocket(9);
    EXPECT_EQ(socketHandler.socketInfoMap.size(), 1u);
    EXPECT_EQ(socketManager.getSocket(9), -1);
    EXPECT_EQ(socketManager.getSocket(10), 11);

    socketHandler.deactivateSocket(10);
    EXPECT_TRUE(socketHandler.socketInfoMap.empty());
    EXPECT_EQ(socketManager.getSocket(10), -1);

    socketHandler.deactivateSocket(77);

    noSocket.detach();
}

TEST_F(FWStatusMctpDiscoveryTest,
       DiscoveryConstructorCoversMapperAndSignalBranches)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    const auto validInterfaces =
        makeMctpInterfaces(9, "uuid-ctor", {1, 2, 3}, {mctp_vdm::MessageType},
                           "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");
    auto noVdmInterfaces = validInterfaces;
    noVdmInterfaces["xyz.openbmc_project.MCTP.Endpoint"]
                   ["SupportedMessageTypes"] = std::vector<uint8_t>{0x01};
    auto missingEndpointInterfaces = validInterfaces;
    missingEndpointInterfaces.erase("xyz.openbmc_project.MCTP.Endpoint");

    fakeBusCalls.getSubTreeResponse = {
        {"/xyz/openbmc_project/mctp",
         {{"svc.good", {"iface"}}, {"svc.fail", {"iface"}}}}};
    dbus::ObjectValueTree managedObjects;
    managedObjects.emplace(
        sdbusplus::message::object_path("/xyz/openbmc_project/mctp/device0"),
        validInterfaces);
    managedObjects.emplace(
        sdbusplus::message::object_path("/xyz/openbmc_project/mctp/device1"),
        noVdmInterfaces);
    managedObjects.emplace(
        sdbusplus::message::object_path("/xyz/openbmc_project/mctp/device2"),
        missingEndpointInterfaces);
    fakeBusCalls.managedObjectResponses.push_back(std::move(managedObjects));
    fakeBusCalls.managedObjectResponses.push_back(std::nullopt);

    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture, nullptr});
    ASSERT_GE(capture.callCount, 1);
    EXPECT_LE(capture.infos.size(), 1u);

    auto signal =
        bus.new_signal("/xyz/openbmc_project/mctp",
                       "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
    signal.append(
        sdbusplus::message::object_path("/xyz/openbmc_project/mctp/device3"),
        validInterfaces);
    (void)sd_bus_message_seal(signal.get(), 0, 0);
    sd_bus_message_rewind(signal.get(), true);
    discovery.discoverEndpoints(signal);
    EXPECT_GE(capture.callCount, 2);
    ASSERT_EQ(capture.infos.size(), 1u);
    EXPECT_EQ(std::get<0>(capture.infos.front()), 9);
    EXPECT_EQ(socketHandler.eidToSockMap.count(9), 1u);
}

TEST_F(FWStatusMctpDiscoveryTest, DiscoveryConstructorHandlesMapperFailure)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    fakeBusCalls.failGetSubTree = true;
    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture});
    EXPECT_EQ(capture.callCount, 1);
    EXPECT_TRUE(capture.infos.empty());
    EXPECT_TRUE(socketHandler.eidToSockMap.empty());
}

TEST_F(FWStatusMctpDiscoveryTest,
       DiscoveryConstructorPassThroughAllFallbackDoesNotPopulateEndpoints)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    fakeBusCalls.passThroughAll = true;
    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture});
    ASSERT_EQ(capture.callCount, 1);
    EXPECT_TRUE(capture.infos.empty());
    EXPECT_TRUE(socketHandler.eidToSockMap.empty());
}

TEST_F(FWStatusMctpDiscoveryTest,
       PopulateMctpInfoCoversValidationAndFilteringBranches)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    fakeBusCalls.failGetSubTree = true;
    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture});

    const auto validInterfaces = makeMctpInterfaces(
        12, "uuid-validate", {1, 2, 3}, {mctp_vdm::MessageType},
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");

    mctp::Infos infos;

    auto missingUuid = validInterfaces;
    missingUuid.erase(mctp::UUIDInterface);
    discovery.populateMctpInfo(missingUuid, infos);
    EXPECT_TRUE(infos.empty());

    auto missingUuidProperty = validInterfaces;
    missingUuidProperty[mctp::UUIDInterface].erase("UUID");
    discovery.populateMctpInfo(missingUuidProperty, infos);
    EXPECT_TRUE(infos.empty());

    auto badUuidType = validInterfaces;
    badUuidType[mctp::UUIDInterface]["UUID"] = static_cast<uint64_t>(1);
    discovery.populateMctpInfo(badUuidType, infos);
    EXPECT_TRUE(infos.empty());

    auto missingUnixSocket = validInterfaces;
    missingUnixSocket.erase(
        std::string(mctp_vdm::MctpDiscovery::unixSocketIntfName));
    discovery.populateMctpInfo(missingUnixSocket, infos);
    EXPECT_TRUE(infos.empty());

    auto missingAddress = validInterfaces;
    missingAddress[std::string(mctp_vdm::MctpDiscovery::unixSocketIntfName)]
                  ["Address"] = std::vector<uint8_t>{};
    discovery.populateMctpInfo(missingAddress, infos);
    EXPECT_TRUE(infos.empty());

    auto missingType = validInterfaces;
    missingType[std::string(mctp_vdm::MctpDiscovery::unixSocketIntfName)]
               ["Type"] = static_cast<uint8_t>(0);
    discovery.populateMctpInfo(missingType, infos);
    EXPECT_TRUE(infos.empty());

    auto missingTypeProperty = validInterfaces;
    missingTypeProperty[std::string(
                            mctp_vdm::MctpDiscovery::unixSocketIntfName)]
        .erase("Type");
    discovery.populateMctpInfo(missingTypeProperty, infos);
    EXPECT_TRUE(infos.empty());

    auto badTypeProperty = validInterfaces;
    badTypeProperty[std::string(mctp_vdm::MctpDiscovery::unixSocketIntfName)]
                   ["Type"] = std::string("bad-type");
    discovery.populateMctpInfo(badTypeProperty, infos);
    EXPECT_TRUE(infos.empty());

    auto missingProtocolProperty = validInterfaces;
    missingProtocolProperty[std::string(
                                mctp_vdm::MctpDiscovery::unixSocketIntfName)]
        .erase("Protocol");
    discovery.populateMctpInfo(missingProtocolProperty, infos);
    EXPECT_TRUE(infos.empty());

    auto badProtocolProperty = validInterfaces;
    badProtocolProperty[std::string(
        mctp_vdm::MctpDiscovery::unixSocketIntfName)]["Protocol"] =
        std::string("bad-protocol");
    discovery.populateMctpInfo(badProtocolProperty, infos);
    EXPECT_TRUE(infos.empty());

    auto missingAddressProperty = validInterfaces;
    missingAddressProperty[std::string(
                               mctp_vdm::MctpDiscovery::unixSocketIntfName)]
        .erase("Address");
    discovery.populateMctpInfo(missingAddressProperty, infos);
    EXPECT_TRUE(infos.empty());

    auto missingEndpoint = validInterfaces;
    missingEndpoint.erase("xyz.openbmc_project.MCTP.Endpoint");
    discovery.populateMctpInfo(missingEndpoint, infos);
    EXPECT_TRUE(infos.empty());

    auto missingEid = validInterfaces;
    missingEid["xyz.openbmc_project.MCTP.Endpoint"].erase("EID");
    discovery.populateMctpInfo(missingEid, infos);
    EXPECT_TRUE(infos.empty());

    auto missingTypes = validInterfaces;
    missingTypes["xyz.openbmc_project.MCTP.Endpoint"].erase(
        "SupportedMessageTypes");
    discovery.populateMctpInfo(missingTypes, infos);
    EXPECT_TRUE(infos.empty());

    auto missingMedium = validInterfaces;
    missingMedium["xyz.openbmc_project.MCTP.Endpoint"].erase("MediumType");
    discovery.populateMctpInfo(missingMedium, infos);
    EXPECT_TRUE(infos.empty());

    auto missingNetworkId = validInterfaces;
    missingNetworkId["xyz.openbmc_project.MCTP.Endpoint"].erase("NetworkId");
    discovery.populateMctpInfo(missingNetworkId, infos);
    EXPECT_TRUE(infos.empty());

    auto noVdmType = validInterfaces;
    noVdmType["xyz.openbmc_project.MCTP.Endpoint"]["SupportedMessageTypes"] =
        std::vector<uint8_t>{0x01};
    discovery.populateMctpInfo(noVdmType, infos);
    EXPECT_TRUE(infos.empty());

    auto badEidType = validInterfaces;
    badEidType["xyz.openbmc_project.MCTP.Endpoint"]["EID"] =
        std::string("bad-eid");
    discovery.populateMctpInfo(badEidType, infos);
    EXPECT_TRUE(infos.empty());

    auto badMessageTypes = validInterfaces;
    badMessageTypes["xyz.openbmc_project.MCTP.Endpoint"]
                   ["SupportedMessageTypes"] = std::string("bad-types");
    discovery.populateMctpInfo(badMessageTypes, infos);
    EXPECT_TRUE(infos.empty());

    auto badMediumType = validInterfaces;
    badMediumType["xyz.openbmc_project.MCTP.Endpoint"]["MediumType"] =
        std::vector<uint8_t>{0x01};
    discovery.populateMctpInfo(badMediumType, infos);
    EXPECT_TRUE(infos.empty());

    auto badAddressType = validInterfaces;
    badAddressType[std::string(mctp_vdm::MctpDiscovery::unixSocketIntfName)]
                  ["Address"] = std::string("bad-address");
    discovery.populateMctpInfo(badAddressType, infos);
    EXPECT_TRUE(infos.empty());

    auto badNetworkId = validInterfaces;
    badNetworkId["xyz.openbmc_project.MCTP.Endpoint"]["NetworkId"] =
        std::string("bad-net");
    discovery.populateMctpInfo(badNetworkId, infos);
    EXPECT_TRUE(infos.empty());

    discovery.populateMctpInfo(validInterfaces, infos);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(std::get<0>(infos.front()), 12);
    EXPECT_EQ(socketHandler.eidToSockMap.count(12), 1u);
}

TEST_F(FWStatusMctpDiscoveryTest,
       PopulateMctpInfoExhaustivelyCoversEndpointPropertyMasks)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    fakeBusCalls.failGetSubTree = true;
    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture});

    const auto validInterfaces =
        makeMctpInterfaces(18, "uuid-mask", {1, 2, 3}, {mctp_vdm::MessageType},
                           "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");

    for (int mask = 0; mask < 8; ++mask)
    {
        SCOPED_TRACE(::testing::Message() << "endpoint-mask=" << mask);

        auto interfaces = validInterfaces;
        auto& endpointProps = interfaces["xyz.openbmc_project.MCTP.Endpoint"];
        if ((mask & 0x1) == 0)
        {
            endpointProps.erase("EID");
        }
        if ((mask & 0x2) == 0)
        {
            endpointProps.erase("SupportedMessageTypes");
        }
        if ((mask & 0x4) == 0)
        {
            endpointProps.erase("MediumType");
        }

        mctp::Infos infos;
        discovery.populateMctpInfo(interfaces, infos);
        EXPECT_EQ(infos.size(), mask == 0x7 ? 1u : 0u);
    }

    EXPECT_EQ(socketHandler.eidToSockMap.count(18), 1u);
}

TEST_F(FWStatusMctpDiscoveryTest,
       PopulateMctpInfoCatchesMalformedRequiredProperties)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    fakeBusCalls.failGetSubTree = true;
    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture});

    const auto validInterfaces =
        makeMctpInterfaces(21, "uuid-alloc", {1, 2, 3}, {mctp_vdm::MessageType},
                           "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB");

    mctp::Infos baselineInfos;
    discovery.populateMctpInfo(validInterfaces, baselineInfos);
    ASSERT_EQ(baselineInfos.size(), 1u);

    auto missingNetworkId = validInterfaces;
    missingNetworkId["xyz.openbmc_project.MCTP.Endpoint"].erase("NetworkId");
    discovery.populateMctpInfo(missingNetworkId, baselineInfos);
    EXPECT_EQ(baselineInfos.size(), 1u);

    auto badUuidType = validInterfaces;
    badUuidType[mctp::UUIDInterface]["UUID"] = std::vector<uint8_t>{1, 2, 3};
    discovery.populateMctpInfo(badUuidType, baselineInfos);
    EXPECT_EQ(baselineInfos.size(), 1u);

    auto badSocketType = validInterfaces;
    badSocketType[std::string(mctp_vdm::MctpDiscovery::unixSocketIntfName)]
                 ["Type"] = std::string("bad-type");
    discovery.populateMctpInfo(badSocketType, baselineInfos);
    EXPECT_EQ(baselineInfos.size(), 1u);
}

TEST_F(FWStatusMctpDiscoveryTest,
       DiscoveryConstructorCoversEmptyMapperEntriesAndServiceDedup)
{
    auto bus = sdbusplus::bus::new_default();
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);
    DiscoveryCapture capture;

    const auto validInterfaces =
        makeMctpInterfaces(13, "uuid-dedup", {9, 8, 7}, {mctp_vdm::MessageType},
                           "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe");

    fakeBusCalls.getSubTreeResponse = {
        {"/xyz/openbmc_project/mctp/empty", {}},
        {"/xyz/openbmc_project/mctp/full",
         {{"svc.one", {"iface"}},
          {"svc.one", {"iface"}},
          {"svc.two", {"iface"}}}},
    };

    dbus::ObjectValueTree managedObjects;
    managedObjects.emplace(
        sdbusplus::message::object_path("/xyz/openbmc_project/mctp/device13"),
        validInterfaces);
    fakeBusCalls.managedObjectResponses.push_back(std::move(managedObjects));
    fakeBusCalls.failManagedObjects.insert("svc.two");

    mctp_vdm::MctpDiscovery discovery(bus, socketHandler, {&capture});
    ASSERT_EQ(capture.callCount, 1);
    EXPECT_LE(capture.infos.size(), 1u);
    EXPECT_LE(socketHandler.eidToSockMap.count(13), 1u);
}

TEST_F(FWStatusMctpDiscoveryTest,
       DiscoveryBinaryCoversRequestRetryTimerBranches)
{
    auto event = sdeventplus::Event::get_default();

    ZeroRetrySuccessTimer zeroRetry(event);
    EXPECT_EQ(zeroRetry.start(), 0);
    EXPECT_EQ(zeroRetry.sendCount, 1);
    EXPECT_EQ(event.run(std::chrono::milliseconds(1)), 0);

    StopBranchTimer stopTimer(event);
    fakeSys.setEnabledResult = -EIO;
    stopTimer.stop();
    fakeSys.setEnabledResult = 0;
}

TEST_F(FWStatusMctpDiscoveryTest,
       DiscoveryBinaryCoversHandlerAndAwaitableBranches)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    socketManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, socketManager,
                                         std::chrono::seconds(0), 0,
                                         std::chrono::milliseconds(1));

    std::optional<size_t> responseLength;
    auto request = makeRuntimeRequest(9, 1, 2, 3);
    EXPECT_EQ(handler.registerRequest(
                  9, 1, 2, 3, std::move(request),
                  [&](uint8_t, const mctp_vdm::Message* response, size_t len) {
                      responseLength = response == nullptr ? len : len + 100;
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    mctp_vdm::Message response{};
    response.hdr = {};
    response.hdr.request = 0;
    fakeSys.setEnabledResult = -EIO;
    handler.handleResponse(9, 1, 2, 3, &response, sizeof(response));
    ASSERT_TRUE(responseLength.has_value());
    EXPECT_EQ(*responseLength, sizeof(response) + 100);
    fakeSys.setEnabledResult = 0;

    handler.handleResponse(9, 31, 2, 7, &response, sizeof(response));

    auto expiringRequest = makeRuntimeRequest(9, 2, 2, 4);
    std::optional<size_t> expiredLen;
    EXPECT_EQ(handler.registerRequest(
                  9, 2, 2, 4, std::move(expiringRequest),
                  [&](uint8_t, const mctp_vdm::Message* resp, size_t len) {
                      if (resp == nullptr)
                      {
                          expiredLen = len;
                      }
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    auto key = mctp_vdm::requester::RequestKey{9, 2, 2, 4};
    auto& timer = std::get<2>(handler.handlers.at(key));
    timer->stop();
    timer->start(std::chrono::microseconds(1));
    fakeSys.setEnabledResult = -EIO;
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);
    ASSERT_TRUE(expiredLen.has_value());
    EXPECT_EQ(*expiredLen, 0u);
    fakeSys.setEnabledResult = 0;

    auto noSocketRequest = makeRuntimeRequest(8, 3, 2, 5);
    EXPECT_LT(handler.registerRequest(
                  8, 3, 2, 5, std::move(noSocketRequest),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              0);

    const mctp_vdm::Message* responsePtr = nullptr;
    size_t responseLen = 11;
    auto invalidRequest = makeRuntimeRequest(9, 4, 2, 6);
    mctp_vdm::requester::SendRecvMctpVdmMsg invalidAwaitable(
        handler, 9, invalidRequest, nullptr, nullptr);
    EXPECT_FALSE(invalidAwaitable.await_ready());
    EXPECT_FALSE(invalidAwaitable.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(invalidAwaitable.await_resume(),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrInvalidData));

    auto errorRequest = makeRuntimeRequest(9, 5, 2, 7);
    mctp_vdm::requester::SendRecvMctpVdmMsg errorAwaitable(
        handler, 9, errorRequest, &responsePtr, &responseLen);
    errorAwaitable.resumeHandle = std::noop_coroutine();
    errorAwaitable.HandleResponse(9, nullptr, 0);
    EXPECT_EQ(errorAwaitable.await_resume(),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral));
    EXPECT_EQ(responsePtr, nullptr);
    EXPECT_EQ(responseLen, 11u);
}

TEST_F(FWStatusMctpDiscoveryTest,
       DiscoveryBinaryCoversCoroutineFinalSuspendBranches)
{
    {
        auto done = completedCoroutine(7);
        EXPECT_TRUE(done.await_ready());
        EXPECT_EQ(done.await_resume(), 7);
    }

    auto detachedDone = completedCoroutine(8);
    detachedDone.detach();
    EXPECT_EQ(detachedDone.handle, nullptr);

    mctp_vdm::requester::Coroutine empty{};
    empty.detach();
    EXPECT_EQ(empty.handle, nullptr);

    auto suspended = suspendedCoroutine(9);
    auto rawHandle = suspended.handle;
    ASSERT_TRUE(rawHandle);
    EXPECT_FALSE(rawHandle.done());
    suspended.detach();
    EXPECT_EQ(suspended.handle, nullptr);
    rawHandle.resume();

    std::vector<uint8_t> trace;
    auto parent = parentCoroutine(trace);
    ASSERT_TRUE(parent.handle);
    EXPECT_TRUE(parent.handle.done());
    EXPECT_EQ(trace, (std::vector<uint8_t>{2, 1, 21}));
    EXPECT_EQ(parent.await_resume(), 22);
}

TEST_F(FWStatusMctpDiscoveryTest,
       DiscoveryBinaryCoversSocketHandlerRuntimeBranches)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager socketManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, socketManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler socketHandler(event, requester, socketManager);

    socketHandler.registerMctpEndpoint(9, SOCK_DGRAM, 0, {1, 2, 3});
    socketHandler.registerMctpEndpoint(10, SOCK_DGRAM, 0, {1, 2, 3});
    EXPECT_EQ(socketHandler.activateSockets({9, 10}), 0);
    ASSERT_NE(socketHandler.io, nullptr);

    std::optional<size_t> responseLength;
    auto request = makeRuntimeRequest(9, 3, 2, 5);
    EXPECT_EQ(
        requester.registerRequest(9, 3, 2, 5, std::move(request),
                                  [&](uint8_t, const mctp_vdm::Message*,
                                      size_t len) { responseLength = len; }),
        static_cast<int>(mctp_vdm::CompletionCodes::Success));

    std::vector<uint8_t> responseBytes(sizeof(mctp_vdm::Message));
    auto* msg = reinterpret_cast<mctp_vdm::Message*>(responseBytes.data());
    msg->hdr = {};
    msg->hdr.request = 0;
    msg->hdr.instanceId = 3;
    msg->hdr.msgType = 2;
    msg->hdr.commandCode = 5;

    fakeSys.recvPeekResult = responseBytes.size();
    fakeSys.recvfromData = responseBytes;
    fakeSys.recvfromResult = responseBytes.size();
    socketHandler.handleReceivedMsg(*socketHandler.io, fakeSys.pipeFds[0],
                                    EPOLLIN);
    ASSERT_TRUE(responseLength.has_value());
    EXPECT_EQ(*responseLength,
              sizeof(mctp_vdm::Message) - sizeof(mctp_vdm::MsgHeader));

    socketHandler.handleReceivedMsg(*socketHandler.io, fakeSys.pipeFds[0], 0);

    fakeSys.recvPeekResult = -1;
    socketHandler.handleReceivedMsg(*socketHandler.io, fakeSys.pipeFds[0],
                                    EPOLLIN);

    fakeSys.recvPeekResult = responseBytes.size();
    fakeSys.recvfromResult = responseBytes.size() - 1;
    socketHandler.handleReceivedMsg(*socketHandler.io, fakeSys.pipeFds[0],
                                    EPOLLIN);

    fakeSys.recvfromResult = responseBytes.size();
    fakeSys.recvfromType = mctp_vdm::MessageType + 1;
    socketHandler.handleReceivedMsg(*socketHandler.io, fakeSys.pipeFds[0],
                                    EPOLLIN);

    fakeSys.recvfromType = mctp_vdm::MessageType;
    msg->hdr.request = 1;
    socketHandler.handleReceivedMsg(*socketHandler.io, fakeSys.pipeFds[0],
                                    EPOLLIN);

    mctp_socket::Handler failingSocketHandler(event, requester, socketManager);
    fakeSys.socketResult = -1;
    EXPECT_LT(failingSocketHandler.initSocket(SOCK_DGRAM, 0, {9, 8, 7}), 0);

    fakeSys.socketResult = dup(fakeSys.pipeFds[0]);
    fakeSys.getsockoptResult = -1;
    EXPECT_LT(failingSocketHandler.initSocket(SOCK_DGRAM, 0, {9, 8, 7}), 0);

    fakeSys.socketResult = dup(fakeSys.pipeFds[0]);
    fakeSys.getsockoptResult = 0;
    fakeSys.bindResult = -1;
    EXPECT_LT(failingSocketHandler.initSocket(SOCK_DGRAM, 0, {9, 8, 7}), 0);
}
