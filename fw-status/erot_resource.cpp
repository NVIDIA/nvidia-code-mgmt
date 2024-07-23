#include "erot_resource.hpp"

mctp_vdm::requester::Coroutine ERoTResource::updateBootStatusAsync()
{
    if (MCTPDiscoveryResource::isDeviceEnumerated() and MCTPDiscoveryResource::checkForEnabledMCTPEids())
    {
        const mctp_vdm::Message* responseMsg = nullptr;
        size_t responseLen = 0;
        auto eid = fetchEid();
        co_await mctpVdmHelper->queryBootStatus(eid, responseMsg, responseLen);

        std::vector<uint8_t> status(responseMsg->payload + 1, responseMsg->payload + responseLen);
        bootStatus->bootStatus(status);
    }
    else
    {
        bootStatus->bootStatus({0});
    }
}

std::vector<uint8_t> ERoTResource::getBootStatus() const noexcept
{
    return bootStatus->bootStatus();
}
