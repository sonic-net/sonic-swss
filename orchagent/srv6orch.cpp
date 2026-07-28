#include <array>
#include <iostream>
#include <sstream>
#include <inttypes.h>
#include <iterator>
#include <memory>

#include "routeorch.h"
#include "logger.h"
#include "srv6orch.h"
#include "sai_serialize.h"
#include "crmorch.h"
#include "subscriberstatetable.h"
#include "redisutility.h"
#include "flex_counter_manager.h"
#include "flow_counter_handler.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

#define ADJ_DELIMITER ','
#define OVERLAY_RIF_DEFAULT_MTU 9100
#define LOCATOR_DEFAULT_BLOCK_LEN "32"
#define LOCATOR_DEFAULT_NODE_LEN "16"
#define LOCATOR_DEFAULT_FUNC_LEN "16"
#define LOCATOR_DEFAULT_ARG_LEN "0"

#define SRV6_FLEX_COUNTER_UPDATE_TIMER 1
#define SRV6_STAT_COUNTER_POLLING_INTERVAL_MS 10000

extern sai_object_id_t gSwitchId;
extern sai_object_id_t  gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_srv6_api_t* sai_srv6_api;
extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_counter_api_t* sai_counter_api;

extern RouteOrch *gRouteOrch;
extern CrmOrch *gCrmOrch;
extern bool gTraditionalFlexCounter;

const map<string, sai_my_sid_entry_endpoint_behavior_t> end_behavior_map =
{
    {"end",                SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E},
    {"end.x",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X},
    {"end.t",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T},
    {"end.dx6",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6},
    {"end.dx4",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4},
    {"end.dt4",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4},
    {"end.dt6",            SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6},
    {"end.dt46",           SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46},
    {"end.b6.encaps",      SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS},
    {"end.b6.encaps.red",  SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS_RED},
    {"end.b6.insert",      SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT},
    {"end.b6.insert.red",  SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT_RED},
    {"udx6",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDX6},
    {"udx4",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDX4},
    {"udt6",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT6},
    {"udt4",               SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT4},
    {"udt46",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT46},
    {"un",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN},
    {"ua",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UA}
};

const map<string, sai_my_sid_entry_endpoint_behavior_flavor_t> end_flavor_map =
{
    {"end",                SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
    {"end.x",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
    {"end.t",              SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD},
    {"un",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE},
    {"ua",                 SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD}
};

const map<string, sai_srv6_sidlist_type_t> sidlist_type_map =
{
    {"insert",             SAI_SRV6_SIDLIST_TYPE_INSERT},
    {"insert.red",         SAI_SRV6_SIDLIST_TYPE_INSERT_RED},
    {"encaps",             SAI_SRV6_SIDLIST_TYPE_ENCAPS},
    {"encaps.red",         SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED}
};

static bool mySidDscpModeToSai(const string& mode, sai_tunnel_dscp_mode_t& sai_mode)
{
    if (mode == "uniform")
    {
        sai_mode = SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL;
        return true;
    }

    if (mode == "pipe")
    {
        sai_mode = SAI_TUNNEL_DSCP_MODE_PIPE_MODEL;
        return true;
    }

    return false;
}

Srv6Orch::Srv6Orch(DBConnector *cfgDb, DBConnector *applDb, const vector<TableConnector>& tables, SwitchOrch *switchOrch, VRFOrch *vrfOrch, NeighOrch *neighOrch):
    Orch(tables),
    m_vrfOrch(vrfOrch),
    m_switchOrch(switchOrch),
    m_neighOrch(neighOrch),
    m_sidTable(applDb, APP_SRV6_SID_LIST_TABLE_NAME),
    m_mysidTable(applDb, APP_SRV6_MY_SID_TABLE_NAME),
    m_piccontextTable(applDb, APP_PIC_CONTEXT_TABLE_NAME),
    m_mysidCfgTable(cfgDb, CFG_SRV6_MY_SID_TABLE_NAME),
    m_locatorCfgTable(cfgDb, CFG_SRV6_MY_LOCATOR_TABLE_NAME),
    m_counter_manager(SRV6_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, SRV6_STAT_COUNTER_POLLING_INTERVAL_MS, false)
{
    m_neighOrch->attach(this);
    createRetryCache(APP_SRV6_MY_SID_TABLE_NAME);
    createRetryCache(APP_PIC_CONTEXT_TABLE_NAME);

    initializeCounters();
}

Srv6Orch::~Srv6Orch()
{
    m_neighOrch->detach(this);
}

void Srv6Orch::initializeCounters()
{
    m_mysid_counters_supported = queryMySidCountersCapability();
    if (!m_mysid_counters_supported)
    {
        SWSS_LOG_INFO("SRv6 counters are not supported on this platform");
        return;
    }

    m_asic_db = make_shared<DBConnector>("ASIC_DB", 0);
    m_counter_db = make_shared<DBConnector>("COUNTERS_DB", 0);
    m_mysid_counters_table = make_unique<Table>(m_counter_db.get(), COUNTERS_SRV6_NAME_MAP);

    if (gTraditionalFlexCounter)
    {
        m_vid_to_rid_table = make_unique<Table>(m_asic_db.get(), "VIDTORID");
    }

    m_counter_update_timer = new SelectableTimer(timespec { .tv_sec = SRV6_FLEX_COUNTER_UPDATE_TIMER , .tv_nsec = 0 });
    auto et = new ExecutableTimer(m_counter_update_timer, this, "SRV6_FLEX_COUNTER_UPDATE_TIMER");
    Orch::addExecutor(et);
}

bool Srv6Orch::queryMySidCountersCapability() const
{
    sai_attr_capability_t capability;
    sai_status_t status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_MY_SID_ENTRY, SAI_MY_SID_ENTRY_ATTR_COUNTER_ID, &capability);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Could not query SRv6 MySID entry attribute SAI_MY_SID_ENTRY_ATTR_COUNTER_ID %d", status);
        return false;
    }

    return capability.set_implemented && capability.create_implemented;
}


bool Srv6Orch::getMySidCountersEnabled() const
{
    return m_mysid_counters_enabled;
}

bool Srv6Orch::getMySidCountersSupported() const
{
    return m_mysid_counters_supported;
}

IpAddress Srv6Orch::getMySidAddress(const sai_my_sid_entry_t& sai_entry) const
{
    ip_addr_t ip_addr = {};
    ip_addr.family = AF_INET6;
    memcpy(&ip_addr.ip_addr.ipv6_addr, sai_entry.sid, sizeof(ip_addr.ip_addr.ipv6_addr));

    return IpAddress(ip_addr);
}

string Srv6Orch::getMySidCounterKey(const sai_my_sid_entry_t& sai_entry) const
{
    auto mysid_addr = getMySidAddress(sai_entry).to_string();
    auto locator_cfg = getMySidEntryLocatorCfg(sai_entry);
    return getMySidPrefix(mysid_addr, locator_cfg);
}

bool Srv6Orch::addMySidCounter(const sai_my_sid_entry_t& sai_entry, sai_object_id_t& counter_oid)
{
    SWSS_LOG_ENTER();

    if (!FlowCounterHandler::createGenericCounter(counter_oid))
    {
        SWSS_LOG_ERROR("Failed to create SAI counter for SRv6 MySID entry");
        return false;
    }

    auto key = getMySidCounterKey(sai_entry);
    vector<FieldValueTuple> fvs = {
        {key, sai_serialize_object_id(counter_oid)}
    };

    m_mysid_counters_table->set("", fvs);

    auto was_empty = m_pending_counters.empty();
    m_pending_counters[counter_oid] = key;

    if (was_empty)
    {
        m_counter_update_timer->start();
    }

    return true;
}

