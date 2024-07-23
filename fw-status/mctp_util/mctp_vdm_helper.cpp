#include "mctp_vdm_helper.hpp"

#include "mctp_vdm_completion_codes.hpp"
#include "types.hpp"
#include "utils.hpp"

#include <xyz/openbmc_project/Logging/Entry/server.hpp>

using namespace mctp_vdm;

MCTPVdmHelper::MCTPVdmHelper(
    sdbusplus::bus::bus& bus,
    mctp_vdm::requester::Handler<mctp_vdm::requester::Request>& reqHandler,
    mctp_socket::Handler& sockHandler, mctp_vdm::InstanceIdMgr& instanceIdMgr) :
    bus(bus),
    reqHandler(reqHandler), sockHandler(sockHandler),
    instanceIdMgr(instanceIdMgr)
{}

mctp_vdm::requester::Coroutine
    MCTPVdmHelper::queryBootStatus(uint8_t eid, const mctp_vdm::Message*& responseMsg,
            size_t& responseLen)
{
    // Initialize MCTP sockets for the list of endpoints
    auto ret = sockHandler.activateSockets({eid});
    if (ret < 0)
    {
        lg2::error("Activating MCTP demux daemon sockets failed. ret={RET}", "RET",
                   unsigned(ret));
        co_return ret;
    }

    auto rc = co_await queryBootStatusImpl(eid, responseMsg, responseLen);
    if (rc != 0)
    {
        lg2::error(
            "Fetching QueryBootStatus on ERoT failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
    }

    co_return rc;
}

mctp_vdm::requester::Coroutine
    MCTPVdmHelper::queryBootStatusImpl(uint8_t eid, const mctp_vdm::Message*& responseMsg,
            size_t& responseLen)
{
    mctp::Request request(sizeof(mctp_vdm::MsgHeader));
    auto requestMsg = reinterpret_cast<mctp_vdm::MsgHeader*>(request.data());
    requestMsg->iana = htobe32(nvidiaIANA);
    requestMsg->request = 1;
    requestMsg->instanceId = instanceIdMgr.getInstanceId(eid);
    requestMsg->msgType = nvidiaMsgType;
    requestMsg->commandCode = 0x05;
    requestMsg->msgVersion = nvidiaMsgVersion;


    auto rc = co_await mctp_vdm::requester::SendRecvMctpVdmMsg<
        mctp_vdm::requester::Handler<mctp_vdm::requester::Request>>(
        reqHandler, eid, request, &responseMsg, &responseLen);
    if (rc)
    {
        co_return rc;
    }

    co_return responseMsg->payload[0];
}

void MCTPVdmHelper::handleMctpEndpoints([[maybe_unused]] const mctp::Infos& mctpInfos)
{
    return;
}