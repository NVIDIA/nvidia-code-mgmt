#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#define private public
#define protected public
#include "../fw-status/mctp_util/socket_handler.cpp"
#undef protected
#undef private

namespace
{

struct SyscallState
{
    int pipeFds[2] = {-1, -1};
    ssize_t sendtoResult = 0;
    std::deque<ssize_t> sendtoResults{};
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
}

mctp::Request makeRequest(uint8_t eid, uint8_t instanceId, uint8_t type,
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

class FWStatusMctpRuntimeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        resetSyscalls();
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

class RetryEnabledTimer : public mctp_vdm::requester::RequestRetryTimer
{
  public:
    explicit RetryEnabledTimer(sdeventplus::Event& event, uint8_t retries = 1,
                               int sendResult = 0) :
        RequestRetryTimer(event, retries, std::chrono::milliseconds(1)),
        sendResult(sendResult)
    {}

    int sendCount = 0;
    int sendResult = 0;

  private:
    int send() const override
    {
        ++const_cast<RetryEnabledTimer*>(this)->sendCount;
        return sendResult;
    }
};

} // namespace

extern "C" ssize_t __wrap_sendto(int, const void*, size_t len, int,
                                 const struct sockaddr*, socklen_t)
{
    if (!fakeSys.sendtoResults.empty())
    {
        auto result = fakeSys.sendtoResults.front();
        fakeSys.sendtoResults.pop_front();
        if (result < 0)
        {
            errno = EIO;
            return -1;
        }
        return result ? result : static_cast<ssize_t>(len);
    }

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

extern "C" ssize_t __wrap_recv(int, void*, size_t, int)
{
    if (fakeSys.recvPeekResult >= 0)
    {
        return fakeSys.recvPeekResult;
    }
    errno = EAGAIN;
    return -1;
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

TEST_F(FWStatusMctpRuntimeTest, InstanceIdManagerExhaustsAndReusesIds)
{
    mctp_vdm::InstanceIdMgr ids;
    std::vector<uint8_t> allocated;
    for (size_t i = 0; i < mctp_vdm::maxInstanceIds; ++i)
    {
        allocated.push_back(ids.getInstanceId(7));
    }
    EXPECT_THROW(ids.getInstanceId(7), sdbusplus::exception_t);
    ids.markFree(7, allocated[3]);
    EXPECT_EQ(ids.getInstanceId(7), allocated[3]);
}

TEST_F(FWStatusMctpRuntimeTest, InstanceIdDirectlyCoversLoopAndExceptionPaths)
{
    mctp_vdm::InstanceId id;

    EXPECT_EQ(id.next(), 0);
    for (uint8_t idx = 1; idx < mctp_vdm::maxInstanceIds; ++idx)
    {
        EXPECT_EQ(id.next(), idx);
    }

    EXPECT_THROW(id.next(), std::runtime_error);

    id.markFree(10);
    EXPECT_EQ(id.next(), 10);
}

TEST_F(FWStatusMctpRuntimeTest, InKernelRequestHandlesSendAndRetryBranches)
{
    auto event = sdeventplus::Event::get_default();
    auto request = makeRequest(9, 1, 2, 3);

    mctp_vdm::requester::InKernelRequest sendOk(
        42, 9, event, std::move(request), 1, std::chrono::milliseconds(1));
    EXPECT_EQ(sendOk.start(), 0);
    sendOk.callback();
    sendOk.callback();
    sendOk.stop();

    auto failedRequest = makeRequest(9, 2, 2, 3);
    fakeSys.sendtoResult = -1;
    mctp_vdm::requester::InKernelRequest sendFail(42, 9, event,
                                                  std::move(failedRequest), 0,
                                                  std::chrono::milliseconds(1));
    EXPECT_LT(sendFail.start(), 0);
}

TEST_F(FWStatusMctpRuntimeTest,
       RequestRetryTimerStartWithoutRetriesSkipsTimerArmingBranch)
{
    auto event = sdeventplus::Event::get_default();
    ZeroRetrySuccessTimer timer(event);

    EXPECT_EQ(timer.start(), 0);
    EXPECT_EQ(timer.sendCount, 1);
    EXPECT_EQ(event.run(std::chrono::milliseconds(1)), 0);
}

TEST_F(FWStatusMctpRuntimeTest, HandlerRegistersRunsAndExpiresRequests)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    std::optional<size_t> responseLength;
    std::optional<const mctp_vdm::Message*> responseSeen;
    auto request = makeRequest(9, 1, 2, 3);
    EXPECT_EQ(handler.registerRequest(
                  9, 1, 2, 3, std::move(request),
                  [&](uint8_t, const mctp_vdm::Message* response, size_t len) {
                      responseSeen = response;
                      responseLength = len;
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));
    ASSERT_TRUE(
        handler.handlers.contains(mctp_vdm::requester::RequestKey{9, 1, 2, 3}));

    mctp_vdm::Message response{};
    response.hdr = {};
    response.hdr.request = 0;
    handler.handleResponse(9, 1, 2, 3, &response, sizeof(response));
    ASSERT_TRUE(responseLength.has_value());
    ASSERT_TRUE(responseSeen.has_value());
    EXPECT_EQ(*responseSeen, &response);
    EXPECT_EQ(*responseLength, sizeof(response));
    EXPECT_FALSE(
        handler.handlers.contains(mctp_vdm::requester::RequestKey{9, 1, 2, 3}));

    responseLength.reset();
    responseSeen.reset();
    auto expiringRequest = makeRequest(9, 2, 2, 4);
    EXPECT_EQ(handler.registerRequest(
                  9, 2, 2, 4, std::move(expiringRequest),
                  [&](uint8_t, const mctp_vdm::Message* resp, size_t len) {
                      responseSeen = resp;
                      responseLength = resp == nullptr ? len : len + 100;
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    const auto key = mctp_vdm::requester::RequestKey{9, 2, 2, 4};
    handler.removeRequestContainer.emplace(
        key, std::make_unique<sdeventplus::source::Defer>(
                 event, [](sdeventplus::source::EventBase&) {}));
    handler.removeRequestEntry(key);
    ASSERT_TRUE(responseLength.has_value());
    ASSERT_TRUE(responseSeen.has_value());
    EXPECT_EQ(*responseSeen, nullptr);
    EXPECT_EQ(*responseLength, 0u);

    auto noSocketRequest = makeRequest(8, 1, 2, 3);
    EXPECT_LT(handler.registerRequest(
                  8, 1, 2, 3, std::move(noSocketRequest),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              0);
}

TEST_F(FWStatusMctpRuntimeTest,
       HandlerRejectsDuplicateRegistrationWithoutFreeingActiveInstanceId)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    auto instanceId = ids.getInstanceId(9);
    auto request = makeRequest(9, instanceId, 2, 3);
    ASSERT_EQ(handler.registerRequest(
                  9, instanceId, 2, 3, std::move(request),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    auto duplicateRequest = makeRequest(9, instanceId, 2, 3);
    EXPECT_EQ(handler.registerRequest(
                  9, instanceId, 2, 3, std::move(duplicateRequest),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral));
    EXPECT_EQ(ids.getInstanceId(9), instanceId + 1);
}

TEST_F(FWStatusMctpRuntimeTest,
       HandlerAdvancesQueueAfterQueuedRequestSendFailure)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    auto activeRequest = makeRequest(9, 1, 2, 3);
    ASSERT_EQ(handler.registerRequest(
                  9, 1, 2, 3, std::move(activeRequest),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    size_t queuedFailures = 0;
    auto queuedRequestA = makeRequest(9, 2, 2, 4);
    ASSERT_EQ(handler.registerRequest(
                  9, 2, 2, 4, std::move(queuedRequestA),
                  [&](uint8_t, const mctp_vdm::Message* response, size_t len) {
                      if (response == nullptr && len == 0)
                      {
                          ++queuedFailures;
                      }
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    auto queuedRequestB = makeRequest(9, 3, 2, 5);
    ASSERT_EQ(handler.registerRequest(
                  9, 3, 2, 5, std::move(queuedRequestB),
                  [&](uint8_t, const mctp_vdm::Message* response, size_t len) {
                      if (response == nullptr && len == 0)
                      {
                          ++queuedFailures;
                      }
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    fakeSys.sendtoResults = {-1, 0};

    mctp_vdm::Message response{};
    response.hdr = {};
    response.hdr.request = 0;
    handler.handleResponse(9, 1, 2, 3, &response, sizeof(response));

    EXPECT_EQ(queuedFailures, 1u);
    EXPECT_TRUE(fakeSys.sendtoResults.empty());
    ASSERT_EQ(handler.handlers.size(), 1u);
    EXPECT_TRUE(std::get<2>(handler.handlers.begin()->second)->isRunning());
}

TEST_F(FWStatusMctpRuntimeTest,
       HandlerCoversUnknownResponseAndTimerStopFailureBranches)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    bool callbackInvoked = false;
    auto request = makeRequest(9, 6, 2, 7);
    EXPECT_EQ(handler.registerRequest(
                  9, 6, 2, 7, std::move(request),
                  [&](uint8_t, const mctp_vdm::Message* response, size_t len) {
                      callbackInvoked = true;
                      EXPECT_NE(response, nullptr);
                      EXPECT_EQ(len, sizeof(mctp_vdm::Message));
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    fakeSys.setEnabledResult = -EIO;
    mctp_vdm::Message response{};
    response.hdr = {};
    response.hdr.request = 0;
    handler.handleResponse(9, 6, 2, 7, &response, sizeof(response));
    EXPECT_TRUE(callbackInvoked);

    fakeSys.setEnabledResult = 0;
    handler.handleResponse(9, 31, 2, 7, &response, sizeof(response));
}

TEST_F(FWStatusMctpRuntimeTest,
       HandlerExpiryCallbackCoversTimerStopFailureDuringTimeout)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(0), 0,
                                         std::chrono::milliseconds(1));

    std::optional<size_t> expiredLen;
    auto request = makeRequest(9, 7, 2, 8);
    EXPECT_EQ(handler.registerRequest(
                  9, 7, 2, 8, std::move(request),
                  [&](uint8_t, const mctp_vdm::Message* response, size_t len) {
                      if (response == nullptr)
                      {
                          expiredLen = len;
                      }
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    fakeSys.setEnabledResult = -EIO;
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);
    ASSERT_TRUE(expiredLen.has_value());
    EXPECT_EQ(*expiredLen, 0u);
    fakeSys.setEnabledResult = 0;
}

TEST_F(FWStatusMctpRuntimeTest, SocketHandlerCoversActivationRxAndDeactivation)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, sockManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler sockHandler(event, requester, sockManager);

    sockHandler.registerMctpEndpoint(9, SOCK_DGRAM, 0, {1, 2, 3});
    sockHandler.registerMctpEndpoint(10, SOCK_DGRAM, 0, {1, 2, 3});
    EXPECT_EQ(sockHandler.activateSockets({9, 10}), 0);
    EXPECT_EQ(sockManager.getSocket(9), fakeSys.pipeFds[0]);
    EXPECT_EQ(sockManager.getSocket(10), fakeSys.pipeFds[0]);
    ASSERT_NE(sockHandler.io, nullptr);

    std::optional<size_t> responseLength;
    auto request = makeRequest(9, 3, 2, 5);
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
    sockHandler.handleReceivedMsg(*sockHandler.io, fakeSys.pipeFds[0], EPOLLIN);
    ASSERT_TRUE(responseLength.has_value());
    EXPECT_EQ(*responseLength,
              sizeof(mctp_vdm::Message) - sizeof(mctp_vdm::MsgHeader));

    fakeSys.recvPeekResult = 0;
    sockHandler.handleReceivedMsg(*sockHandler.io, fakeSys.pipeFds[0], EPOLLIN);

    sockHandler.deactivateSocket(9);
    EXPECT_EQ(sockManager.getSocket(9), -1);
    EXPECT_EQ(sockManager.getSocket(10), fakeSys.pipeFds[0]);
    sockHandler.deactivateSocket(10);
    EXPECT_EQ(sockManager.getSocket(10), -1);
}

TEST_F(FWStatusMctpRuntimeTest, SocketHandlerCoversExistingSocketAndErrors)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, sockManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler sockHandler(event, requester, sockManager);

    sockHandler.registerMctpEndpoint(9, SOCK_DGRAM, 0, {1, 2, 3});
    sockHandler.socketInfoMap[{1, 2, 3}] = std::make_tuple(
        std::make_unique<utils::CustomFD>(dup(fakeSys.pipeFds[0])),
        std::make_unique<sdeventplus::source::IO>(
            event, fakeSys.pipeFds[0], EPOLLIN,
            std::bind_front(&mctp_socket::Handler::handleReceivedMsg,
                            &sockHandler)));
    EXPECT_EQ(sockHandler.activateSockets({9}), 0);
    EXPECT_EQ(sockManager.getSocket(9),
              (*std::get<0>(sockHandler.socketInfoMap[{1, 2, 3}]))());

    sockHandler.handleReceivedMsg(
        *std::get<1>(sockHandler.socketInfoMap[{1, 2, 3}]), fakeSys.pipeFds[0],
        0);

    fakeSys.recvPeekResult = -1;
    sockHandler.handleReceivedMsg(
        *std::get<1>(sockHandler.socketInfoMap[{1, 2, 3}]), fakeSys.pipeFds[0],
        EPOLLIN);

    std::vector<uint8_t> responseBytes(sizeof(mctp_vdm::Message));
    auto* msg = reinterpret_cast<mctp_vdm::Message*>(responseBytes.data());
    msg->hdr = {};
    msg->hdr.request = 0;

    fakeSys.recvPeekResult = responseBytes.size();
    fakeSys.recvfromData = responseBytes;
    fakeSys.recvfromResult = responseBytes.size() - 1;
    sockHandler.handleReceivedMsg(
        *std::get<1>(sockHandler.socketInfoMap[{1, 2, 3}]), fakeSys.pipeFds[0],
        EPOLLIN);

    fakeSys.recvfromResult = responseBytes.size();
    fakeSys.recvfromType = mctp_vdm::MessageType + 1;
    sockHandler.handleReceivedMsg(
        *std::get<1>(sockHandler.socketInfoMap[{1, 2, 3}]), fakeSys.pipeFds[0],
        EPOLLIN);

    fakeSys.recvfromType = mctp_vdm::MessageType;
    msg->hdr.request = 1;
    sockHandler.handleReceivedMsg(
        *std::get<1>(sockHandler.socketInfoMap[{1, 2, 3}]), fakeSys.pipeFds[0],
        EPOLLIN);

    sockHandler.deactivateSockets();
    EXPECT_EQ(sockManager.getSocket(9), -1);
}

TEST_F(FWStatusMctpRuntimeTest, SocketInitializationCoversFailurePaths)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, sockManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler sockHandler(event, requester, sockManager);

    fakeSys.socketResult = -1;
    EXPECT_LT(sockHandler.initSocket(SOCK_DGRAM, 0, {1, 2, 3}), 0);

    fakeSys.socketResult = dup(fakeSys.pipeFds[0]);
    fakeSys.getsockoptResult = -1;
    EXPECT_LT(sockHandler.initSocket(SOCK_DGRAM, 0, {1, 2, 3}), 0);

    fakeSys.socketResult = dup(fakeSys.pipeFds[0]);
    fakeSys.getsockoptResult = 0;
    fakeSys.bindResult = -1;
    EXPECT_LT(sockHandler.initSocket(SOCK_DGRAM, 0, {1, 2, 3}), 0);
}

TEST_F(FWStatusMctpRuntimeTest, UtilityHeadersCoverCustomFdAndEndpointHelpers)
{
    int duplicated = dup(fakeSys.pipeFds[0]);
    ASSERT_GE(duplicated, 0);
    {
        utils::CustomFD fd(duplicated);
        EXPECT_EQ(fd(), duplicated);
    }

    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(7, 17);
    sockManager.registerEndpoint(8, 18);
    std::vector<uint8_t> active(sockManager.getActiveEndpoints().begin(),
                                sockManager.getActiveEndpoints().end());
    EXPECT_EQ(active.size(), 2u);
}

TEST_F(FWStatusMctpRuntimeTest,
       SocketHandlerHelperBranchesCoverUnknownAndSharedPaths)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler requester(event, ids, sockManager,
                                           std::chrono::seconds(1), 0,
                                           std::chrono::milliseconds(1));
    mctp_socket::Handler sockHandler(event, requester, sockManager);

    sockHandler.deactivateSocket(99);

    sockHandler.registerMctpEndpoint(9, SOCK_DGRAM, 0, {1, 2, 3});
    sockHandler.registerMctpEndpoint(10, SOCK_DGRAM, 0, {1, 2, 3});
    sockManager.registerEndpoint(9, fakeSys.pipeFds[0]);
    sockManager.registerEndpoint(10, fakeSys.pipeFds[0]);
    EXPECT_FALSE(sockHandler.checkActiveEndpoints(9));
    sockManager.clearMctpEndpoint(10);
    EXPECT_TRUE(sockHandler.checkActiveEndpoints(9));

    fakeSys.socketResult = -1;
    EXPECT_EQ(sockHandler.activateSockets({9}), 0);
    EXPECT_EQ(sockManager.getSocket(9), fakeSys.pipeFds[0]);
}

TEST_F(FWStatusMctpRuntimeTest, HandlerExpiryAwaitableAndCoroutineBranches)
{
    const auto keyA = mctp_vdm::requester::RequestKey{9, 1, 2, 3};
    const auto keyB = mctp_vdm::requester::RequestKey{9, 1, 2, 3};
    const auto keyC = mctp_vdm::requester::RequestKey{9, 2, 2, 3};
    EXPECT_TRUE(keyA == keyB);
    EXPECT_FALSE(keyA == keyC);
    EXPECT_EQ(mctp_vdm::requester::RequestKeyHasher{}(keyA),
              mctp_vdm::requester::RequestKeyHasher{}(keyB));

    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    sockManager.registerEndpoint(10, 43);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    std::optional<size_t> expiredLen;
    std::optional<const mctp_vdm::Message*> expiredResp;
    auto firstRequest = makeRequest(9, 1, 2, 3);
    EXPECT_EQ(handler.registerRequest(
                  9, 1, 2, 3, std::move(firstRequest),
                  [&](uint8_t, const mctp_vdm::Message* response, size_t len) {
                      expiredResp = response;
                      expiredLen = len;
                  }),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    auto queuedRequest = makeRequest(9, 2, 2, 4);
    EXPECT_EQ(handler.registerRequest(
                  9, 2, 2, 4, std::move(queuedRequest),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    auto otherEidRequest = makeRequest(10, 3, 2, 5);
    EXPECT_EQ(handler.registerRequest(
                  10, 3, 2, 5, std::move(otherEidRequest),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));
    EXPECT_EQ(handler.runRegisteredRequest(11),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    auto& timer = std::get<2>(
        handler.handlers.at(mctp_vdm::requester::RequestKey{9, 1, 2, 3}));
    timer->stop();
    timer->start(std::chrono::microseconds(1));
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);
    EXPECT_GT(event.run(std::chrono::milliseconds(1)), 0);
    ASSERT_TRUE(expiredLen.has_value());
    ASSERT_TRUE(expiredResp.has_value());
    EXPECT_EQ(*expiredResp, nullptr);
    EXPECT_EQ(*expiredLen, 0u);

    auto invalidRequest = makeRequest(9, 4, 2, 6);
    mctp_vdm::requester::SendRecvMctpVdmMsg invalidAwaitable(
        handler, 9, invalidRequest, nullptr, nullptr);
    EXPECT_FALSE(invalidAwaitable.await_ready());
    EXPECT_FALSE(invalidAwaitable.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(invalidAwaitable.await_resume(),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrInvalidData));

    const mctp_vdm::Message* responsePtr = nullptr;
    size_t responseLen = 0;
    auto responseRequest = makeRequest(9, 5, 2, 7);
    mctp_vdm::requester::SendRecvMctpVdmMsg errorAwaitable(
        handler, 9, responseRequest, &responsePtr, &responseLen);
    errorAwaitable.resumeHandle = std::noop_coroutine();
    errorAwaitable.HandleResponse(9, nullptr, 0);
    EXPECT_EQ(errorAwaitable.await_resume(),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral));
    EXPECT_EQ(responsePtr, nullptr);
    EXPECT_EQ(responseLen, 0u);

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
}

TEST_F(FWStatusMctpRuntimeTest, CoroutineDetachCoversDoneAndNullHandlePaths)
{
    auto completed = completedCoroutine(10);
    ASSERT_TRUE(completed.handle);
    EXPECT_TRUE(completed.handle.done());
    completed.detach();
    EXPECT_EQ(completed.handle, nullptr);

    mctp_vdm::requester::Coroutine empty{
        std::coroutine_handle<mctp_vdm::requester::Coroutine::promise_type>{}};
    empty.detach();
    EXPECT_EQ(empty.handle, nullptr);
}

TEST_F(FWStatusMctpRuntimeTest,
       NestedCoroutineAwaitCoversParentResumeFinalSuspendBranch)
{
    std::vector<uint8_t> trace;
    auto parent = parentCoroutine(trace);

    ASSERT_TRUE(parent.handle);
    EXPECT_TRUE(parent.handle.done());
    EXPECT_EQ(trace, (std::vector<uint8_t>{2, 1, 21}));
    EXPECT_EQ(parent.await_resume(), 22);
}

TEST_F(FWStatusMctpRuntimeTest, RuntimeHelpersCoverRemainingHandlerBranches)
{
    EXPECT_FALSE((mctp_vdm::requester::RequestKey{1, 1, 2, 3} ==
                  mctp_vdm::requester::RequestKey{2, 1, 2, 3}));
    EXPECT_FALSE((mctp_vdm::requester::RequestKey{1, 1, 2, 3} ==
                  mctp_vdm::requester::RequestKey{1, 9, 2, 3}));
    EXPECT_FALSE((mctp_vdm::requester::RequestKey{1, 1, 2, 3} ==
                  mctp_vdm::requester::RequestKey{1, 1, 4, 3}));
    EXPECT_FALSE((mctp_vdm::requester::RequestKey{1, 1, 2, 3} ==
                  mctp_vdm::requester::RequestKey{1, 1, 2, 5}));

    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    auto request = makeRequest(9, 1, 2, 3);
    ASSERT_EQ(handler.registerRequest(
                  9, 1, 2, 3, std::move(request),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));

    auto key = mctp_vdm::requester::RequestKey{9, 1, 2, 3};
    std::get<2>(handler.handlers.at(key))->stop();
    mctp_vdm::Message response{};
    response.hdr = {};
    handler.handleResponse(9, 1, 2, 3, &response, sizeof(response));

    handler.removeRequestEntry(key);
    EXPECT_FALSE(handler.handlers.contains(key));

    auto failingRequest = makeRequest(9, 2, 2, 4);
    fakeSys.sendtoResult = -1;
    EXPECT_LT(handler.registerRequest(
                  9, 2, 2, 4, std::move(failingRequest),
                  [](uint8_t, const mctp_vdm::Message*, size_t) {}),
              0);

    const mctp_vdm::Message* responsePtr = nullptr;
    auto lengthNullRequest = makeRequest(9, 3, 2, 5);
    mctp_vdm::requester::SendRecvMctpVdmMsg lengthNullAwaitable(
        handler, 9, lengthNullRequest, &responsePtr, nullptr);
    EXPECT_FALSE(lengthNullAwaitable.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(lengthNullAwaitable.await_resume(),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrInvalidData));

    size_t responseLen = 11;
    auto zeroLengthRequest = makeRequest(9, 4, 2, 6);
    mctp_vdm::requester::SendRecvMctpVdmMsg zeroLengthAwaitable(
        handler, 9, zeroLengthRequest, &responsePtr, &responseLen);
    zeroLengthAwaitable.resumeHandle = std::noop_coroutine();
    zeroLengthAwaitable.HandleResponse(9, &response, 0);
    EXPECT_EQ(zeroLengthAwaitable.await_resume(),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral));
    EXPECT_EQ(responsePtr, nullptr);
    EXPECT_EQ(responseLen, 11u);

    {
        auto suspended = suspendedCoroutine(12);
        ASSERT_TRUE(suspended.handle);
        EXPECT_FALSE(suspended.handle.done());
        suspended.handle.destroy();
        suspended.handle = nullptr;
    }
}

TEST_F(FWStatusMctpRuntimeTest,
       RequestRetryTimerAndAwaitableCoverStartFailureBranches)
{
    auto event = sdeventplus::Event::get_default();

    RetryEnabledTimer sendFailure(event, 1, -EIO);
    EXPECT_EQ(sendFailure.start(), -EIO);
    EXPECT_EQ(sendFailure.sendCount, 1);

    RetryEnabledTimer timerStartFailure(event);
    fakeSys.setEnabledResult = -EIO;
    EXPECT_EQ(timerStartFailure.start(), -1);
    EXPECT_EQ(timerStartFailure.sendCount, 1);
    fakeSys.setEnabledResult = 0;

    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    const mctp_vdm::Message* responsePtr = nullptr;
    size_t responseLen = 17;

    auto noSocketRequest = makeRequest(8, 1, 2, 3);
    mctp_vdm::requester::SendRecvMctpVdmMsg registerFailAwaitable(
        handler, 8, noSocketRequest, &responsePtr, &responseLen);
    EXPECT_FALSE(registerFailAwaitable.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(registerFailAwaitable.await_resume(), static_cast<uint8_t>(0xFF));

    mctp_vdm::Message response{};
    response.hdr = {};
    auto nullResponseRequest = makeRequest(9, 2, 2, 4);
    mctp_vdm::requester::SendRecvMctpVdmMsg nullResponseAwaitable(
        handler, 9, nullResponseRequest, &responsePtr, &responseLen);
    nullResponseAwaitable.resumeHandle = std::noop_coroutine();
    nullResponseAwaitable.HandleResponse(9, nullptr, sizeof(response));
    EXPECT_EQ(nullResponseAwaitable.await_resume(),
              static_cast<int>(mctp_vdm::CompletionCodes::ErrGeneral));
    EXPECT_EQ(responsePtr, nullptr);
    EXPECT_EQ(responseLen, 17u);
}

TEST_F(FWStatusMctpRuntimeTest,
       HandlerRunRegisteredRequestCoversQueuedSecondarySelectionBranch)
{
    auto event = sdeventplus::Event::get_default();
    mctp_socket::Manager sockManager;
    sockManager.registerEndpoint(9, 42);
    mctp_vdm::InstanceIdMgr ids;
    mctp_vdm::requester::Handler handler(event, ids, sockManager,
                                         std::chrono::seconds(1), 0,
                                         std::chrono::milliseconds(1));

    auto makeEntry = [&](uint8_t instanceId, uint8_t command) {
        return mctp_vdm::requester::Handler::RequestValue{
            std::make_unique<mctp_vdm::requester::InKernelRequest>(
                42, 9, event, makeRequest(9, instanceId, 2, command), 0,
                std::chrono::milliseconds(1)),
            [](uint8_t, const mctp_vdm::Message*, size_t) {},
            std::make_unique<sdbusplus::Timer>(event.get(), [] {})};
    };

    const auto firstKey = mctp_vdm::requester::RequestKey{9, 1, 2, 3};
    const auto secondKey = mctp_vdm::requester::RequestKey{9, 2, 2, 4};
    handler.handlers.emplace(firstKey, makeEntry(1, 3));
    handler.handlers.emplace(secondKey, makeEntry(2, 4));

    EXPECT_EQ(handler.runRegisteredRequest(9),
              static_cast<int>(mctp_vdm::CompletionCodes::Success));
    EXPECT_TRUE(std::get<2>(handler.handlers.at(firstKey))->isRunning() ||
                std::get<2>(handler.handlers.at(secondKey))->isRunning());
}

TEST_F(FWStatusMctpRuntimeTest, RequestRetryTimerStopCoversErrorBranch)
{
    auto event = sdeventplus::Event::get_default();
    StopBranchTimer timer(event);

    fakeSys.setEnabledResult = -EIO;
    timer.stop();
}