bool Srv6Orch::removeMySidCounter(const sai_my_sid_entry_t& sai_entry, sai_object_id_t& counter_oid)
{
    SWSS_LOG_ENTER();

    if (counter_oid == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    auto status = sai_counter_api->remove_counter(counter_oid);
    if (status != SAI_STATUS_SUCCESS &&
        handleSaiRemoveStatus(SAI_API_COUNTER, status) != task_success)
    {
        return false;
    }

    auto key = getMySidCounterKey(sai_entry);

    m_mysid_counters_table->hdel("", key);

    auto was_pending = m_pending_counters.erase(counter_oid) == 1;
    if (!was_pending)
    {
        SWSS_LOG_INFO("Unregistering SRv6 counter for %s, oid %s", key.c_str(), sai_serialize_object_id(counter_oid).c_str());
        m_counter_manager.clearCounterIdList(counter_oid);
    }

    counter_oid = SAI_NULL_OBJECT_ID;
    return true;
}

void Srv6Orch::setMySidEntryCounter(const sai_my_sid_entry_t& sai_entry, sai_object_id_t counter_oid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_MY_SID_ENTRY_ATTR_COUNTER_ID;
    attr.value.oid = counter_oid;

    auto status = sai_srv6_api->set_my_sid_entry_attribute(&sai_entry, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set my_sid entry counter oid to %s, rc: %s", sai_serialize_object_id(counter_oid).c_str(), sai_serialize_status(status).c_str());
    }
}

void Srv6Orch::setCountersState(bool enable)
{
    SWSS_LOG_ENTER();

    if (!getMySidCountersSupported())
    {
        SWSS_LOG_WARN("Ignoring SRv6 counters state change as they are not supported on this platform");
        return;
    }

    if (enable == m_mysid_counters_enabled)
    {
        return;
    }

    SWSS_LOG_NOTICE("Setting SRv6 MySID counters state to %s", enable ? "enabled" : "disabled");

    for (auto& mysid : srv6_my_sid_table_)
    {
        const auto& sai_entry = mysid.second.entry;
        auto &counter_oid = mysid.second.counter;

        if (enable)
        {
            addMySidCounter(sai_entry, counter_oid);
            setMySidEntryCounter(sai_entry, counter_oid);
        } else {
            setMySidEntryCounter(sai_entry, SAI_NULL_OBJECT_ID);
            if (!removeMySidCounter(sai_entry, counter_oid))
            {
                SWSS_LOG_ERROR("Failed to remove counter while disabling MySID counters");
            }
        }
    }

    m_mysid_counters_enabled = enable;
}

void Srv6Orch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    string value;
    for (auto it = m_pending_counters.begin(); it != m_pending_counters.end();)
    {
        const auto oid = sai_serialize_object_id(it->first);
        if (!gTraditionalFlexCounter || m_vid_to_rid_table->hget("", oid, value))
        {
            SWSS_LOG_INFO("Registering SRv6 counter for %s, oid %s", it->second.c_str(), oid.c_str());

            unordered_set<string> counter_stats;
            FlowCounterHandler::getGenericCounterStatIdList(counter_stats);
            m_counter_manager.setCounterIdList(it->first, CounterType::SRV6, counter_stats);
            it = m_pending_counters.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (m_pending_counters.empty())
    {
        m_counter_update_timer->stop();
    }
}

MySidLocatorCfg Srv6Orch::getMySidEntryLocatorCfg(const sai_my_sid_entry_t& sai_entry) const
{
    return {
        sai_entry.locator_block_len,
        sai_entry.locator_node_len,
        sai_entry.function_len,
        sai_entry.args_len,
    };
}


string Srv6Orch::getMySidPrefix(const string& my_sid_addr, const MySidLocatorCfg& locator_cfg) const
{
    return my_sid_addr + "/" + to_string(locator_cfg.block_len + locator_cfg.node_len + locator_cfg.func_len);
}

bool Srv6Orch::getLocatorCfgFromDb(const string& locator, MySidLocatorCfg& cfg)
{
    vector<FieldValueTuple> fvs;
    auto exists = m_locatorCfgTable.get(locator, fvs);
    if (!exists)
    {
        SWSS_LOG_ERROR("Failed to get the SRv6 locator %s - not present in the CONFIG_DB", locator.c_str());
        return false;
    }

    auto blen = fvsGetValue(fvs, "block_len", true);
    auto nlen = fvsGetValue(fvs, "node_len", true);
    auto flen = fvsGetValue(fvs, "func_len", true);
    auto alen = fvsGetValue(fvs, "arg_len", true);

    cfg = {
        (uint8_t)stoi(blen.get_value_or(LOCATOR_DEFAULT_BLOCK_LEN)),
        (uint8_t)stoi(nlen.get_value_or(LOCATOR_DEFAULT_NODE_LEN)),
        (uint8_t)stoi(flen.get_value_or(LOCATOR_DEFAULT_FUNC_LEN)),
        (uint8_t)stoi(alen.get_value_or(LOCATOR_DEFAULT_ARG_LEN))
    };

    return true;
}

bool Srv6Orch::reverseLookupLocator(const vector<string>& candidates, const MySidLocatorCfg& locator_cfg, string& locator)
{
    for (const auto& candidate: candidates)
    {
        MySidLocatorCfg cfg;
        auto ok = getLocatorCfgFromDb(candidate, cfg);
        if (!ok) {
            continue;
        }

        if (locator_cfg == cfg)
        {
            SWSS_LOG_DEBUG("Found a locator %s matching the config", candidate.c_str());
            locator = candidate;
            return true;
        }
    }

    return false;
}

void Srv6Orch::addMySidCfgCacheEntry(const string& my_sid_key, const vector<FieldValueTuple>& fvs)
{
    auto key_list = tokenize(my_sid_key, '|');
    auto locator = key_list[0];
    auto my_sid_prefix = key_list[1];
    boost::optional<sai_tunnel_dscp_mode_t> dscp_mode = boost::none;

    auto cfg = fvsGetValue(fvs, "decap_dscp_mode", false);
    if (cfg)
    {
        sai_tunnel_dscp_mode_t dscp_mode_sai;
        if (!mySidDscpModeToSai(*cfg, dscp_mode_sai))
        {
            SWSS_LOG_ERROR("Invalid MySID %s DSCP mode: %s", my_sid_prefix.c_str(), cfg->c_str());
            return;
        }
        dscp_mode = dscp_mode_sai;
    }

    my_sid_dscp_cfg_cache_.insert({my_sid_prefix, {locator, dscp_mode}});
    SWSS_LOG_INFO("Saving MySID entry %s %s DSCP mode %s", locator.c_str(), my_sid_prefix.c_str(), cfg ? cfg->c_str() : "none");
}

void Srv6Orch::removeMySidCfgCacheEntry(const string& my_sid_key)
{
    auto key_list = tokenize(my_sid_key, '|');
    auto locator = key_list[0];
    auto my_sid_prefix = key_list[1];

    auto cfg_cache = my_sid_dscp_cfg_cache_.equal_range(my_sid_prefix);
    for (auto it = cfg_cache.first; it != cfg_cache.second; ++it)
    {
        if (it->second.first == locator)
        {
            my_sid_dscp_cfg_cache_.erase(it);
            break;
        }
    }
}

void Srv6Orch::mySidCfgCacheRefresh()
{
    SWSS_LOG_INFO("Refreshing SRv6 MySID configuration cache");

    vector<KeyOpFieldsValuesTuple> entries;
    m_mysidCfgTable.getContent(entries);

    for (const auto& entry : entries)
    {
        addMySidCfgCacheEntry(kfvKey(entry), kfvFieldsValues(entry));
    }
}

bool Srv6Orch::getMySidEntryDscpMode(const string& my_sid_addr, const MySidLocatorCfg& locator_cfg, boost::optional<sai_tunnel_dscp_mode_t>& dscp_mode)
{
    auto my_sid_prefix = getMySidPrefix(my_sid_addr, locator_cfg);

    auto cfg_cache = my_sid_dscp_cfg_cache_.equal_range(my_sid_prefix);
    if (cfg_cache.first == my_sid_dscp_cfg_cache_.end())
    {
        mySidCfgCacheRefresh();

        cfg_cache = my_sid_dscp_cfg_cache_.equal_range(my_sid_prefix);
        if (cfg_cache.first == my_sid_dscp_cfg_cache_.end())
        {
            SWSS_LOG_INFO("SRv6 MySID entry %s is not available in the CONFIG_DB", my_sid_prefix.c_str());
            return false;
        }
    }

    auto cache_start = cfg_cache.first;
    auto cache_end = cfg_cache.second;

    if (distance(cache_start, cache_end) == 1)
    {
        const Srv6MySidDscpCfgCacheVal& cache_val = cache_start->second;
        dscp_mode = cache_val.second;

        SWSS_LOG_INFO("Found decap DSCP mode for MySID addr %s locator %s in the cache", my_sid_prefix.c_str(), cache_val.first.c_str());
        return true;
    }

    // There are multiple mysid entries with the same address but different locators
    vector<string> locator_candidates;
    transform(cache_start, cache_end, back_inserter(locator_candidates),
               [](const auto& v) { return v.second.first; });

    string locator;
    auto found = reverseLookupLocator(locator_candidates, locator_cfg, locator);
    if (!found)
    {
        SWSS_LOG_ERROR("Cannot find a locator in the CONFIG DB for MySID Entry %s", my_sid_prefix.c_str());
        return false;
    }

    for (auto it = cache_start; it != cache_end; ++it)
    {
        const Srv6MySidDscpCfgCacheVal& cache_val = it->second;
        if (cache_val.first == locator)
        {
            SWSS_LOG_INFO("Found decap DSCP mode for MySID addr %s locator %s after locator reverse lookup", my_sid_prefix.c_str(), locator.c_str());
            dscp_mode = cache_val.second;
            return true;
        }
    }

    return false;
}

bool Srv6Orch::initIpInIpTunnel(MySidIpInIpTunnel& tunnel, sai_tunnel_dscp_mode_t dscp_mode)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> overlay_intf_attrs;
    sai_attribute_t attr;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = gVirtualRouterId;
    overlay_intf_attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    overlay_intf_attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    attr.value.u32 = OVERLAY_RIF_DEFAULT_MTU;
    overlay_intf_attrs.push_back(attr);

    auto status = sai_router_intfs_api->create_router_interface(&tunnel.overlay_rif_oid, gSwitchId, (uint32_t)overlay_intf_attrs.size(), overlay_intf_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create overlay router interface for MySID IPinIP tunnel: %d", status);
        return false;
    }

    vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    attr.value.oid = tunnel.overlay_rif_oid;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
    attr.value.s32 = SAI_TUNNEL_PEER_MODE_P2MP;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
    attr.value.s32 = dscp_mode;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
    attr.value.s32 = SAI_TUNNEL_TTL_MODE_PIPE_MODEL;
    tunnel_attrs.push_back(attr);

    status = sai_tunnel_api->create_tunnel(&tunnel.tunnel_oid, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create MySID IPinIP tunnel: %d", status);
        sai_router_intfs_api->remove_router_interface(tunnel.overlay_rif_oid);
        tunnel.overlay_rif_oid = SAI_NULL_OBJECT_ID;
        return false;
    }

    SWSS_LOG_INFO("Created MySID IPinIP tunnel");

    return true;
}

bool Srv6Orch::deinitIpInIpTunnel(MySidIpInIpTunnel& tunnel)
{
    SWSS_LOG_ENTER();

    if (tunnel.tunnel_oid != SAI_NULL_OBJECT_ID)
    {
        auto status = sai_tunnel_api->remove_tunnel(tunnel.tunnel_oid);
        if (status != SAI_STATUS_SUCCESS &&
            handleSaiRemoveStatus(SAI_API_TUNNEL, status) != task_success)
        {
            SWSS_LOG_ERROR("Failed to remove MySID IPinIP tunnel: %d", status);
            return false;
        }
        tunnel.tunnel_oid = SAI_NULL_OBJECT_ID;
    }

    if (tunnel.overlay_rif_oid != SAI_NULL_OBJECT_ID)
    {
        auto status = sai_router_intfs_api->remove_router_interface(tunnel.overlay_rif_oid);
        if (status != SAI_STATUS_SUCCESS &&
            handleSaiRemoveStatus(SAI_API_ROUTER_INTERFACE, status) != task_success)
        {
            SWSS_LOG_ERROR("Failed to remove MySID IPinIP tunnel RIF: %d", status);
            return false;
        }
        tunnel.overlay_rif_oid = SAI_NULL_OBJECT_ID;
    }

    SWSS_LOG_INFO("Removed MySID IPinIP tunnel");

    return true;
}

bool Srv6Orch::createMySidIpInIpTunnel(sai_tunnel_dscp_mode_t dscp_mode, sai_object_id_t& tunnel_oid)
{
    SWSS_LOG_ENTER();

    MySidIpInIpTunnel& uniform_tunnel = my_sid_ipinip_tunnels_.dscp_uniform_tunnel;
    MySidIpInIpTunnel& pipe_tunnel = my_sid_ipinip_tunnels_.dscp_pipe_tunnel;

    MySidIpInIpTunnel& tunnel_info = (dscp_mode == SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL) ? uniform_tunnel : pipe_tunnel;
    if (tunnel_info.refcount == 0)
    {
        auto ok = initIpInIpTunnel(tunnel_info, dscp_mode);
        if (!ok) {
            return false;
        }
    }

    tunnel_info.refcount++;
    tunnel_oid = tunnel_info.tunnel_oid;

    SWSS_LOG_INFO("Increased refcount for MySID IPinIP tunnel to %" PRIu64, tunnel_info.refcount);

    return true;
}

bool Srv6Orch::removeMySidIpInIpTunnel(sai_tunnel_dscp_mode_t dscp_mode)
{
    SWSS_LOG_ENTER();

    MySidIpInIpTunnel& uniform_tunnel = my_sid_ipinip_tunnels_.dscp_uniform_tunnel;
    MySidIpInIpTunnel& pipe_tunnel = my_sid_ipinip_tunnels_.dscp_pipe_tunnel;

    MySidIpInIpTunnel& tunnel_info = (dscp_mode == SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL) ? uniform_tunnel : pipe_tunnel;
    if (tunnel_info.refcount > 0)
    {
        tunnel_info.refcount--;
    }

    SWSS_LOG_INFO("Decreased refcount for MySID IPinIP tunnel to %" PRIu64, tunnel_info.refcount);

    if (tunnel_info.refcount == 0 &&
        (tunnel_info.tunnel_oid != SAI_NULL_OBJECT_ID ||
         tunnel_info.overlay_rif_oid != SAI_NULL_OBJECT_ID))
    {
        return deinitIpInIpTunnel(tunnel_info);
    }

    return true;
}

bool Srv6Orch::createMySidIpInIpTunnelTermEntry(sai_object_id_t tunnel_oid, const sai_ip6_t& sid_ip, sai_object_id_t& term_entry_oid)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> tunnel_table_entry_attrs;
    sai_attribute_t attr;

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = gVirtualRouterId;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel_oid;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
    attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(attr.value.ipaddr.addr.ip6, sid_ip, sizeof(attr.value.ipaddr.addr.ip6));
    tunnel_table_entry_attrs.push_back(attr);

    auto status = sai_tunnel_api->create_tunnel_term_table_entry(&term_entry_oid, gSwitchId, (uint32_t)tunnel_table_entry_attrs.size(), tunnel_table_entry_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel termination entry for MySID - %d", status);
        return false;
    }

    SWSS_LOG_INFO("Created tunnel termination entry for MySID entry");

    return true;
}

