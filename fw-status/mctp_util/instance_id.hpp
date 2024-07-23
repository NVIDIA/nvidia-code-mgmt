#pragma once

#include "xyz/openbmc_project/Common/error.hpp"

#include <bitset>
#include <map>
#include <stdexcept>

namespace mctp_vdm
{

constexpr size_t maxInstanceIds = 32;

/** @class InstanceId
 *
 *  @brief Implementation of MCTP VDM instance id
 */
class InstanceId
{
  public:
    /** @brief Get next unused instance id
     *
     *  @return - MCTP VDM instance id
     */
    uint8_t next()
    {
        uint8_t idx = 0;
        while (idx < id.size() && id.test(idx))
        {
            ++idx;
        }

        if (idx == id.size())
        {
            throw std::runtime_error("No free instance ids");
        }

        id.set(idx);
        return idx;
    }

    /** @brief Mark an instance id as unused
     *  @param[in] instanceId - MCTP VDM instance id to be freed
     */
    void markFree(uint8_t instanceId)
    {
        id.set(instanceId, false);
    }

  private:
    std::bitset<maxInstanceIds> id;
};

class InstanceIdMgr
{
  public:
    InstanceIdMgr() = default;
    InstanceIdMgr(const InstanceIdMgr&) = delete;
    InstanceIdMgr& operator=(const InstanceIdMgr&) = delete;
    InstanceIdMgr(InstanceIdMgr&&) = delete;
    InstanceIdMgr& operator=(InstanceIdMgr&&) = delete;
    ~InstanceIdMgr() = default;

    /** @brief Implementation */
    uint8_t getInstanceId(uint8_t eid)
    {
        if (ids.find(eid) == ids.end())
        {
            InstanceId id;
            ids.emplace(eid, InstanceId());
        }

        uint8_t id{};
        try
        {
            id = ids[eid].next();
        }
        catch (const std::runtime_error& e)
        {
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                TooManyResources();
        }

        return id;
    }

    /** @brief Mark an instance id as unused
     *  @param[in] eid - MCTP eid to which this instance id belongs
     *  @param[in] instanceId - MCTP VDM instance id to be freed
     *  @note will throw std::out_of_range if instanceId > 31
     */
    void markFree(uint8_t eid, uint8_t instanceId)
    {
        ids[eid].markFree(instanceId);
    }

  private:
    /** @brief EID to MCTP VDM Instance ID map */
    std::map<uint8_t, InstanceId> ids;
};

} // namespace mctp_vdm