#include "dashpavalidationorch.h"

#include "dashvnetorch.h"
#include "crmorch.h"

#include "taskworker.h"
#include "pbutils.h"
#include "saihelper.h"
#include "swssnet.h"

#include <cinttypes>
#include <boost/functional/hash.hpp>

extern sai_dash_pa_validation_api_t* sai_dash_pa_validation_api;
extern size_t gMaxBulkSize;
extern CrmOrch *gCrmOrch;
extern sai_object_id_t gSwitchId;

using namespace std;
using namespace swss;

size_t IpAddressHash::operator()(swss::IpAddress addr) const
{
    size_t seed = 0;
    const auto &inner = addr.getIp();
    boost::hash_combine(seed, inner.family);
    if (inner.family == AF_INET)
    {
        boost::hash_combine(seed, inner.ip_addr.ipv4_addr);
    }
    else if (inner.family == AF_INET6)
    {
        boost::hash_combine(seed, inner.ip_addr.ipv6_addr);
    }
    return seed;
}

DashPaValidationOrch::DashPaValidationOrch(swss::DBConnector *db, swss::ZmqServer *zmqServer):
    vnetOrch_(nullptr),
    ZmqOrch(db, {APP_DASH_PA_VALIDATION_TABLE_NAME}, zmqServer)
{
    SWSS_LOG_ENTER();
}

void DashPaValidationOrch::setVnetOrch(const DashVnetOrch *vnetOrch)
{
    vnetOrch_ = vnetOrch;
}

bool DashPaValidationOrch::parseVni(const std::string& key, uint32_t& vni) {
    std::istringstream iss(key);

    iss >> vni;

    return !iss.fail() && iss.eof();
}

bool DashPaValidationOrch::entryRefCountInc(const PaValidationEntry& entry)
{
    SWSS_LOG_ENTER();

    bool new_entry = false;

    auto vni = vni_map_.find(entry.vni);
    if (vni == vni_map_.end())
    {
        new_entry = true;
        vni_map_[entry.vni] = {{entry.address, 1}};
    } else {
        auto& vni_addresses = vni->second;
        auto address = vni_addresses.find(entry.address);
        if (address == vni_addresses.end())
        {
            new_entry = true;
            vni_addresses[entry.address] = 1;
        } else {
            vni_addresses[entry.address]++;
        }

        SWSS_LOG_DEBUG("PA Validation entry (%u, %s) refcount is increased to %" PRIu64, entry.vni, entry.address.to_string().c_str(), vni_addresses[entry.address]);
    }

    return new_entry;
}

bool DashPaValidationOrch::entryRefCountDec(const PaValidationEntry& entry)
{
    SWSS_LOG_ENTER();

    auto vni = vni_map_.find(entry.vni);
    if (vni == vni_map_.end())
    {
        SWSS_LOG_WARN("PA Validaion entry VNI %u IP %s is removed or not created yet", entry.vni, entry.address.to_string().c_str());
        return false;
    }

    auto& vni_addresses = vni->second;
    auto address = vni_addresses.find(entry.address);
    if (address == vni_addresses.end())
    {
        SWSS_LOG_WARN("PA Validaion entry VNI %u IP %s is removed or not created yet", entry.vni, entry.address.to_string().c_str());
        return false;
    }

    SWSS_LOG_DEBUG("PA Validation entry (%u, %s) refcount is decreased to %" PRIu64, entry.vni, entry.address.to_string().c_str(), address->second - 1);

    if (address->second == 1)
    {
        vni_addresses.erase(address);
        if (vni_addresses.empty())
        {
            vni_map_.erase(entry.vni);
        }

        return true;
    } else {
        address->second--;
        return false;
    }
}

bool DashPaValidationOrch::fetchVniAddresses(uint32_t vni, PaValidationEntryAddresses& addresses) const
{
    SWSS_LOG_ENTER();

    auto it = vni_map_.find(vni);
    if (it == vni_map_.end())
    {
        return true;
    }

    const auto& vni_addresses = it->second;

    addresses.reserve(vni_addresses.size());
    transform(vni_addresses.begin(), vni_addresses.end(), std::back_inserter(addresses),
               [](const auto& kv) { return kv.first; });

    return false;
}

void DashPaValidationOrch::paValidationConfigProcess(const PaValidationEntryList& toCreate, const PaValidationEntryList& toRemove)
{
    SWSS_LOG_ENTER();

    auto task_status = paValidationConfigApply(toCreate, toRemove);
    if (task_status != task_success)
    {
        parseHandleSaiStatusFailure(task_status);
    }
}