bool Srv6Orch::removeMySidIpInIpTunnelTermEntry(sai_object_id_t term_entry_oid)
{
    SWSS_LOG_ENTER();

    auto status = sai_tunnel_api->remove_tunnel_term_table_entry(term_entry_oid);
    if (status != SAI_STATUS_SUCCESS &&
        handleSaiRemoveStatus(SAI_API_TUNNEL, status) != task_success)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel termination entry for MySID entry - %d", status);
        return false;
    }

    SWSS_LOG_INFO("Removed tunnel termination entry for MySID entry");

    return true;
}

bool Srv6Orch::cleanupStaleMySidTunnel(MySidEntry& entry)
{
    if (entry.stale_tunnel_term_entry != SAI_NULL_OBJECT_ID)
    {
        if (!removeMySidIpInIpTunnelTermEntry(entry.stale_tunnel_term_entry))
        {
            return false;
        }
        entry.stale_tunnel_term_entry = SAI_NULL_OBJECT_ID;
    }

    if (entry.stale_tunnel_ref)
    {
        if (!removeMySidIpInIpTunnel(entry.stale_dscp_mode))
        {
            return false;
        }
        entry.stale_tunnel_ref = false;
    }

    return true;
}

void Srv6Orch::srv6TunnelUpdateNexthops(const string srv6_source, const NextHopKey nhkey, bool insert)
{
    if (insert)
    {
        srv6_tunnel_table_[srv6_source].nexthops.insert(nhkey);
    }
    else
    {
        srv6_tunnel_table_[srv6_source].nexthops.erase(nhkey);
    }
}

size_t Srv6Orch::srv6TunnelNexthopSize(const string srv6_source)
{
    return srv6_tunnel_table_[srv6_source].nexthops.size();
}

bool Srv6Orch::sidListExists(const string &segment_name)
{
    SWSS_LOG_ENTER();
    if (sid_table_.find(segment_name) != sid_table_.end())
    {
        return true;
    }
    return false;
}

bool Srv6Orch::createSrv6Tunnel(const string srv6_source)
{
    SWSS_LOG_ENTER();
    vector<sai_attribute_t> tunnel_attrs;
    sai_attribute_t attr;
    sai_status_t status;
    sai_object_id_t tunnel_id;

    if (srv6_tunnel_table_.find(srv6_source) != srv6_tunnel_table_.end())
    {
        SWSS_LOG_INFO("Tunnel exists for the source %s", srv6_source.c_str());
        return true;
    }

    SWSS_LOG_INFO("Create tunnel for the source %s", srv6_source.c_str());
    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_SRV6;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(attr);

    IpAddress src_ip(srv6_source);
    sai_ip_address_t ipaddr;
    ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(ipaddr.addr.ip6, src_ip.getV6Addr(), sizeof(ipaddr.addr.ip6));
    attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    attr.value.ipaddr = ipaddr;
    tunnel_attrs.push_back(attr);

    status = sai_tunnel_api->create_tunnel(&tunnel_id, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel for %s", srv6_source.c_str());
        return false;
    }
    srv6_tunnel_table_[srv6_source].tunnel_object_id = tunnel_id;
    return true;
}

bool Srv6Orch::srv6NexthopExists(const NextHopKey &nhKey)
{
    SWSS_LOG_ENTER();
    if (srv6_nexthop_table_.find(nhKey) != srv6_nexthop_table_.end())
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool Srv6Orch::removeSrv6NexthopWithoutVpn(const NextHopKey &nhKey)
{
    SWSS_LOG_ENTER();
    return deleteSrv6Nexthop(nhKey);
}

bool Srv6Orch::removeSrv6Nexthops(const std::vector<NextHopGroupKey> &nhgv)
{
    SWSS_LOG_ENTER();

    // 1. remove vpn_sid first
    for (auto& it_nhg : nhgv)
    {
        if (it_nhg.is_srv6_vpn())
        {
            for (auto &sr_nh : it_nhg.getNextHops())
            {
                if (sr_nh.isSrv6Vpn())
                {
                    if (!deleteSrv6Vpn(sr_nh.ip_address.to_string(), sr_nh.srv6_vpn_sid, getAggId(it_nhg)))
                    {
                        SWSS_LOG_ERROR("Failed to delete SRV6 vpn %s", sr_nh.to_string(false, true).c_str());
                        return false;
                    }
                }
            }
            decreasePrefixAggIdRefCount(it_nhg);
            deleteAggId(it_nhg);
        }
    }

    // 2. delete nexthop & prefix agg id
    for (auto& nhg : nhgv)
    {
        for (auto &sr_nh : nhg.getNextHops())
        {
            if (!deleteSrv6Nexthop(sr_nh))
            {
                SWSS_LOG_ERROR("Failed to delete SRV6 nexthop %s", sr_nh.to_string(false,true).c_str());
                return false;
            }
        }
    }

    return true;
}

bool Srv6Orch::createSrv6Nexthop(const NextHopKey &nh)
{
    SWSS_LOG_ENTER();
    string srv6_segment = nh.srv6_segment;
    string srv6_source = nh.srv6_source;
    string srv6_tunnel_endpoint;

    if (srv6NexthopExists(nh))
    {
        SWSS_LOG_INFO("SRV6 nexthop already created for %s", nh.to_string(false,true).c_str());
        return true;
    }

    sai_object_id_t srv6_segment_id;
    sai_object_id_t srv6_tunnel_id;

    if (srv6_segment == "")
    {
        srv6_segment_id = SAI_NULL_OBJECT_ID;
    }
    else
    {
        if (!sidListExists(srv6_segment))
        {
            SWSS_LOG_ERROR("Segment %s does not exist", srv6_segment.c_str());
            return false;
        }
        srv6_segment_id = sid_table_[srv6_segment].sid_object_id;
    }

    if (nh.ip_address.isZero())
    {
        srv6_tunnel_endpoint = srv6_source;
        srv6_tunnel_id = srv6_tunnel_table_[srv6_tunnel_endpoint].tunnel_object_id;
    }
    else
    {
        srv6_tunnel_endpoint = nh.ip_address.to_string();
        srv6_tunnel_id = srv6_p2p_tunnel_table_[srv6_tunnel_endpoint].tunnel_id;
    }

    SWSS_LOG_INFO("Create srv6 nh for tunnel src %s with seg %s", srv6_source.c_str(), srv6_segment.c_str());
    vector<sai_attribute_t> nh_attrs;
    sai_object_id_t nexthop_id;
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    attr.value.s32 = SAI_NEXT_HOP_TYPE_SRV6_SIDLIST;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_SRV6_SIDLIST_ID;
    attr.value.oid = srv6_segment_id;
    nh_attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    attr.value.oid = srv6_tunnel_id;
    nh_attrs.push_back(attr);

    status = sai_next_hop_api->create_next_hop(&nexthop_id, gSwitchId,
                                                (uint32_t)nh_attrs.size(),
                                                nh_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create srv6 nexthop for %s", nh.to_string(false,true).c_str());
        return false;
    }
    m_neighOrch->updateSrv6Nexthop(nh, nexthop_id);
    srv6_nexthop_table_[nh] = nexthop_id;
    if (srv6_segment != "")
    {
        sid_table_[srv6_segment].nexthops.insert(nh);
    }

    if (nh.ip_address.isZero())
    {
        srv6TunnelUpdateNexthops(srv6_source, nh, true);
    }
    else
    {
        srv6P2ptunnelUpdateNexthops(nh, true);
    }
    return true;
}

bool Srv6Orch::deleteSrv6Nexthop(const NextHopKey &nh)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    if (!srv6NexthopExists(nh))
    {
        return true;
    }

    SWSS_LOG_DEBUG("SRV6 Nexthop %s refcount %d", nh.to_string(false,true).c_str(), m_neighOrch->getNextHopRefCount(nh));
    if (m_neighOrch->getNextHopRefCount(nh) == 0)
    {
        sai_object_id_t nexthop_id;
        nexthop_id = srv6_nexthop_table_[nh];
        status = sai_next_hop_api->remove_next_hop(nexthop_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove SRV6 nexthop %s", nh.to_string(false,true).c_str());
            return false;
        }

        /* Decrease srv6 segment reference */
        if (nh.srv6_segment != "")
        {
            /* Update nexthop in SID table after deleting the nexthop */
            SWSS_LOG_INFO("Seg %s nexthop refcount %zu",
                      nh.srv6_segment.c_str(),
                      sid_table_[nh.srv6_segment].nexthops.size());
            if (sid_table_[nh.srv6_segment].nexthops.find(nh) != sid_table_[nh.srv6_segment].nexthops.end())
            {
                sid_table_[nh.srv6_segment].nexthops.erase(nh);
            }
        }
        m_neighOrch->updateSrv6Nexthop(nh, 0);

        srv6_nexthop_table_.erase(nh);

        /* Delete NH from the tunnel map */
        SWSS_LOG_INFO("Delete NH %s from tunnel map",
            nh.to_string(false, true).c_str());

        if (nh.ip_address.isZero())
        {
            string srv6_source = nh.srv6_source;
            srv6TunnelUpdateNexthops(srv6_source, nh, false);
            size_t tunnel_nhs = srv6TunnelNexthopSize(srv6_source);
            if (tunnel_nhs == 0)
            {
                status = sai_tunnel_api->remove_tunnel(srv6_tunnel_table_[srv6_source].tunnel_object_id);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to remove SRV6 tunnel object for source %s", srv6_source.c_str());
                    return false;
                }
                srv6_tunnel_table_.erase(srv6_source);
            }
            else
            {
                SWSS_LOG_INFO("Nexthops referencing this tunnel object %s: %zu", srv6_source.c_str(),tunnel_nhs);
            }
        }
        else
        {
            std::string endpoint = nh.ip_address.to_string();
            srv6P2ptunnelUpdateNexthops(nh, false);
            if (!deleteSrv6P2pTunnel(endpoint))
            {
                SWSS_LOG_ERROR("Failed to remove SRV6 p2p tunnel object for dst %s,", endpoint.c_str());
                return false;
            }
        }
    }

    return true;
}

bool Srv6Orch::createSrv6NexthopWithoutVpn(const NextHopKey &nh, sai_object_id_t &nexthop_id)
{
    SWSS_LOG_ENTER();

    // 1. create tunnel
    if (nh.ip_address.isZero())
    {
        // create srv6 tunnel
        auto srv6_source = nh.srv6_source;
        if (!createSrv6Tunnel(srv6_source))
        {
            SWSS_LOG_ERROR("Failed to create tunnel for source %s", srv6_source.c_str());
            return false;
        }
    }
    else
    {
        // create p2p tunnel
        if (!createSrv6P2pTunnel(nh.srv6_source, nh.ip_address.to_string()))
        {
            SWSS_LOG_ERROR("Failed to create SRV6 p2p tunnel %s", nh.to_string(false, true).c_str());
            return false;
        }
    }

    // 2. create nexthop
    if (!createSrv6Nexthop(nh))
    {
        SWSS_LOG_ERROR("Failed to create SRV6 nexthop %s", nh.to_string(false,true).c_str());
        return false;
    }

    nexthop_id = srv6_nexthop_table_[nh];
    return true;
}

bool Srv6Orch::srv6Nexthops(const NextHopGroupKey &nhgKey, sai_object_id_t &nexthop_id)
{
    SWSS_LOG_ENTER();
    set<NextHopKey> nexthops = nhgKey.getNextHops();

    for (auto nh : nexthops)
    { 
        // create SRv6 nexthop
        if (!createSrv6NexthopWithoutVpn(nh, nexthop_id))
        {
            SWSS_LOG_ERROR("Failed to create SRv6 nexthop %s", nh.to_string(false, true).c_str());
            return false;
        }
    }

    // create SRv6 VPN if need
    if (nhgKey.is_srv6_vpn())
    {
        for (auto it = nexthops.begin(); it != nexthops.end(); ++it)
        {
            if (it->isSrv6Vpn())
            {
                if (!createSrv6Vpn(it->ip_address.to_string(), it->srv6_vpn_sid, getAggId(nhgKey)))
                {
                    SWSS_LOG_ERROR("Failed to create SRV6 vpn %s", it->to_string(false, true).c_str());
                    return false;
                }
            }
        }

        increasePrefixAggIdRefCount(nhgKey);
    }

    if (nhgKey.getSize() == 1)
    {
        NextHopKey nhkey(nhgKey.to_string(), false, true);
        nexthop_id = srv6_nexthop_table_[nhkey];
    }
    return true;
}

bool Srv6Orch::createUpdateSidList(const string sid_name, const string sid_list, const string sidlist_type)
{
    SWSS_LOG_ENTER();
    bool exists = (sid_table_.find(sid_name) != sid_table_.end()) && sid_table_[sid_name].sid_object_id;
    sai_segment_list_t segment_list;
    vector<string>sid_ips = tokenize(sid_list, SID_LIST_DELIMITER);
    sai_object_id_t segment_oid;
    segment_list.count = (uint32_t)sid_ips.size();
    if (segment_list.count == 0)
    {
        SWSS_LOG_ERROR("segment list count is zero, skip");
        return true;
    }
    SWSS_LOG_INFO("Segment count %d", segment_list.count);
    auto segment_buf = std::make_unique<sai_ip6_t[]>(segment_list.count);
    segment_list.list = segment_buf.get();
    uint32_t index = 0;

    for (string ip_str : sid_ips)
    {
        IpPrefix ip(ip_str);
        SWSS_LOG_INFO("Segment %s, count %d", ip.to_string().c_str(), segment_list.count);
        memcpy(segment_list.list[index++], ip.getIp().getV6Addr(), 16);
    }
    sai_attribute_t attr;
    sai_status_t status;
    if (!exists)
    {
        /* Create sidlist object with list of ipv6 prefixes */
        SWSS_LOG_INFO("Create SID list");
        vector<sai_attribute_t> attributes;
        attr.id = SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST;
        attr.value.segmentlist.list = segment_list.list;
        attr.value.segmentlist.count = segment_list.count;
        attributes.push_back(attr);

        attr.id = SAI_SRV6_SIDLIST_ATTR_TYPE;
        if (sidlist_type_map.find(sidlist_type) == sidlist_type_map.end())
        {
            SWSS_LOG_INFO("Use default sidlist type: ENCAPS_RED");
            attr.value.s32 = SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED;
        }
        else
        {
            SWSS_LOG_INFO("sidlist type: %s", sidlist_type.c_str());
            attr.value.s32 = sidlist_type_map.at(sidlist_type);
        }
        attributes.push_back(attr);
        status = sai_srv6_api->create_srv6_sidlist(&segment_oid, gSwitchId, (uint32_t) attributes.size(), attributes.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create srv6 sidlist object, rv %d", status);
            return false;
        }
        sid_table_[sid_name].sid_object_id = segment_oid;
    }
    else
    {
        SWSS_LOG_INFO("Set SID list");

        /* Update sidlist object with new set of ipv6 addresses */
        attr.id = SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST;
        attr.value.segmentlist.list = segment_list.list;
        attr.value.segmentlist.count = segment_list.count;
        segment_oid = (sid_table_.find(sid_name)->second).sid_object_id;
        status = sai_srv6_api->set_srv6_sidlist_attribute(segment_oid, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set srv6 sidlist object with new segments, rv %d", status);
            return false;
        }
    }
    return true;
}

task_process_status Srv6Orch::deleteSidList(const string sid_name)
{
    SWSS_LOG_ENTER();
    sai_status_t status = SAI_STATUS_SUCCESS;
    if (sid_table_.find(sid_name) == sid_table_.end())
    {
        SWSS_LOG_ERROR("segment name %s doesn't exist", sid_name.c_str());
        return task_process_status::task_failed;
    }

    if (sid_table_[sid_name].nexthops.size() > 0)
    {
        SWSS_LOG_NOTICE("segment object %s referenced by other nexthops: count %zu, not deleting",
                      sid_name.c_str(), sid_table_[sid_name].nexthops.size());
        return task_process_status::task_need_retry;
    }
    SWSS_LOG_INFO("Remove sid list, segname %s", sid_name.c_str());
    status = sai_srv6_api->remove_srv6_sidlist(sid_table_[sid_name].sid_object_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete SRV6 sidlist object for %s", sid_name.c_str());
        return task_process_status::task_failed;
    }
    sid_table_.erase(sid_name);
    return task_process_status::task_success;
}

task_process_status Srv6Orch::doTaskSidTable(const KeyOpFieldsValuesTuple & tuple)
{
    SWSS_LOG_ENTER();
    string sid_name = kfvKey(tuple);
    string op = kfvOp(tuple);
    string sid_list, sidlist_type;

    for (auto i : kfvFieldsValues(tuple))
    {
        if (fvField(i) == "path")
        {
          sid_list = fvValue(i);
        }
        if (fvField(i) == "type")
        {
          sidlist_type = fvValue(i);
        }
    }
    if (op == SET_COMMAND)
    {
        if (!createUpdateSidList(sid_name, sid_list, sidlist_type))
        {
          SWSS_LOG_ERROR("Failed to process sid %s", sid_name.c_str());
          return task_process_status::task_failed;
        }
    }
    else if (op == DEL_COMMAND)
    {
        task_process_status status = deleteSidList(sid_name);
        if (status != task_process_status::task_success)
        {
            SWSS_LOG_ERROR("Failed to delete sid %s", sid_name.c_str());
            return status;
        }
    } else {
        SWSS_LOG_ERROR("Invalid command");
        return task_process_status::task_failed;
    }

    return task_process_status::task_success;
}

bool Srv6Orch::mySidExists(string my_sid_string)
{
    if (srv6_my_sid_table_.find(my_sid_string) != srv6_my_sid_table_.end())
    {
        return true;
    }
    return false;
}

/*
 * Neighbor change notification to be processed for the SRv6 MySID entries
 *
 * In summary, this function handles both add and delete neighbor notifications
 *
 * When a neighbor ADD notification is received, we do the following steps:
 *     - We walk through the list of pending SRv6 MySID entries that are waiting for this neighbor to be ready
 *     - For each SID, we install the SID into the ASIC
 *     - We remove the SID from the pending MySID entries list
 * 
 * When a neighbor DELETE notification is received, we do the following steps:
 *     - We walk through the list of pending SRv6 MySID entries installed in the ASIC
 *     - For each SID, we remove the SID from the ASIC
 *     - We add the SID to the pending MySID entries list
 */
void Srv6Orch::updateNeighbor(const NeighborUpdate& update)
{
    SWSS_LOG_ENTER();

    /* Check if the received notification is a neighbor add or a neighbor delete */
    if (update.add)
    {
        SWSS_LOG_INFO("Neighbor ADD event: %s alias '%s', retrying pending SRv6 SIDs",
                        update.entry.ip_address.to_string().c_str(), update.entry.alias.c_str());
        NextHopKey nexthop(update.entry.ip_address.to_string(), update.entry.alias);
        notifyRetry(this, APP_SRV6_MY_SID_TABLE_NAME,
                    make_constraint(RETRY_CST_MYSID_NEXTHOP, nexthop.to_string()));
    }
    else
    {
        /*
         * It's a neighbor delete notification, let's uninstall the SRv6 MySID entries associated with that
         * nexthop from the ASIC, and add them to the SRv6 MySID entries pending set.
         */

        SWSS_LOG_INFO("Neighbor DELETE event: %s alias '%s', removing associated SRv6 SIDs",
                        update.entry.ip_address.to_string().c_str(), update.entry.alias.c_str());

        for (auto it = srv6_my_sid_table_.begin(); it != srv6_my_sid_table_.end();)
        {
            /* Skip SIDs that are not associated with a L3 Adjacency */
            if (it->second.endAdjString.empty())
            {
                ++it;
                continue;
            }

            try
            {
                NextHopKey entry_nexthop(it->second.endAdjString);
                NextHopKey updated_nexthop(update.entry.ip_address, update.entry.alias);
                if (entry_nexthop != updated_nexthop)
                {
                    ++it;
                    continue;
                }
            }
            catch (const std::invalid_argument &e)
            {
                /* SRv6 SID is associated with an invalid L3 Adjacency IP address, skipping */
                ++it;
                continue;
            }

            /*
             * Save SID entry information to temp variables, before removing the SID.
             * This information will be consumed used later. 
             */
            string my_sid_string = it->first;
            const string dt_vrf = it->second.endVrfString;
            const string adj = it->second.endAdjString;
            string end_action;
            for (auto iter = end_behavior_map.begin(); iter != end_behavior_map.end(); iter++)
            {
                if (iter->second == it->second.endBehavior)
                {
                    end_action = iter->first;
                    break;
                }
            }

            /* Skip SIDs with unknown SRv6 behavior */
            if (end_action.empty())
            {
                ++it;
                continue;
            }

            SWSS_LOG_INFO("Removing SID %s, action %s, vrf %s, adj %s", my_sid_string.c_str(), dt_vrf.c_str(), adj.c_str(), end_action.c_str());

            /* Let's delete the SID from the ASIC */
            unordered_map<string, MySidEntry>::iterator tmp = it;
            ++tmp;
            if (deleteMysidEntry(it->first) != task_success)
            {
                SWSS_LOG_ERROR("Failed to delete my_sid entry for sid %s", it->first.c_str());
                ++it;
                continue;
            }
            it = tmp;

            SWSS_LOG_INFO("SID %s removed successfully", my_sid_string.c_str());

            vector<FieldValueTuple> fields = {{"action", end_action}, {"adj", adj}};
            if (!dt_vrf.empty())
            {
                fields.emplace_back("vrf", dt_vrf);
            }
            addToRetry(APP_SRV6_MY_SID_TABLE_NAME,
                       Task{my_sid_string, SET_COMMAND, fields},
                       make_constraint(RETRY_CST_MYSID_NEXTHOP, NextHopKey(adj).to_string()));
        }
    }
}

void Srv6Orch::notifyVrfAvailable(const std::string &vrf)
{
    notifyRetry(this, APP_SRV6_MY_SID_TABLE_NAME,
                make_constraint(RETRY_CST_MYSID_VRF, vrf));
}

void Srv6Orch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    assert(cntx);

    switch(type) {
    case SUBJECT_TYPE_NEIGH_CHANGE:
    {
        NeighborUpdate *update = static_cast<NeighborUpdate *>(cntx);
        updateNeighbor(*update);
        break;
    }
    default:
        // Received update in which we are not interested
        // Ignore it
        return;
    }
}

bool Srv6Orch::sidEntryEndpointBehavior(string action, sai_my_sid_entry_endpoint_behavior_t &end_behavior,
                                        sai_my_sid_entry_endpoint_behavior_flavor_t &end_flavor)
{
    if (end_behavior_map.find(action) == end_behavior_map.end())
    {
        SWSS_LOG_ERROR("Invalid endpoint behavior function");
        return false;
    }
    end_behavior = end_behavior_map.at(action);

    if (end_flavor_map.find(action) != end_flavor_map.end())
    {
        end_flavor = end_flavor_map.at(action);
    }

    return true;
}

bool Srv6Orch::mySidVrfRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior)
{
    if (end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_T ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT4 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT6 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT46)
    {
      return true;
    }
    return false;
}