task_process_status DashPaValidationOrch::paValidationConfigApply(const PaValidationEntryList& toCreate, const PaValidationEntryList& toRemove)
{
    EntityBulker<sai_dash_pa_validation_api_t> bulker(sai_dash_pa_validation_api, gMaxBulkSize);
    std::vector<sai_status_t> statuses;

    SWSS_LOG_ENTER();

    statuses.reserve(toCreate.size() + toRemove.size());

    for (const auto& entry: toCreate)
    {
        bool new_entry = entryRefCountInc(entry);
        if (!new_entry)
        {
            // mark entry as created to omit error handling
            statuses.emplace_back(SAI_STATUS_SUCCESS);
        } else {
            sai_attribute_t attr;
            uint32_t attr_count = 1;

            sai_pa_validation_entry_t sai_entry;
            sai_entry.vnet_id = entry.vnet_oid;
            sai_entry.switch_id = gSwitchId;
            swss::copy(sai_entry.sip, entry.address);

            attr.id = SAI_PA_VALIDATION_ENTRY_ATTR_ACTION;
            attr.value.u32 = SAI_PA_VALIDATION_ENTRY_ACTION_PERMIT;

            statuses.emplace_back();
            bulker.create_entry(&statuses.back(), &sai_entry, attr_count, &attr);

            gCrmOrch->incCrmResUsedCounter(entry.address.isV4() ? CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION : CrmResourceType::CRM_DASH_IPV6_PA_VALIDATION);
        }
    }

    for (const auto& entry: toRemove)
    {
        bool last_ref = entryRefCountDec(entry);
        if (!last_ref)
        {
            // mark entry as removed to omit error handling
            statuses.emplace_back(SAI_STATUS_SUCCESS);
        } else {
            sai_pa_validation_entry_t sai_entry;
            sai_entry.vnet_id = entry.vnet_oid;
            sai_entry.switch_id = gSwitchId;
            swss::copy(sai_entry.sip, entry.address);

            statuses.emplace_back();
            bulker.remove_entry(&statuses.back(), &sai_entry);

            gCrmOrch->decCrmResUsedCounter(entry.address.isV4() ? CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION : CrmResourceType::CRM_DASH_IPV6_PA_VALIDATION);
        }
    }

    bulker.flush();

    size_t i = 0;
    for (; i < toCreate.size(); i++)
    {
        const auto& entry = toCreate[i];
        const auto& status = statuses[i];

        if (status == SAI_STATUS_SUCCESS)
        {
            continue;
        }

        SWSS_LOG_ERROR("Failed to create PA validation entry for VNI %u IP %s", entry.vni, entry.address.to_string().c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_PA_VALIDATION, status);
        if (handle_status != task_success)
        {
            return handle_status;
        }
    }

    for (;i < toRemove.size(); i++)
    {
        const auto& entry = toRemove[i];
        const auto& status = statuses[i];

        if (status == SAI_STATUS_SUCCESS)
        {
            continue;
        }

        SWSS_LOG_ERROR("Failed to remvoe PA validation entry for VNI %u IP %s", entry.vni, entry.address.to_string().c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_PA_VALIDATION, status);
        if (handle_status != task_success)
        {
            return handle_status;
        }
    }

    return task_success;
}

task_process_status DashPaValidationOrch::addPaValidationEntries(const PaValidationEntryList& entries)
{
    return paValidationConfigApply(entries, {});
}

task_process_status DashPaValidationOrch::removePaValidationEntries(const PaValidationEntryList& entries)
{
    return paValidationConfigApply({}, entries);
}

void DashPaValidationOrch::doTask(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    SWSS_LOG_INFO("Table name: %s", tn.c_str());

    if (tn == APP_DASH_PA_VALIDATION_TABLE_NAME)
    {
        doTaskPaValidation(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}

void DashPaValidationOrch::doTaskPaValidation(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    PaValidationEntryList toCreate, toRemove;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;
        string key = kfvKey(tuple);
        string op = kfvOp(tuple);

        uint32_t vni;
        if (!parseVni(key, vni))
        {
            SWSS_LOG_WARN("Failed to parse VNI from PA Validation key %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        sai_object_id_t vnet_oid;
        if (!vnetOrch_->getVnetByVni(vni, vnet_oid))
        {
            SWSS_LOG_INFO("VNET for VNI %u is not created yet or removed", vni);
            it++;
            continue;
        }

        if (op == SET_COMMAND)
        {
            dash::pa_validation::PaValidation pbEntry;
            if (!parsePbMessage(kfvFieldsValues(tuple), pbEntry))
            {
                SWSS_LOG_WARN("Failed to parse protobuff messaage for PA Validation (vni: %u)", vni);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            PaValidationEntryAddresses addresses;
            if (!toAddresses(pbEntry, addresses))
            {
                SWSS_LOG_WARN("Failed to parse PA Validation (vni: %u) addresses", vni);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            for (const auto addr : addresses)
            {
                toCreate.push_back(PaValidationEntry{vnet_oid, vni, addr});
            }

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            PaValidationEntryAddresses addresses;
            bool empty = fetchVniAddresses(vni, addresses);
            if (empty)
            {
                SWSS_LOG_WARN("PA validation entries for VNI %u are already removed or not created yet", vni);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            for (const auto addr : addresses)
            {
                toRemove.push_back(PaValidationEntry{vnet_oid, vni, addr});
            }

            it = consumer.m_toSync.erase(it);
        }
    }

    paValidationConfigProcess(toCreate, toRemove);
}

bool DashPaValidationOrch::toAddresses(const dash::pa_validation::PaValidation& entry, PaValidationEntryAddresses &addresses)
{
    addresses.reserve(entry.addresses().size());

    for (const auto& addr : entry.addresses())
    {
        addresses.push_back(IpAddress(to_swss(addr)));
    }

    return true;
}