bool Srv6Orch::mySidNextHopRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior)
{
    if (end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDX4 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDX6 ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_ENCAPS_RED ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_B6_INSERT_RED ||
        end_behavior == SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UA)
    {
      return true;
    }
    return false;
}

bool Srv6Orch::mySidTunnelRequired(const string& my_sid_addr, const sai_my_sid_entry_t& sai_entry, sai_my_sid_entry_endpoint_behavior_t end_behavior, boost::optional<sai_tunnel_dscp_mode_t>& dscp_mode)
{
    if (end_behavior != SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN &&
        end_behavior != SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT46)
    {
        return false;
    }

    auto locator_cfg = getMySidEntryLocatorCfg(sai_entry);

    auto status = getMySidEntryDscpMode(my_sid_addr, locator_cfg, dscp_mode);
    return status && dscp_mode.has_value();
}

task_process_status Srv6Orch::createUpdateMysidEntry(string my_sid_string, const string dt_vrf,
                                                     const string adj, const string end_action)
{
    SWSS_LOG_ENTER();
    const string key_string = my_sid_string;
    sai_my_sid_entry_endpoint_behavior_t end_behavior;
    sai_my_sid_entry_endpoint_behavior_flavor_t end_flavor = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE;

    if (!sidEntryEndpointBehavior(end_action, end_behavior, end_flavor))
    {
        SWSS_LOG_ERROR("Invalid my_sid action %s", end_action.c_str());
        return task_invalid_entry;
    }

    sai_my_sid_entry_t my_sid_entry{};
    my_sid_entry.vr_id = gVirtualRouterId;
    my_sid_entry.switch_id = gSwitchId;

    array<uint8_t, 4> lengths{};
    size_t offset = 0;
    try
    {
        for (size_t index = 0; index < lengths.size(); ++index)
        {
            auto delimiter = key_string.find(MY_SID_KEY_DELIMITER, offset);
            if (delimiter == string::npos || delimiter == offset)
            {
                SWSS_LOG_ERROR("Invalid MySID key %s", key_string.c_str());
                return task_invalid_entry;
            }
            auto value = stoi(key_string.substr(offset, delimiter - offset));
            if (value < 0 || value > 128)
            {
                SWSS_LOG_ERROR("Invalid MySID locator length in key %s", key_string.c_str());
                return task_invalid_entry;
            }
            lengths[index] = static_cast<uint8_t>(value);
            offset = delimiter + 1;
        }

        my_sid_string = key_string.substr(offset);
        IpAddress address(my_sid_string);
        if (address.isV4())
        {
            SWSS_LOG_ERROR("MySID address must be IPv6 in key %s", key_string.c_str());
            return task_invalid_entry;
        }
        memcpy(my_sid_entry.sid, address.getV6Addr(), sizeof(my_sid_entry.sid));
    }
    catch (const exception& error)
    {
        SWSS_LOG_ERROR("Invalid MySID key %s: %s", key_string.c_str(), error.what());
        return task_invalid_entry;
    }

    my_sid_entry.locator_block_len = lengths[0];
    my_sid_entry.locator_node_len = lengths[1];
    my_sid_entry.function_len = lengths[2];
    my_sid_entry.args_len = lengths[3];
    uint32_t total_length = static_cast<uint32_t>(lengths[0]) + lengths[1] + lengths[2] + lengths[3];
    if (total_length > 128)
    {
        SWSS_LOG_ERROR("MySID locator lengths exceed 128 bits in key %s", key_string.c_str());
        return task_invalid_entry;
    }

    SWSS_LOG_INFO("MySid: sid %s, action %s, vrf %s, block %d, node %d, func %d, arg %d dt_vrf %s, adj %s",
      my_sid_string.c_str(), end_action.c_str(), dt_vrf.c_str(),my_sid_entry.locator_block_len, my_sid_entry.locator_node_len,
      my_sid_entry.function_len, my_sid_entry.args_len, dt_vrf.c_str(), adj.c_str());

    sai_object_id_t vrf_oid = gVirtualRouterId;
    if (mySidVrfRequired(end_behavior))
    {
        if (dt_vrf == "default")
        {
            vrf_oid = gVirtualRouterId;
        }
        else if (m_vrfOrch->isVRFexists(dt_vrf))
        {
            vrf_oid = m_vrfOrch->getVRFid(dt_vrf);
            if (vrf_oid == SAI_NULL_OBJECT_ID)
            {
                return task_need_retry;
            }
        }
        else
        {
            return task_need_retry;
        }
    }

    NextHopKey nexthop;
    sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;
    if (mySidNextHopRequired(end_behavior))
    {
        vector<string> adjv = tokenize(adj, ADJ_DELIMITER);
        if (adjv.size() > 1)
        {
            SWSS_LOG_ERROR("Failed to create my_sid entry %s adj %s: ECMP adjacency not yet supported", key_string.c_str(), adj.c_str());
            return task_invalid_entry;
        }

        nexthop = NextHopKey(adj);
        if (!m_neighOrch->hasNextHop(nexthop))
        {
            return task_need_retry;
        }
        next_hop_oid = m_neighOrch->getNextHopId(nexthop);
        if (next_hop_oid == SAI_NULL_OBJECT_ID)
        {
            return task_need_retry;
        }
    }

    boost::optional<sai_tunnel_dscp_mode_t> dscp_mode;
    bool tunnel_required = mySidTunnelRequired(my_sid_string, my_sid_entry, end_behavior, dscp_mode);
    bool counter_required = getMySidCountersSupported() && getMySidCountersEnabled();
    if (tunnel_required)
    {
        end_flavor = SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_USD;
    }
    auto existing = srv6_my_sid_table_.find(key_string);
    bool entry_exists = existing != srv6_my_sid_table_.end();

    if (entry_exists && existing->second.sai_removed)
    {
        auto cleanup_status = deleteMysidEntry(key_string);
        if (cleanup_status != task_success)
        {
            return cleanup_status;
        }
        existing = srv6_my_sid_table_.end();
        entry_exists = false;
    }

    if (entry_exists && !cleanupStaleMySidTunnel(existing->second))
    {
        return task_need_retry;
    }

    if (entry_exists)
    {
        const auto& current = existing->second;
        bool same_tunnel = tunnel_required
                        ? current.tunnel_oid != SAI_NULL_OBJECT_ID &&
                            current.tunnel_term_entry != SAI_NULL_OBJECT_ID &&
                            current.dscp_mode == dscp_mode.get()
            : current.tunnel_oid == SAI_NULL_OBJECT_ID;
        bool same_counter = !counter_required || current.counter != SAI_NULL_OBJECT_ID;
        if (!current.sai_removed && current.endBehavior == end_behavior &&
            current.endFlavor == end_flavor &&
            current.endVrfString == (mySidVrfRequired(end_behavior) ? dt_vrf : "") &&
            current.endAdjString == (mySidNextHopRequired(end_behavior) ? adj : "") &&
            current.vrf_oid == vrf_oid && current.next_hop_oid == next_hop_oid &&
            same_tunnel && same_counter)
        {
            SWSS_LOG_INFO("MySID entry %s already matches desired state", key_string.c_str());
            return task_success;
        }
    }

    sai_object_id_t tunnel_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t tunnel_term_entry = SAI_NULL_OBJECT_ID;
    bool acquired_tunnel = false;
    bool created_tunnel_term = false;
    bool retarget_tunnel_term = false;
    if (tunnel_required)
    {
        bool reuse_tunnel = entry_exists && existing->second.tunnel_oid != SAI_NULL_OBJECT_ID &&
                            existing->second.dscp_mode == dscp_mode.get();
        if (reuse_tunnel)
        {
            tunnel_oid = existing->second.tunnel_oid;
            tunnel_term_entry = existing->second.tunnel_term_entry;
            if (tunnel_term_entry == SAI_NULL_OBJECT_ID)
            {
                if (!createMySidIpInIpTunnelTermEntry(tunnel_oid, my_sid_entry.sid, tunnel_term_entry))
                {
                    return task_need_retry;
                }
                created_tunnel_term = true;
            }
        }
        else
        {
            if (!createMySidIpInIpTunnel(dscp_mode.get(), tunnel_oid))
            {
                return task_need_retry;
            }
            acquired_tunnel = true;
            if (entry_exists && existing->second.tunnel_term_entry != SAI_NULL_OBJECT_ID)
            {
                tunnel_term_entry = existing->second.tunnel_term_entry;
                retarget_tunnel_term = true;
            }
            else if (!createMySidIpInIpTunnelTermEntry(tunnel_oid, my_sid_entry.sid, tunnel_term_entry))
            {
                removeMySidIpInIpTunnel(dscp_mode.get());
                return task_need_retry;
            }
            else
            {
                created_tunnel_term = true;
            }
        }
    }

    auto makeOidAttr = [](sai_attr_id_t id, sai_object_id_t oid)
    {
        sai_attribute_t attr{};
        attr.id = id;
        attr.value.oid = oid;
        return attr;
    };
    auto makeS32Attr = [](sai_attr_id_t id, int32_t value)
    {
        sai_attribute_t attr{};
        attr.id = id;
        attr.value.s32 = value;
        return attr;
    };

    if (!entry_exists)
    {
        vector<sai_attribute_t> attributes;
        if (mySidVrfRequired(end_behavior))
        {
            attributes.push_back(makeOidAttr(SAI_MY_SID_ENTRY_ATTR_VRF, vrf_oid));
        }
        if (mySidNextHopRequired(end_behavior))
        {
            attributes.push_back(makeOidAttr(SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID, next_hop_oid));
        }
        if (tunnel_required)
        {
            attributes.push_back(makeOidAttr(SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID, tunnel_oid));
        }
        attributes.push_back(makeS32Attr(SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR, end_behavior));
        if (end_flavor != SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_NONE)
        {
            attributes.push_back(makeS32Attr(SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR, end_flavor));
        }

        sai_object_id_t counter_oid = SAI_NULL_OBJECT_ID;
        if (counter_required)
        {
            if (!addMySidCounter(my_sid_entry, counter_oid))
            {
                if (created_tunnel_term)
                {
                    removeMySidIpInIpTunnelTermEntry(tunnel_term_entry);
                }
                if (acquired_tunnel)
                {
                    removeMySidIpInIpTunnel(dscp_mode.get());
                }
                return task_need_retry;
            }
            attributes.push_back(makeOidAttr(SAI_MY_SID_ENTRY_ATTR_COUNTER_ID, counter_oid));
        }

        auto status = sai_srv6_api->create_my_sid_entry(&my_sid_entry, (uint32_t) attributes.size(), attributes.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create my_sid entry %s, rv %d", key_string.c_str(), status);
            auto handle_status = handleSaiCreateStatus(SAI_API_SRV6, status);
            if (handle_status != task_success)
            {
                removeMySidCounter(my_sid_entry, counter_oid);
                if (created_tunnel_term)
                {
                    removeMySidIpInIpTunnelTermEntry(tunnel_term_entry);
                }
                if (acquired_tunnel)
                {
                    removeMySidIpInIpTunnel(dscp_mode.get());
                }
                return handle_status;
            }
        }

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_SRV6_MY_SID_ENTRY);
        MySidEntry applied;
        applied.entry = my_sid_entry;
        applied.endBehavior = end_behavior;
        applied.endFlavor = end_flavor;
        applied.endVrfString = mySidVrfRequired(end_behavior) ? dt_vrf : "";
        applied.endAdjString = mySidNextHopRequired(end_behavior) ? adj : "";
        applied.vrf_oid = vrf_oid;
        applied.next_hop_oid = next_hop_oid;
        applied.tunnel_oid = tunnel_oid;
        applied.tunnel_term_entry = tunnel_term_entry;
        if (tunnel_required)
        {
            applied.dscp_mode = dscp_mode.get();
        }
        applied.counter = counter_oid;
        srv6_my_sid_table_.emplace(key_string, applied);

        if (mySidVrfRequired(end_behavior))
        {
            m_vrfOrch->increaseVrfRefCount(dt_vrf);
        }
        if (mySidNextHopRequired(end_behavior))
        {
            m_neighOrch->increaseNextHopRefCount(nexthop, 1);
        }
        return task_success;
    }

    auto current = existing->second;
    sai_object_id_t counter_oid = current.counter;
    bool created_counter = false;
    if (counter_required && counter_oid == SAI_NULL_OBJECT_ID)
    {
        if (!addMySidCounter(my_sid_entry, counter_oid))
        {
            if (created_tunnel_term)
            {
                removeMySidIpInIpTunnelTermEntry(tunnel_term_entry);
            }
            if (acquired_tunnel)
            {
                removeMySidIpInIpTunnel(dscp_mode.get());
            }
            return task_need_retry;
        }
        created_counter = true;
    }

    struct AttributeChange
    {
        sai_attribute_t desired;
        sai_attribute_t previous;
    };
    vector<AttributeChange> changes;
    if (current.endBehavior != end_behavior)
    {
        changes.push_back({
            makeS32Attr(SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR, end_behavior),
            makeS32Attr(SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR, current.endBehavior)});
    }
    if (current.endFlavor != end_flavor)
    {
        changes.push_back({
            makeS32Attr(SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR, end_flavor),
            makeS32Attr(SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR, current.endFlavor)});
    }
    if (current.vrf_oid != vrf_oid)
    {
        changes.push_back({
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_VRF, vrf_oid),
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_VRF, current.vrf_oid)});
    }
    if (current.next_hop_oid != next_hop_oid)
    {
        changes.push_back({
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID, next_hop_oid),
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID, current.next_hop_oid)});
    }
    if (current.tunnel_oid != tunnel_oid)
    {
        changes.push_back({
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID, tunnel_oid),
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID, current.tunnel_oid)});
    }
    if (current.counter != counter_oid)
    {
        changes.push_back({
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_COUNTER_ID, counter_oid),
            makeOidAttr(SAI_MY_SID_ENTRY_ATTR_COUNTER_ID, current.counter)});
    }

    size_t applied_count = 0;
    for (const auto& change : changes)
    {
        auto status = sai_srv6_api->set_my_sid_entry_attribute(&my_sid_entry, &change.desired);
        if (status != SAI_STATUS_SUCCESS)
        {
            while (applied_count > 0)
            {
                --applied_count;
                auto rollback_status = sai_srv6_api->set_my_sid_entry_attribute(
                    &my_sid_entry, &changes[applied_count].previous);
                if (rollback_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to roll back my_sid entry %s attribute %d, rv %d",
                                   key_string.c_str(), changes[applied_count].previous.id, rollback_status);
                }
            }
            if (created_tunnel_term)
            {
                removeMySidIpInIpTunnelTermEntry(tunnel_term_entry);
            }
            if (acquired_tunnel)
            {
                removeMySidIpInIpTunnel(dscp_mode.get());
            }
            if (created_counter)
            {
                removeMySidCounter(my_sid_entry, counter_oid);
            }
            return handleSaiSetStatus(SAI_API_SRV6, status);
        }
        ++applied_count;
    }

    if (retarget_tunnel_term)
    {
        sai_attribute_t tunnel_attr{};
        tunnel_attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
        tunnel_attr.value.oid = tunnel_oid;
        auto status = sai_tunnel_api->set_tunnel_term_table_entry_attribute(
            tunnel_term_entry, &tunnel_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            while (applied_count > 0)
            {
                --applied_count;
                auto rollback_status = sai_srv6_api->set_my_sid_entry_attribute(
                    &my_sid_entry, &changes[applied_count].previous);
                if (rollback_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to roll back my_sid entry %s attribute %d, rv %d",
                                   key_string.c_str(), changes[applied_count].previous.id, rollback_status);
                }
            }
            if (acquired_tunnel)
            {
                removeMySidIpInIpTunnel(dscp_mode.get());
            }
            if (created_counter)
            {
                removeMySidCounter(my_sid_entry, counter_oid);
            }
            return handleSaiSetStatus(SAI_API_TUNNEL, status);
        }
    }

    MySidEntry applied = current;
    applied.entry = my_sid_entry;
    applied.endBehavior = end_behavior;
    applied.endFlavor = end_flavor;
    applied.endVrfString = mySidVrfRequired(end_behavior) ? dt_vrf : "";
    applied.endAdjString = mySidNextHopRequired(end_behavior) ? adj : "";
    applied.vrf_oid = vrf_oid;
    applied.next_hop_oid = next_hop_oid;
    applied.tunnel_oid = tunnel_oid;
    applied.tunnel_term_entry = tunnel_term_entry;
    applied.counter = counter_oid;
    applied.sai_removed = false;
    if (tunnel_required)
    {
        applied.dscp_mode = dscp_mode.get();
    }
    if (current.tunnel_oid != SAI_NULL_OBJECT_ID && current.tunnel_oid != tunnel_oid)
    {
        if (current.tunnel_term_entry != tunnel_term_entry)
        {
            applied.stale_tunnel_term_entry = current.tunnel_term_entry;
        }
        applied.stale_dscp_mode = current.dscp_mode;
        applied.stale_tunnel_ref = true;
    }
    srv6_my_sid_table_[key_string] = applied;

    bool old_vrf_required = mySidVrfRequired(current.endBehavior);
    bool new_vrf_required = mySidVrfRequired(end_behavior);
    if (new_vrf_required && (!old_vrf_required || current.endVrfString != dt_vrf))
    {
        m_vrfOrch->increaseVrfRefCount(dt_vrf);
    }
    if (old_vrf_required && (!new_vrf_required || current.endVrfString != dt_vrf))
    {
        m_vrfOrch->decreaseVrfRefCount(current.endVrfString);
    }

    bool old_nh_required = mySidNextHopRequired(current.endBehavior);
    bool new_nh_required = mySidNextHopRequired(end_behavior);
    if (new_nh_required && (!old_nh_required || current.endAdjString != adj))
    {
        m_neighOrch->increaseNextHopRefCount(nexthop, 1);
    }
    if (old_nh_required && (!new_nh_required || current.endAdjString != adj))
    {
        m_neighOrch->decreaseNextHopRefCount(NextHopKey(current.endAdjString), 1);
    }

    return cleanupStaleMySidTunnel(srv6_my_sid_table_[key_string]) ? task_success : task_need_retry;
}

task_process_status Srv6Orch::deleteMysidEntry(const string my_sid_string)
{
    auto it = srv6_my_sid_table_.find(my_sid_string);
    if (it == srv6_my_sid_table_.end())
    {
        SWSS_LOG_INFO("My_sid_entry already absent for %s", my_sid_string.c_str());
        return task_success;
    }

    auto& cached = it->second;
    auto& my_sid_entry = cached.entry;

    SWSS_LOG_NOTICE("MySid Delete: sid %s", my_sid_string.c_str());
    if (!cached.sai_removed)
    {
        auto status = sai_srv6_api->remove_my_sid_entry(&my_sid_entry);
        if (status != SAI_STATUS_SUCCESS)
        {
            auto handle_status = handleSaiRemoveStatus(SAI_API_SRV6, status);
            if (handle_status != task_success)
            {
                return handle_status;
            }
        }
        cached.sai_removed = true;
    }

    if (!cached.crm_released)
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_SRV6_MY_SID_ENTRY);
        cached.crm_released = true;
    }

    if (!removeMySidCounter(my_sid_entry, cached.counter))
    {
        return task_need_retry;
    }

    if (!cached.references_released)
    {
        if (mySidVrfRequired(cached.endBehavior))
        {
            m_vrfOrch->decreaseVrfRefCount(cached.endVrfString);
        }
        if (mySidNextHopRequired(cached.endBehavior))
        {
            m_neighOrch->decreaseNextHopRefCount(NextHopKey(cached.endAdjString), 1);
        }
        cached.references_released = true;
    }

    if (!cleanupStaleMySidTunnel(cached))
    {
        return task_need_retry;
    }

    if (cached.tunnel_term_entry != SAI_NULL_OBJECT_ID)
    {
        if (!removeMySidIpInIpTunnelTermEntry(cached.tunnel_term_entry))
        {
            return task_need_retry;
        }
        cached.tunnel_term_entry = SAI_NULL_OBJECT_ID;
    }
    if (cached.tunnel_oid != SAI_NULL_OBJECT_ID)
    {
        if (!removeMySidIpInIpTunnel(cached.dscp_mode))
        {
            return task_need_retry;
        }
        cached.tunnel_oid = SAI_NULL_OBJECT_ID;
    }

    srv6_my_sid_table_.erase(it);
    return task_success;
}

uint32_t Srv6Orch::getAggId(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();
    static uint32_t g_agg_id = 1;
    uint32_t agg_id;

    if (srv6_prefix_agg_id_table_.find(nhg) != srv6_prefix_agg_id_table_.end()) {
        agg_id = srv6_prefix_agg_id_table_[nhg].prefix_agg_id;
        SWSS_LOG_INFO("Agg id already exist, agg_id_key: %s, agg_id %u", nhg.to_string().c_str(), agg_id);
    } else {
        while (srv6_prefix_agg_id_set_.find(g_agg_id) != srv6_prefix_agg_id_set_.end()) {
            SWSS_LOG_INFO("Agg id %d is busy, try next", g_agg_id);
            g_agg_id++;
            // restart with 1 if flip
            if (g_agg_id == 0) {
                g_agg_id = 1;
            }
        }
        agg_id = g_agg_id;
        srv6_prefix_agg_id_table_[nhg].prefix_agg_id = g_agg_id;
        // initialize ref_count with 0, will be added in increasePrefixAggIdRefCount() later
        srv6_prefix_agg_id_table_[nhg].ref_count = 0;
        srv6_prefix_agg_id_set_.insert(g_agg_id);
        SWSS_LOG_INFO("Agg id not exist, create agg_id_key: %s, agg_id %u", nhg.to_string().c_str(), agg_id);
    }

    return agg_id;
}

uint32_t Srv6Orch::getAggId(const std::string& index)
{
    SWSS_LOG_ENTER();
    static uint32_t g_agg_id = 1;
    uint32_t agg_id;

    if (srv6_prefix_agg_id_table_for_nhg_.find(index) != srv6_prefix_agg_id_table_for_nhg_.end()) {
        agg_id = srv6_prefix_agg_id_table_for_nhg_[index].prefix_agg_id;
        SWSS_LOG_INFO("Agg id already exist, agg_id_key: %s, agg_id %u", index.c_str(), agg_id);
    } else {
        while (srv6_prefix_agg_id_set_.find(g_agg_id) != srv6_prefix_agg_id_set_.end()) {
            SWSS_LOG_INFO("Agg id %d is busy, try next", g_agg_id);
            g_agg_id++;
            // restart with 1 if flip
            if (g_agg_id == 0) {
                g_agg_id = 1;
            }
        }
        agg_id = g_agg_id;
        srv6_prefix_agg_id_table_for_nhg_[index].prefix_agg_id = g_agg_id;
        // initialize ref_count with 0, will be added in increasePrefixAggIdRefCount() later
        srv6_prefix_agg_id_table_for_nhg_[index].ref_count = 0;
        srv6_prefix_agg_id_set_.insert(g_agg_id);
        SWSS_LOG_INFO("Agg id not exist, create agg_id_key: %s, agg_id %u", index.c_str(), agg_id);
    }

    return agg_id;
}

void Srv6Orch::deleteAggId(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();
    uint32_t agg_id;

    if (srv6_prefix_agg_id_table_.find(nhg) == srv6_prefix_agg_id_table_.end()) {
        return;
    }

    agg_id = srv6_prefix_agg_id_table_[nhg].prefix_agg_id;
    if (srv6_prefix_agg_id_table_[nhg].ref_count == 0) {
        srv6_prefix_agg_id_table_.erase(nhg);
        srv6_prefix_agg_id_set_.erase(agg_id);
        SWSS_LOG_INFO("Delete Agg id %d, agg_id_key: %s", agg_id, nhg.to_string().c_str());
    }
    else
    {
        SWSS_LOG_INFO("Referencing this prefix agg id %u : %u", agg_id, srv6_prefix_agg_id_table_[nhg].ref_count);
    }
}

void Srv6Orch::deleteAggId(const std::string& index)
{
    SWSS_LOG_ENTER();
    uint32_t agg_id;

    if (srv6_prefix_agg_id_table_for_nhg_.find(index) == srv6_prefix_agg_id_table_for_nhg_.end()) {
        return;
    }

    agg_id = srv6_prefix_agg_id_table_for_nhg_[index].prefix_agg_id;
    if (srv6_prefix_agg_id_table_for_nhg_[index].ref_count == 0) {
        srv6_prefix_agg_id_table_for_nhg_.erase(index);
        srv6_prefix_agg_id_set_.erase(agg_id);
        SWSS_LOG_INFO("Delete Agg id %d, agg_id_key: %s", agg_id, index.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Referencing this prefix agg id %u : %u", agg_id, srv6_prefix_agg_id_table_for_nhg_[index].ref_count);
    }
}

void Srv6Orch::increasePicContextIdRefCount(const std::string &index)
{
    SWSS_LOG_ENTER();
    if (srv6_pic_context_table_.find(index) == srv6_pic_context_table_.end())
        SWSS_LOG_ERROR("Unexpected refcount increase for context id %s", index.c_str());
    else
        ++srv6_pic_context_table_[index].ref_count;
}

void Srv6Orch::decreasePicContextIdRefCount(const std::string &index)
{
    SWSS_LOG_ENTER();
    if (srv6_pic_context_table_.find(index) == srv6_pic_context_table_.end())
        SWSS_LOG_ERROR("Unexpected refcount decrease for context id %s", index.c_str());
    else
        --srv6_pic_context_table_[index].ref_count;

    if (srv6_pic_context_table_[index].ref_count == 0) {
        notifyRetry(this, APP_PIC_CONTEXT_TABLE_NAME, make_constraint(RETRY_CST_PIC_REF, index));
    }
}

void Srv6Orch::increasePrefixAggIdRefCount(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();
    if (srv6_prefix_agg_id_table_.find(nhg) == srv6_prefix_agg_id_table_.end())
    {
        SWSS_LOG_ERROR("Unexpected prefix agg refcount increase for nexthop %s", nhg.to_string().c_str());
    }
    else
    {
        srv6_prefix_agg_id_table_[nhg].ref_count++;
    }
}

void Srv6Orch::increasePrefixAggIdRefCount(const std::string& index)
{
    SWSS_LOG_ENTER();
    if (srv6_prefix_agg_id_table_for_nhg_.find(index) == srv6_prefix_agg_id_table_for_nhg_.end())
    {
        SWSS_LOG_ERROR("Unexpected prefix agg refcount increase for nexthop %s", index.c_str());
    }
    else
    {
        ++srv6_prefix_agg_id_table_for_nhg_[index].ref_count;
    }
}

void Srv6Orch::decreasePrefixAggIdRefCount(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();
    if (srv6_prefix_agg_id_table_.find(nhg) == srv6_prefix_agg_id_table_.end())
    {
        SWSS_LOG_ERROR("Unexpected prefix agg refcount decrease for nexthop %s", nhg.to_string().c_str());
    }
    else
    {
        srv6_prefix_agg_id_table_[nhg].ref_count--;
    }
}

void Srv6Orch::decreasePrefixAggIdRefCount(const std::string& index)
{
    SWSS_LOG_ENTER();
    if (srv6_prefix_agg_id_table_for_nhg_.find(index) == srv6_prefix_agg_id_table_for_nhg_.end())
    {
        SWSS_LOG_ERROR("Unexpected prefix agg refcount decrease for nexthop %s", index.c_str());
    }
    else
    {
        --srv6_prefix_agg_id_table_for_nhg_[index].ref_count;
    }
}

bool Srv6Orch::srv6P2pTunnelExists(const std::string &endpoint)
{
    if (srv6_p2p_tunnel_table_.find(endpoint) != srv6_p2p_tunnel_table_.end())
    {
        return true;
    }
    return false;
}

bool Srv6Orch::createSrv6P2pTunnel(const std::string &src, const std::string &endpoint)
{
    SWSS_LOG_ENTER();
    sai_status_t saistatus;
    sai_object_id_t srv6_tunnel_map_id;

    sai_attribute_t tunnel_map_attr;
    vector<sai_attribute_t> tunnel_map_attrs;

    if (srv6P2pTunnelExists(endpoint)) {
        return true;
    }

    // 0. create tunnel map
    tunnel_map_attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
    tunnel_map_attr.value.u32 = SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID;
    tunnel_map_attrs.push_back(tunnel_map_attr);

    saistatus = sai_tunnel_api->create_tunnel_map(&srv6_tunnel_map_id, gSwitchId,
        (uint32_t)tunnel_map_attrs.size(), tunnel_map_attrs.data());
    if (saistatus != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to create srv6 p2p tunnel map for src_ip: %s dst_ip: %s", src.c_str(), endpoint.c_str());
        return false;
    }

    // 1. create tunnel
    sai_object_id_t tunnel_id;
    sai_attribute_t tunnel_attr;
    vector<sai_attribute_t> tunnel_attrs;
    sai_ip_address_t ipaddr;

    tunnel_attr.id = SAI_TUNNEL_ATTR_TYPE;
    tunnel_attr.value.s32 = SAI_TUNNEL_TYPE_SRV6;
    tunnel_attrs.push_back(tunnel_attr);

    IpAddress src_ip(src);
    ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(ipaddr.addr.ip6, src_ip.getV6Addr(), sizeof(ipaddr.addr.ip6));
    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    tunnel_attr.value.ipaddr = ipaddr;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    tunnel_attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(tunnel_attr);

    sai_object_id_t tunnel_map_list[1];
    tunnel_map_list[0] = srv6_tunnel_map_id;
    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
    tunnel_attr.value.objlist.count = 1;
    tunnel_attr.value.objlist.list = tunnel_map_list;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
    tunnel_attr.value.u32 = SAI_TUNNEL_PEER_MODE_P2P;
    tunnel_attrs.push_back(tunnel_attr);

    IpAddress dst_ip(endpoint);
    ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memcpy(ipaddr.addr.ip6, dst_ip.getV6Addr(), sizeof(ipaddr.addr.ip6));
    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
    tunnel_attr.value.ipaddr = ipaddr;
    tunnel_attrs.push_back(tunnel_attr);

    saistatus = sai_tunnel_api->create_tunnel(
        &tunnel_id, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (saistatus != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create srv6 p2p tunnel for src ip: %s, dst ip: %s",
            src.c_str(), endpoint.c_str());

        sai_tunnel_api->remove_tunnel_map(srv6_tunnel_map_id);
        return false;
    }

    srv6_p2p_tunnel_table_[endpoint].tunnel_id = tunnel_id;
    srv6_p2p_tunnel_table_[endpoint].tunnel_map_id = srv6_tunnel_map_id;
    return true;
}

bool Srv6Orch::deleteSrv6P2pTunnel(const std::string &endpoint)
{
    if (srv6_p2p_tunnel_table_.find(endpoint) == srv6_p2p_tunnel_table_.end())
    {
        return true;
    }

    if (srv6P2pTunnelNexthopSize(endpoint) || srv6P2pTunnelEntrySize(endpoint))
    {
        SWSS_LOG_INFO("There are still SRv6 VPNs or Nexthops referencing this srv6 p2p tunnel object dst %s", endpoint.c_str());
        return true;
    }

    sai_status_t status;

    // 0. remove tunnel
    status = sai_tunnel_api->remove_tunnel(srv6_p2p_tunnel_table_[endpoint].tunnel_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove SRV6 p2p tunnel object for dst_ip: %s", endpoint.c_str());
        return false;
    }

    // 1. remove tunnel map
    status = sai_tunnel_api->remove_tunnel_map(srv6_p2p_tunnel_table_[endpoint].tunnel_map_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove SRV6 tunnel map object for dst_ip: %s", endpoint.c_str());
        return false;
    }

    srv6_p2p_tunnel_table_.erase(endpoint);
    return true;
}

void Srv6Orch::srv6P2ptunnelUpdateNexthops(const NextHopKey &nhkey, bool insert)
{
    if (insert)
    {
        srv6_p2p_tunnel_table_[nhkey.ip_address.to_string()].nexthops.insert(nhkey);
    }
    else
    {
        srv6_p2p_tunnel_table_[nhkey.ip_address.to_string()].nexthops.erase(nhkey);
    }
}

size_t Srv6Orch::srv6P2pTunnelNexthopSize(const std::string &endpoint)
{
    return srv6_p2p_tunnel_table_[endpoint].nexthops.size();
}

void Srv6Orch::srv6P2pTunnelUpdateEntries(const Srv6TunnelMapEntryKey &tmek, bool insert)
{
    if (insert)
        srv6_p2p_tunnel_table_[tmek.endpoint].tunnel_map_entries.insert(tmek);
    else
        srv6_p2p_tunnel_table_[tmek.endpoint].tunnel_map_entries.erase(tmek);
}

size_t Srv6Orch::srv6P2pTunnelEntrySize(const std::string &endpoint)
{
    return srv6_p2p_tunnel_table_[endpoint].tunnel_map_entries.size();
}

bool Srv6Orch::createSrv6Vpns(const Srv6PicContextInfo &pci, const std::string &context_id)
{
    auto agg_id = getAggId(context_id);
    for (size_t i = 0; i < pci.nexthops.size(); ++i)
    {
        if (!createSrv6Vpn(pci.nexthops[i], pci.sids[i], agg_id))
        {
            for (size_t j = 0; j < i; ++j)
            {
                deleteSrv6Vpn(pci.nexthops[j], pci.sids[j], agg_id);
            }
            deleteAggId(context_id);
            return false;
        }
    }

    increasePrefixAggIdRefCount(context_id);

    return true;
}

bool Srv6Orch::createSrv6Vpn(const std::string &endpoint, const std::string &sid, const uint32_t prefix_agg_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    Srv6TunnelMapEntryKey tmek;
    tmek.endpoint = endpoint;
    tmek.vpn_sid = sid;
    tmek.prefix_agg_id = prefix_agg_id;

    if (srv6_tunnel_map_entry_table_.find(tmek) != srv6_tunnel_map_entry_table_.end())
    {
        srv6_tunnel_map_entry_table_[tmek].ref_count++;
        return true;
    }

    if (srv6_p2p_tunnel_table_.find(endpoint) == srv6_p2p_tunnel_table_.end())
    {
        SWSS_LOG_ERROR("Tunnel map for endpoint %s does not exist", endpoint.c_str());
        return false;
    }
    sai_object_id_t tunnel_map_id = srv6_p2p_tunnel_table_[endpoint].tunnel_map_id;

    // 1. create vpn tunnel_map entry
    sai_attribute_t tunnel_map_entry_attr;
    vector<sai_attribute_t> tunnel_map_entry_attrs;
    sai_object_id_t tunnel_entry_id;

    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    tunnel_map_entry_attr.value.u32 = SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID;
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    tunnel_map_entry_attr.value.oid = tunnel_map_id;
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_PREFIX_AGG_ID_KEY;
    tunnel_map_entry_attr.value.u32 = tmek.prefix_agg_id;
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    IpAddress vpn_sid(tmek.vpn_sid);
    tunnel_map_entry_attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_SRV6_VPN_SID_VALUE;
    memcpy(tunnel_map_entry_attr.value.ip6, vpn_sid.getV6Addr(), sizeof(sai_ip6_t));
    tunnel_map_entry_attrs.push_back(tunnel_map_entry_attr);

    status = sai_tunnel_api->create_tunnel_map_entry(&tunnel_entry_id, gSwitchId,
                                                (uint32_t)tunnel_map_entry_attrs.size(),
                                                tunnel_map_entry_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create vpn tunnel_map entry for vpn_sid: %s", tmek.vpn_sid.c_str());
        return false;
    }

    // add reference for tunnel map entry
    srv6_tunnel_map_entry_table_[tmek].tunnel_map_entry_id = tunnel_entry_id;
    srv6_tunnel_map_entry_table_[tmek].ref_count = 1;

    srv6P2pTunnelUpdateEntries(tmek, true);
    return true;
}

bool Srv6Orch::deleteSrv6Vpns(const std::string &context_id)
{
    const auto &it = srv6_pic_context_table_.find(context_id);
    if (it == srv6_pic_context_table_.end())
    {
        SWSS_LOG_ERROR("Failed to find context id %s", context_id.c_str());
        return false;
    }

    bool success = true;
    auto agg_id = getAggId(context_id);
    for (size_t i = 0; i < it->second.nexthops.size(); ++i)
    {
        if (!deleteSrv6Vpn(it->second.nexthops[i], it->second.sids[i], agg_id))
        {
            success = false;
        }
    }

    if (success)
    {
        decreasePrefixAggIdRefCount(context_id);
    }
    deleteAggId(context_id);

    return success;
}

bool Srv6Orch::deleteSrv6Vpn(const std::string &endpoint, const std::string &sid, const uint32_t prefix_agg_id)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    // 1. remove tunnel_map entry if need
    sai_object_id_t tunnel_entry_id;

    Srv6TunnelMapEntryKey tmek;
    tmek.endpoint = endpoint;
    tmek.vpn_sid = sid;
    tmek.prefix_agg_id = prefix_agg_id;

    if (srv6_tunnel_map_entry_table_.find(tmek) == srv6_tunnel_map_entry_table_.end())
    {
        return true;
    }

    srv6_tunnel_map_entry_table_[tmek].ref_count--;
    if (srv6_tunnel_map_entry_table_[tmek].ref_count == 0)
    {
        tunnel_entry_id = srv6_tunnel_map_entry_table_[tmek].tunnel_map_entry_id;
        status = sai_tunnel_api->remove_tunnel_map_entry(tunnel_entry_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove nexthop tunnel map entry (endpoint: %s, sid: %s, agg_id: %u)",
                                tmek.endpoint.c_str(), tmek.vpn_sid.c_str(), tmek.prefix_agg_id);
            return false;
        }
        srv6_tunnel_map_entry_table_.erase(tmek);

        srv6P2pTunnelUpdateEntries(tmek, false);
        if (!deleteSrv6P2pTunnel(tmek.endpoint))
        {
            SWSS_LOG_ERROR("Failed to remove SRV6 p2p tunnel object for dst %s,", endpoint.c_str());
            return false;
        }
    }
    else
    {
        SWSS_LOG_INFO("Nexthops referencing this tunnel map entry endpoint %s, vpn_sid %s, prefix_agg_id %u : %u",
            tmek.endpoint.c_str(),
            tmek.vpn_sid.c_str(),
            tmek.prefix_agg_id,
            srv6_tunnel_map_entry_table_[tmek].ref_count);
    }
    return true;
}

Srv6Orch::MySidTaskResult Srv6Orch::doTaskMySidTable(const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(tuple);
    string end_action, dt_vrf, adj;

    /* Key for mySid : block_len:node_len:function_len:args_len:sid-ip */
    string keyString = kfvKey(tuple);

    for (auto i : kfvFieldsValues(tuple))
    {
        if (fvField(i) == "action")
        {
          end_action = fvValue(i);
        }
        if(fvField(i) == "vrf")
        {
          dt_vrf = fvValue(i);
        }
        if(fvField(i) == "adj")
        {
          adj = fvValue(i);
        }
    }
    if (op == SET_COMMAND)
    {
        sai_my_sid_entry_endpoint_behavior_t end_behavior;
        sai_my_sid_entry_endpoint_behavior_flavor_t end_flavor;
        if (!sidEntryEndpointBehavior(end_action, end_behavior, end_flavor))
        {
            SWSS_LOG_ERROR("Invalid my_sid action %s for sid %s", end_action.c_str(), keyString.c_str());
            return {task_invalid_entry, boost::none};
        }

        if (mySidVrfRequired(end_behavior))
        {
            if (dt_vrf.empty())
            {
                SWSS_LOG_ERROR("Missing VRF for my_sid entry %s", keyString.c_str());
                return {task_invalid_entry, boost::none};
            }
            if (dt_vrf != "default" && !m_vrfOrch->isVRFexists(dt_vrf))
            {
                return {task_need_retry, make_constraint(RETRY_CST_MYSID_VRF, dt_vrf)};
            }
        }

        if (mySidNextHopRequired(end_behavior))
        {
            if (adj.empty())
            {
                SWSS_LOG_ERROR("Missing adjacency for my_sid entry %s", keyString.c_str());
                return {task_invalid_entry, boost::none};
            }

            NextHopKey nexthop(adj);
            if (!m_neighOrch->hasNextHop(nexthop) ||
                m_neighOrch->getNextHopId(nexthop) == SAI_NULL_OBJECT_ID)
            {
                return {task_need_retry,
                        make_constraint(RETRY_CST_MYSID_NEXTHOP, nexthop.to_string())};
            }
        }

                auto status = createUpdateMysidEntry(keyString, dt_vrf, adj, end_action);
                if (status != task_success)
        {
          SWSS_LOG_ERROR("Failed to create/update my_sid entry for sid %s", keyString.c_str());
                    return {status, boost::none};
        }
    }
    else if(op == DEL_COMMAND)
    {
                auto status = deleteMysidEntry(keyString);
                if (status != task_success)
        {
          SWSS_LOG_ERROR("Failed to delete my_sid entry for sid %s", keyString.c_str());
                    return {status, boost::none};
        }
    }
    else
    {
        SWSS_LOG_ERROR("Invalid command %s for my_sid entry %s", op.c_str(), keyString.c_str());
        return {task_invalid_entry, boost::none};
    }

    return {task_success, boost::none};
}

void Srv6Orch::doTaskCfgMySidTable(const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();

    auto op = kfvOp(tuple);
    auto key = kfvKey(tuple);
    auto& fvs = kfvFieldsValues(tuple);

    if (op == SET_COMMAND)
    {
        addMySidCfgCacheEntry(key, fvs);
    }
    else if (op == DEL_COMMAND)
    {
        removeMySidCfgCacheEntry(key);
    }
    else
    {
        SWSS_LOG_ERROR("Unexpected command");
    }
}

task_process_status Srv6Orch::doTaskPicContextTable(const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(tuple);
    string key = kfvKey(tuple);
    const auto &it = srv6_pic_context_table_.find(key);
    if (op == SET_COMMAND)
    {
        if (it != srv6_pic_context_table_.end())
        {
            SWSS_LOG_ERROR("update is not allowed for pic context table");
            return task_duplicated;
        }
        Srv6PicContextInfo pci;
        pci.ref_count = 0;
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "nexthop" && fvValue(i) != "")
            {
                pci.nexthops = tokenize(fvValue(i), ',');
            }
            else if (fvField(i) == "vpn_sid" && fvValue(i) != "")
            {
                pci.sids = tokenize(fvValue(i), ',');
            }
        }
        if (pci.nexthops.size() != pci.sids.size())
        {
            SWSS_LOG_ERROR("inconsistent number of endpoints(%zu) and vpn sids(%zu)",
                                pci.nexthops.size(), pci.sids.size());
            return task_failed;
        }

        if (!createSrv6Vpns(pci ,key))
        {
            SWSS_LOG_ERROR("Failed to create SRv6 VPNs for context id %s", key.c_str());
            return task_need_retry;
        }

        srv6_pic_context_table_[key] = pci;
        notifyRetry(gRouteOrch, APP_ROUTE_TABLE_NAME, make_constraint(RETRY_CST_PIC, key));
    }
    else if (op == DEL_COMMAND)
    {
        if (it == srv6_pic_context_table_.end())
        {
            SWSS_LOG_INFO("Unable to find pic context entry for key %s", key.c_str());
            return task_ignore;
        }
        else if (it->second.ref_count != 0)
        {
            if (addToRetry(APP_PIC_CONTEXT_TABLE_NAME, Task(tuple), make_constraint(RETRY_CST_PIC_REF, key)))
            {
                return task_ignore;
            }
            SWSS_LOG_INFO("Unable to delete context id %s, because it is referenced %u times", key.c_str(), it->second.ref_count);
            return task_need_retry;
        }
        else if (!deleteSrv6Vpns(key))
        {
            SWSS_LOG_ERROR("Failed to delete SRv6 VPNs for context id %s", key.c_str());
            return task_need_retry;
        }
        srv6_pic_context_table_.erase(it);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_ignore;
    }
    return task_success;
}

bool Srv6Orch::contextIdExists(const std::string &context_id)
{
    if (srv6_pic_context_table_.find(context_id) == srv6_pic_context_table_.end())
        return false;
    return true;
}

void Srv6Orch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    task_process_status status;
    const string &table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while(it != consumer.m_toSync.end())
    {
        auto t = it->second;
        SWSS_LOG_INFO("table name : %s",table_name.c_str());
        if (table_name == APP_SRV6_SID_LIST_TABLE_NAME)
        {
            status = doTaskSidTable(t);
            if (status == task_process_status::task_need_retry)
            {
                it++;
                continue;
            }
        }
        else if (table_name == APP_SRV6_MY_SID_TABLE_NAME)
        {
            auto result = doTaskMySidTable(t);
            if (result.dependency &&
                addToRetry(APP_SRV6_MY_SID_TABLE_NAME, Task(t), *result.dependency))
            {
                consumer.m_toSync.erase(it++);
                continue;
            }
            WarmStart::WarmStartState warm_state = WarmStart::RECONCILED;
            WarmStart::getWarmStartState("orchagent", warm_state);
            bool warm_restore_in_progress = WarmStart::isWarmStart() &&
                                            warm_state == WarmStart::INITIALIZED;
            if (result.status == task_need_retry ||
                ((result.status == task_failed || result.status == task_invalid_entry) &&
                 warm_restore_in_progress))
            {
                ++it;
                continue;
            }
        }
        else if (table_name == APP_PIC_CONTEXT_TABLE_NAME)
        {
            status = doTaskPicContextTable(t);
            if (status == task_need_retry)
            {
                ++it;
                continue;
            }
        }
        else if (table_name == CFG_SRV6_MY_SID_TABLE_NAME)
        {
            doTaskCfgMySidTable(t);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown table : %s",table_name.c_str());
        }
        consumer.m_toSync.erase(it++);
    }
}
