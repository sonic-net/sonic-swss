#include <string.h>
#include <inttypes.h>
#include "tunneldecaporch.h"
#include "portsorch.h"
#include "crmorch.h"
#include "logger.h"
#include "swssnet.h"
#include "qosorch.h"
#include "subscriberstatetable.h"

using namespace std;
using namespace swss;

#define OVERLAY_RIF_DEFAULT_MTU 9100
#define APPEND_IF_NOT_EMPTY(vec, obj, attr) \
    if (!obj.attr.empty())                  \
    vec.push_back({#attr, obj.attr})        \

extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_next_hop_api_t* sai_next_hop_api;

extern sai_object_id_t  gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;
extern CrmOrch*         gCrmOrch;
extern QosOrch*         gQosOrch;

TunnelDecapOrch::TunnelDecapOrch(
    DBConnector *appDb, DBConnector *stateDb,
    DBConnector *configDb, const vector<string> &tableNames)
    : Orch(appDb, tableNames),
      stateTunnelDecapTable(make_unique<Table>(stateDb, STATE_TUNNEL_DECAP_TABLE_NAME)),
      stateTunnelDecapTermTable(make_unique<Table>(stateDb, STATE_TUNNEL_DECAP_TERM_TABLE_NAME))
{
    SWSS_LOG_ENTER();

    auto cfgSubnetDecapSubTable = new SubscriberStateTable(configDb, CFG_SUBNET_DECAP_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0);
    deque<KeyOpFieldsValuesTuple> entries;
    cfgSubnetDecapSubTable->pops(entries);
    // init subnet decap config
    for (auto &entry : entries)
    {
        doSubnetDecapTask(entry);
    }

    Orch::addExecutor(new Consumer(cfgSubnetDecapSubTable, this, CFG_SUBNET_DECAP_TABLE_NAME));
}

void TunnelDecapOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    string table_name = consumer.getTableName();
    if (table_name == APP_TUNNEL_DECAP_TABLE_NAME)
    {
        doDecapTunnelTask(consumer);
    }
    else if (table_name == APP_TUNNEL_DECAP_TERM_TABLE_NAME)
    {
        doDecapTunnelTermTask(consumer);
    }
    else if (table_name == CFG_SUBNET_DECAP_TABLE_NAME)
    {
        doSubnetDecapTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Invalid table %s", table_name.c_str());
    }

    return;
}

void TunnelDecapOrch::doDecapTunnelTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        IpAddress src_ip;
        IpAddress* p_src_ip = nullptr;
        string tunnel_type;
        string dscp_mode;
        string ecn_mode;
        string encap_ecn_mode;
        string ttl_mode;
        sai_object_id_t dscp_to_tc_map_id = SAI_NULL_OBJECT_ID;
        sai_object_id_t tc_to_pg_map_id = SAI_NULL_OBJECT_ID;
        // The tc_to_dscp_map_id and tc_to_queue_map_id are parsed here for muxorch to retrieve
        sai_object_id_t tc_to_dscp_map_id = SAI_NULL_OBJECT_ID;
        sai_object_id_t tc_to_queue_map_id = SAI_NULL_OBJECT_ID;

        bool valid = true;
        task_process_status task_status = task_process_status::task_success;

        sai_object_id_t tunnel_id = SAI_NULL_OBJECT_ID;

        // checking to see if the tunnel already exists
        bool exists = (tunnelTable.find(key) != tunnelTable.end());
        if (exists)
        {
            tunnel_id = tunnelTable[key].tunnel_id;
        }

        if (op == SET_COMMAND)
        {

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "tunnel_type")
                {
                    tunnel_type = fvValue(i);
                    if (tunnel_type != "IPINIP")
                    {
                        SWSS_LOG_ERROR("Invalid tunnel type %s", tunnel_type.c_str());
                        valid = false;
                        break;
                    }
                }
                else if (fvField(i) == "src_ip")
                {
                    try
                    {
                        src_ip = IpAddress(fvValue(i));
                        p_src_ip = &src_ip;
                    }
                    catch (const std::invalid_argument &e)
                    {
                        SWSS_LOG_ERROR("%s", e.what());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        SWSS_LOG_ERROR("cannot modify src ip for existing tunnel");
                    }
                }
                else if (fvField(i) == "dscp_mode")
                {
                    dscp_mode = fvValue(i);
                    if (dscp_mode != "uniform" && dscp_mode != "pipe")
                    {
                        SWSS_LOG_ERROR("Invalid dscp mode %s\n", dscp_mode.c_str());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        setTunnelAttribute(fvField(i), dscp_mode, tunnel_id);
                        tunnelTable[key].dscp_mode = dscp_mode;
                    }
                }
                else if (fvField(i) == "ecn_mode")
                {
                    ecn_mode = fvValue(i);
                    if (ecn_mode != "copy_from_outer" && ecn_mode != "standard")
                    {
                        SWSS_LOG_ERROR("Invalid ecn mode %s\n", ecn_mode.c_str());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        SWSS_LOG_NOTICE("Skip setting ecn_mode since the SAI attribute SAI_TUNNEL_ATTR_DECAP_ECN_MODE is create only");
                        valid = false;
                        break;
                    }
                }
                else if (fvField(i) == "encap_ecn_mode")
                {
                    encap_ecn_mode = fvValue(i);
                    if (encap_ecn_mode != "standard")
                    {
                        SWSS_LOG_ERROR("Only standard encap ecn mode is supported currently %s\n", ecn_mode.c_str());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        SWSS_LOG_NOTICE("Skip setting encap_ecn_mode since the SAI attribute SAI_TUNNEL_ATTR_ENCAP_ECN_MODE is create only");
                        valid = false;
                        break;
                    }
                }
                else if (fvField(i) == "ttl_mode")
                {
                    ttl_mode = fvValue(i);
                    if (ttl_mode != "uniform" && ttl_mode != "pipe")
                    {
                        SWSS_LOG_ERROR("Invalid ttl mode %s\n", ttl_mode.c_str());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        setTunnelAttribute(fvField(i), ttl_mode, tunnel_id);
                    }
                }
                else if (fvField(i) == decap_dscp_to_tc_field_name)
                {
                    dscp_to_tc_map_id = gQosOrch->resolveTunnelQosMap(table_name, key, decap_dscp_to_tc_field_name, t);
                    if (dscp_to_tc_map_id == SAI_NULL_OBJECT_ID)
                    {
                        SWSS_LOG_NOTICE("QoS map %s is not ready yet", decap_dscp_to_tc_field_name.c_str());
                        task_status = task_process_status::task_need_retry;
                        break;
                    }
                    if (exists)
                    {
                        setTunnelAttribute(fvField(i), dscp_to_tc_map_id, tunnel_id);
                    }
                }
                else if (fvField(i) == decap_tc_to_pg_field_name)
                {
                    tc_to_pg_map_id = gQosOrch->resolveTunnelQosMap(table_name, key, decap_tc_to_pg_field_name, t);
                    if (tc_to_pg_map_id == SAI_NULL_OBJECT_ID)
                    {
                        SWSS_LOG_NOTICE("QoS map %s is not ready yet", decap_tc_to_pg_field_name.c_str());
                        task_status = task_process_status::task_need_retry;
                        break;
                    }
                    if (exists)
                    {
                        setTunnelAttribute(fvField(i), tc_to_pg_map_id, tunnel_id);
                    }
                }
                else if (fvField(i) == encap_tc_to_dscp_field_name)
                {
                    tc_to_dscp_map_id = gQosOrch->resolveTunnelQosMap(table_name, key, encap_tc_to_dscp_field_name, t);
                    if (tc_to_dscp_map_id == SAI_NULL_OBJECT_ID)
                    {
                        SWSS_LOG_NOTICE("QoS map %s is not ready yet", encap_tc_to_dscp_field_name.c_str());
                        task_status = task_process_status::task_need_retry;
                        break;
                    }
                    if (exists)
                    {
                        // Record only
                        tunnelTable[key].encap_tc_to_dscp_map_id = tc_to_dscp_map_id;
                    }
                }
                else if (fvField(i) == encap_tc_to_queue_field_name)
                {
                    tc_to_queue_map_id = gQosOrch->resolveTunnelQosMap(table_name, key, encap_tc_to_queue_field_name, t);
                    if (tc_to_queue_map_id == SAI_NULL_OBJECT_ID)
                    {
                        SWSS_LOG_NOTICE("QoS map %s is not ready yet", encap_tc_to_queue_field_name.c_str());
                        task_status = task_process_status::task_need_retry;
                        break;
                    }
                    if (exists)
                    {
                        // Record only
                        tunnelTable[key].encap_tc_to_queue_map_id = tc_to_queue_map_id;
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("unknown decap tunnel table attribute '%s'.", fvField(i).c_str());
                    valid = false;
                    break;
                }
            }

            if (task_status == task_process_status::task_need_retry)
            {
                ++it;
                continue;
            }

            //create new tunnel if it doesn't exists already
            if (valid && !exists)
            {

                if (addDecapTunnel(key, tunnel_type, p_src_ip, dscp_mode, ecn_mode, encap_ecn_mode, ttl_mode,
                                   dscp_to_tc_map_id, tc_to_pg_map_id))
                {
                    // Record only
                    tunnelTable[key].encap_tc_to_dscp_map_id = tc_to_dscp_map_id;
                    tunnelTable[key].encap_tc_to_queue_map_id = tc_to_queue_map_id;
                    SWSS_LOG_NOTICE("Tunnel %s added to ASIC_DB.", key.c_str());

                    // process unhandled decap terms
                    processUnhandledDecapTunnelTerms(key);
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to add tunnel %s to ASIC_DB.", key.c_str());
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (exists)
            {
                decreaseTunnelRefCount(key);
                RemoveTunnelIfNotReferenced(key);
            }
            else
            {
                SWSS_LOG_ERROR("Tunnel %s cannot be removed since it doesn't exist.", key.c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s.", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

void TunnelDecapOrch::doDecapTunnelTermTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    static const map<string, TunnelTermType> DecapTermTypes = {
        {"P2P", TUNNEL_TERM_TYPE_P2P},
        {"P2MP", TUNNEL_TERM_TYPE_P2MP},
        {"MP2MP", TUNNEL_TERM_TYPE_MP2MP}};

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple &t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        string tunnel_name;
        string dst_ip_str;
        string src_ip_str;
        IpPrefix dst_ip;
        IpPrefix src_ip;
        TunnelTermType term_type = TUNNEL_TERM_TYPE_P2MP;
        string subnet_type;
        bool valid = true;

        size_t found = key.find(DEFAULT_KEY_SEPARATOR);
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("%s: invalid tunnel decap term key %s.", key.c_str(), key.c_str());
            valid = false;
        }
        else
        {
            tunnel_name = key.substr(0, found);
            dst_ip_str = key.substr(found + 1);
            try
            {
                dst_ip = IpPrefix(dst_ip_str);
            }
            catch (const std::invalid_argument &e)
            {
                SWSS_LOG_ERROR("%s: invalid destination IP prefix %s.", key.c_str(), e.what());
                valid = false;
            }
        }

        if (!valid)
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        bool tunnel_exists = (tunnelTable.find(tunnel_name) != tunnelTable.end());
        bool is_subnet_decap_term = (tunnel_name == subnetDecapConfig.tunnel ||
                                     tunnel_name == subnetDecapConfig.tunnel_v6);
        bool is_v4_term = dst_ip.isV4();

        if (op == SET_COMMAND)
        {
            for (auto &fv : kfvFieldsValues(t))
            {
                if (fvField(fv) == "src_ip")
                {
                    src_ip_str = fvValue(fv);
                    try
                    {
                        src_ip = IpPrefix(src_ip_str);
                    }
                    catch (const std::invalid_argument &e)
                    {
                        SWSS_LOG_ERROR("%s: invalid source IP prefix %s.", key.c_str(), src_ip_str.c_str());
                        valid = false;
                        break;
                    }
                }
                else if (fvField(fv) == "term_type")
                {
                    auto it = DecapTermTypes.find(fvValue(fv));
                    if (it == DecapTermTypes.end())
                    {
                        SWSS_LOG_ERROR("%s: invalid tunnel decap term type %s.", key.c_str(), fvValue(fv).c_str());
                        valid = false;
                        break;
                    }
                    term_type = it->second;
                }
                else if (fvField(fv) == "subnet_type")
                {
                    subnet_type = fvValue(fv);
                    if (subnet_type != "vlan" && subnet_type != "vip")
                    {
                        SWSS_LOG_ERROR("%s: invalid subnet type: %s.", key.c_str(), subnet_type.c_str());
                        valid = false;
                        break;
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("%s: unknown decap term table attribute '%s'", key.c_str(), fvField(fv).c_str());
                    valid = false;
                    break;
                }
            }

            if (valid)
            {
                if (is_subnet_decap_term && term_type != TUNNEL_TERM_TYPE_MP2MP)
                {
                    SWSS_LOG_ERROR("%s: only MP2MP tunnel decap term is allowed for subnet decap tunnel.", key.c_str());
                    valid = false;
                }
                else if (!subnet_type.empty() && term_type != TUNNEL_TERM_TYPE_MP2MP)
                {
                    SWSS_LOG_ERROR("%s: only MP2MP is allowed for subnet decap term.", key.c_str());
                    valid = false;
                }
                else if (term_type == TUNNEL_TERM_TYPE_P2P && src_ip_str.empty())
                {
                    SWSS_LOG_ERROR("%s: no source IP is provided.", key.c_str());
                    valid = false;
                }
                else if (term_type == TUNNEL_TERM_TYPE_MP2MP && !is_subnet_decap_term && src_ip_str.empty())
                {
                    SWSS_LOG_ERROR("%s: no source IP is provided.", key.c_str());
                    valid = false;
                }
            }

            if (valid)
            {
                // if subnet decap is enabled, take source IP from the subnet decap config
                // for subnet decap tunnnel term
                if (subnetDecapConfig.enable)
                {
                    if (is_subnet_decap_term)
                    {
                        if (is_v4_term)
                        {
                            if (!subnetDecapConfig.src_ip.empty())
                            {
                                src_ip_str = subnetDecapConfig.src_ip;
                            }
                            else
                            {
                                SWSS_LOG_ERROR("%s: source IP is not configured for subnet decap term, ignored.", key.c_str());
                                it = consumer.m_toSync.erase(it);
                                continue;
                            }
                        }
                        else
                        {
                            if (!subnetDecapConfig.src_ip_v6.empty())
                            {
                                src_ip_str = subnetDecapConfig.src_ip_v6;
                            }
                            else
                            {
                                SWSS_LOG_ERROR("%s: source IPv6 is not configured for subnet decap term, ignored.", key.c_str());
                                it = consumer.m_toSync.erase(it);
                                continue;
                            }
                        }
                    }
                }
                else if (is_subnet_decap_term)
                {
                    SWSS_LOG_ERROR("%s: subnet decap is disabled, ignored.", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                if (tunnel_exists)
                {
                    if (!addDecapTunnelTermEntry(tunnel_name, src_ip_str, dst_ip_str, term_type, subnet_type))
                    {
                        SWSS_LOG_ERROR("%s: failed to add tunnel decap term to ASIC_DB.", key.c_str());
                    }
                }
                else
                {
                    SWSS_LOG_NOTICE("%s: tunnel doesn't exist, added to unhandled list.", key.c_str());
                    addUnhandledDecapTunnelTerm(tunnel_name, src_ip_str, dst_ip_str, term_type, subnet_type);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (tunnel_exists)
            {
                if (removeDecapTunnelTermEntry(tunnel_name, dst_ip_str))
                {
                    RemoveTunnelIfNotReferenced(tunnel_name);
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to remove tunnel decap term %s from ASIC_DB.", key.c_str());
                }
            }
            else
            {
                SWSS_LOG_NOTICE("Tunnel for decap term %s doesn't exist, removed from unhandled list.", key.c_str());
                removeUnhandledDecapTunnelTerm(tunnel_name, dst_ip_str);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s.", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

void TunnelDecapOrch::doSubnetDecapTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple &t = it->second;
        doSubnetDecapTask(t);
        it = consumer.m_toSync.erase(it);
    }
}

void TunnelDecapOrch::doSubnetDecapTask(const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();

    string key = kfvKey(tuple);
    string op = kfvOp(tuple);

    bool valid = true;
    string src_ip_str;
    string src_ip_v6_str;
    IpPrefix src_ip{""};
    IpPrefix src_ip_v6{""};
    bool enable = false;

    if (op == SET_COMMAND)
    {
        for (auto &fv : kfvFieldsValues(tuple))
        {
            if (fvField(fv) == "src_ip")
            {
                src_ip_str = fvValue(fv);
                try
                {
                    src_ip = swss::IpPrefix(src_ip_str);
                }
                catch (const std::invalid_argument &e)
                {
                    SWSS_LOG_ERROR("Invalid source IP prefix %s.", src_ip_str.c_str());
                    valid = false;
                    break;
                }
                if (!src_ip.isV4())
                {
                    SWSS_LOG_ERROR("Invalid source IP prefix %s.", src_ip_str.c_str());
                    valid = false;
                    break;
                }
            }
            else if (fvField(fv) == "src_ip_v6")
            {
                src_ip_v6_str = fvValue(fv);
                try
                {
                    src_ip_v6 = swss::IpPrefix(src_ip_v6_str);
                }
                catch (const std::invalid_argument &e)
                {
                    SWSS_LOG_ERROR("Invalid source IPv6 prefix %s.", src_ip_v6_str.c_str());
                    valid = false;
                    break;
                }
                if (src_ip_v6.isV4())
                {
                    SWSS_LOG_ERROR("Invalid source IPv6 prefix %s.", src_ip_v6_str.c_str());
                    valid = false;
                    break;
                }
            }
            else if (fvField(fv) == "status")
            {
                enable = (fvValue(fv) == "enable");
            }
            else
            {
                SWSS_LOG_ERROR("unknown subnet decap table attribute '%s'.", fvField(fv).c_str());
                valid = false;
                break;
            }
        }

        if (src_ip_str.empty() && src_ip_v6_str.empty())
        {
            SWSS_LOG_ERROR("Both src_ip and src_ip_v6 of subnet decap are not set.");
            valid = false;
        }

        if (valid)
        {
            subnetDecapConfig.enable = enable;
            ostringstream oss;
            oss << "Updated subnet decap config, enable: " << enable;
            if (!src_ip_str.empty())
            {
                src_ip_str = src_ip.to_string();
                oss << " , src_ip: " << src_ip_str;
                if (subnetDecapConfig.src_ip.empty())
                {
                    subnetDecapConfig.src_ip = src_ip_str;
                }
                else if (subnetDecapConfig.src_ip != src_ip_str)
                {
                    if (subnetDecapConfig.enable)
                    {
                        // update source IP of existing IPv4 decap terms
                        setIpAttribute(subnetDecapConfig.tunnel, src_ip_str);
                        // update source IP of unhandled IPv4 decap terms
                        updateUnhandledDecapTunnelTerms(subnetDecapConfig.tunnel, src_ip_str);
                    }
                    subnetDecapConfig.src_ip = src_ip_str;
                }
            }
            if (!src_ip_v6_str.empty())
            {
                src_ip_v6_str = src_ip_v6.to_string();
                oss << " , src_ip_v6: " << src_ip_v6_str;
                if (subnetDecapConfig.src_ip_v6.empty())
                {
                    subnetDecapConfig.src_ip_v6 = src_ip_v6_str;
                }
                else if (subnetDecapConfig.src_ip_v6 != src_ip_v6_str)
                {
                    if (subnetDecapConfig.enable)
                    {
                        // update source IP of existing IPv6 decap terms
                        setIpAttribute(subnetDecapConfig.tunnel_v6, src_ip_v6_str);
                        // update source IP of unhandled IPv6 decap terms
                        updateUnhandledDecapTunnelTerms(subnetDecapConfig.tunnel_v6, src_ip_v6_str);
                    }
                    subnetDecapConfig.src_ip_v6 = src_ip_v6_str;
                }
            }
            oss << ".";
            SWSS_LOG_NOTICE("%s", oss.str().c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        subnetDecapConfig.enable = false;
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s.", op.c_str());
    }
}

/**
 * Function Description:
 *    @brief adds a decap tunnel to ASIC_DB
 *
 * Arguments:
 *    @param[in] type - type of tunnel
 *    @param[in] p_src_ip - source ip address for encap (nullptr to skip this)
 *    @param[in] dscp - dscp mode (uniform/pipe)
 *    @param[in] ecn - ecn mode (copy_from_outer/standard)
 *    @param[in] ttl - ttl mode (uniform/pipe)
 *    @param[in] dscp_to_tc_map_id - Map ID for remapping DSCP to TC (decap)
 *    @param[in] tc_to_pg_map_id   - Map ID for remapping TC to PG (decap)
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::addDecapTunnel(
    string key,
    string type,
    IpAddress* p_src_ip,
    string dscp,
    string ecn,
    string encap_ecn,
    string ttl,
    sai_object_id_t dscp_to_tc_map_id,
    sai_object_id_t tc_to_pg_map_id)
{

    SWSS_LOG_ENTER();

    sai_status_t status;
    // adding tunnel attributes to array and writing to ASIC_DB
    sai_attribute_t attr;
    vector<sai_attribute_t> tunnel_attrs;
    sai_object_id_t overlayIfId;

    // create the overlay router interface to create a LOOPBACK type router interface (decap)
    vector<sai_attribute_t> overlay_intf_attrs;

    sai_attribute_t overlay_intf_attr;
    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    overlay_intf_attr.value.oid = gVirtualRouterId;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    overlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    overlay_intf_attr.value.u32 = OVERLAY_RIF_DEFAULT_MTU;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    status = sai_router_intfs_api->create_router_interface(&overlayIfId, gSwitchId, (uint32_t)overlay_intf_attrs.size(), overlay_intf_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create overlay router interface %d", status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_ROUTER_INTERFACE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Create overlay loopback router interface oid:%" PRIx64, overlayIfId);

    // tunnel type (only ipinip for now)
    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    attr.value.oid = overlayIfId;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(attr);

    // tunnel src ip
    if (p_src_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
        copy(attr.value.ipaddr, p_src_ip->to_string());
        tunnel_attrs.push_back(attr);
    }

    // decap ecn mode (copy from outer/standard)
    attr.id = SAI_TUNNEL_ATTR_DECAP_ECN_MODE;
    if (ecn == "copy_from_outer")
    {
        attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER;
    }
    else if (ecn == "standard")
    {
        attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_STANDARD;
    }
    tunnel_attrs.push_back(attr);

    if (!encap_ecn.empty())
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_ECN_MODE;
        if (encap_ecn == "standard")
        {
            attr.value.s32 = SAI_TUNNEL_ENCAP_ECN_MODE_STANDARD;
            tunnel_attrs.push_back(attr);
        }
    }

    // ttl mode (uniform/pipe)
    attr.id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
    if (ttl == "uniform")
    {
        attr.value.s32 = SAI_TUNNEL_TTL_MODE_UNIFORM_MODEL;
    }
    else if (ttl == "pipe")
    {
        attr.value.s32 = SAI_TUNNEL_TTL_MODE_PIPE_MODEL;
    }
    tunnel_attrs.push_back(attr);

    // dscp mode (uniform/pipe)
    attr.id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
    if (dscp == "uniform")
    {
        attr.value.s32 = SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL;
    }
    else if (dscp == "pipe")
    {
        attr.value.s32 = SAI_TUNNEL_DSCP_MODE_PIPE_MODEL;
    }
    tunnel_attrs.push_back(attr);

    // DSCP_TO_TC_MAP
    if (dscp_to_tc_map_id != SAI_NULL_OBJECT_ID)
    {
        attr.id = SAI_TUNNEL_ATTR_DECAP_QOS_DSCP_TO_TC_MAP;
        attr.value.oid = dscp_to_tc_map_id;
        tunnel_attrs.push_back(attr);
    }

    //TC_TO_PG_MAP
    if (tc_to_pg_map_id != SAI_NULL_OBJECT_ID)
    {
        attr.id = SAI_TUNNEL_ATTR_DECAP_QOS_TC_TO_PRIORITY_GROUP_MAP;
        attr.value.oid = tc_to_pg_map_id;
        tunnel_attrs.push_back(attr);
    }

    // write attributes to ASIC_DB
    sai_object_id_t tunnel_id;
    status = sai_tunnel_api->create_tunnel(&tunnel_id, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED || status == SAI_STATUS_ATTR_NOT_IMPLEMENTED_0)
        {
            SWSS_LOG_WARN("SAI_API_TUNNEL (create SAI_TUNNEL_TYPE_IPINIP): unsupported (%d) for tunnel %s (%d).", (int)status, key.c_str(), (int)tunnel_id);

            // Make sure we're not leaking a SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID overlayIfId here.
            sai_status_t rm_status = sai_router_intfs_api->remove_router_interface(overlayIfId);
            if (rm_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove tunnel overlay interface: %" PRIu64, overlayIfId);
                task_process_status rm_handle_status = handleSaiRemoveStatus(SAI_API_ROUTER_INTERFACE, rm_status);
                if (rm_handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(rm_handle_status);
                }
            }
            return false;
        }

        SWSS_LOG_ERROR("Failed to create tunnel");
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    tunnelTable[key] = {
        tunnel_id,              // tunnel_id
        overlayIfId,            // overlay_intf_id
        1,                      // ref count
        {},                     // tunnel_term_info
        type,                   // tunnel_type
        dscp,                   // dscp_mode
        ecn,                    // ecn_mode
        encap_ecn,              // encap_ecn_mode
        ttl,                    // ttl_mode
        SAI_NULL_OBJECT_ID,     // encap_tc_to_dscp_map_id
        SAI_NULL_OBJECT_ID      // encap_tc_to_queue_map_id
    };
    setDecapTunnelStatus(key);

    return true;
}

/**
 * Function Description:
 *    @brief adds a decap tunnel termination entry to ASIC_DB
 *
 * Arguments:
 *    @param[in] tunnel_name - key of the tunnel from APP_DB
 *    @param[in] src_ip_str - source ip prefix of the decap term entry
 *    @param[in] dst_ip_str - destination ip prefix of the decap term entry
 *    @param[in] term_type - P2P or P2MP. Other types (MP2P and MP2MP) not supported yet
 *    @param[in] subnet_type - the subnet type
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::addDecapTunnelTermEntry(
    std::string tunnel_name,
    std::string src_ip_str,
    std::string dst_ip_str,
    TunnelTermType term_type,
    std::string subnet_type)
{
    SWSS_LOG_ENTER();

    auto tunnel_it = tunnelTable.find(tunnel_name);
    if (tunnel_it == tunnelTable.end())
    {
        SWSS_LOG_ERROR("Tunnel %s does not exist.", tunnel_name.c_str());
        return false;
    }
    auto &tunnel = tunnel_it->second;

    sai_attribute_t attr;
    IpPrefix src_ip{src_ip_str};
    IpPrefix dst_ip{dst_ip_str};

    if (tunnel.tunnel_term_info.find(dst_ip) != tunnel.tunnel_term_info.end())
    {
        SWSS_LOG_NOTICE("Tunnel decap term entry %s already exists.", dst_ip_str.c_str());
        return true;
    }

    // adding tunnel table entry attributes to array and writing to ASIC_DB
    vector<sai_attribute_t> tunnel_table_entry_attrs;
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = gVirtualRouterId;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    if (term_type == TUNNEL_TERM_TYPE_P2P)
    {
        attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P;
    }
    else if (term_type == TUNNEL_TERM_TYPE_P2MP)
    {
        attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
    }
    else if (term_type == TUNNEL_TERM_TYPE_MP2MP)
    {
        attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP;
    }
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel.tunnel_id;
    tunnel_table_entry_attrs.push_back(attr);

    if (term_type == TUNNEL_TERM_TYPE_P2P || term_type == TUNNEL_TERM_TYPE_MP2MP)
    {
        if (src_ip.isV4() != dst_ip.isV4())
        {
            SWSS_LOG_ERROR("Src IP %s doesn't match IP version of dst IP %s.", src_ip_str.c_str(), dst_ip_str.c_str());
            return false;
        }
        // Set src ip for P2P or MP2MP
        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP;
        copy(attr.value.ipaddr, src_ip.getIp());
        tunnel_table_entry_attrs.push_back(attr);
    }

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
    copy(attr.value.ipaddr, dst_ip.getIp());
    tunnel_table_entry_attrs.push_back(attr);

    if (term_type == TUNNEL_TERM_TYPE_MP2MP)
    {
        // Set src/dst ip mask for MP2MP
        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK;
        copy(attr.value.ipaddr, src_ip.getMask());
        tunnel_table_entry_attrs.push_back(attr);

        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK;
        copy(attr.value.ipaddr, dst_ip.getMask());
        tunnel_table_entry_attrs.push_back(attr);
    }

    // create the tunnel table entry
    sai_object_id_t tunnel_term_table_entry_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_term_table_entry(&tunnel_term_table_entry_id, gSwitchId, (uint32_t)tunnel_table_entry_attrs.size(), tunnel_table_entry_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED || status == SAI_STATUS_ATTR_NOT_IMPLEMENTED_0)
        {
            SWSS_LOG_WARN("SAI_API_TUNNEL (create SAI_TUNNEL_TYPE_IPINIP): unsupported (%d) for tunnel %s with IP %s.", (int)status, tunnel_name.c_str(), dst_ip.to_string().c_str());
            return false;
        }        
        SWSS_LOG_ERROR("Failed to create tunnel decap term entry %s.", dst_ip.to_string().c_str());
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    tunnel.tunnel_term_info[dst_ip] = {
        tunnel_term_table_entry_id, // tunnel_term_id
        src_ip_str,                 // src_ip
        dst_ip_str,                 // dst_ip
        term_type,                  // tunnel_type
        subnet_type                 // subnet_type
    };
    increaseTunnelRefCount(tunnel_name);
    setDecapTunnelTermStatus(tunnel_name, dst_ip_str, src_ip_str, term_type, subnet_type);

    SWSS_LOG_NOTICE("Created tunnel decap term entry %s.", dst_ip_str.c_str());

    return true;
}

/**
 * Function Description:
 *    @brief sets attributes for a tunnel
 *
 * Arguments:
 *    @param[in] field - field to set the attribute for
 *    @param[in] value - value to set the attribute to
 *    @param[in] existing_tunnel_id - the id of the tunnel you want to set the attribute for
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::setTunnelAttribute(string field, string value, sai_object_id_t existing_tunnel_id)
{

    sai_attribute_t attr;

    if (field == "ttl_mode")
    {
        // ttl mode (uniform/pipe)
        attr.id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
        if (value == "uniform")
        {
            attr.value.s32 = SAI_TUNNEL_TTL_MODE_UNIFORM_MODEL;
        }
        else if (value == "pipe")
        {
            attr.value.s32 = SAI_TUNNEL_TTL_MODE_PIPE_MODEL;
        }
    }

    if (field == "dscp_mode")
    {
        // dscp mode (uniform/pipe)
        attr.id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
        if (value == "uniform")
        {
            attr.value.s32 = SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL;
        }
        else if (value == "pipe")
        {
            attr.value.s32 = SAI_TUNNEL_DSCP_MODE_PIPE_MODEL;
        }
    }

    sai_status_t status = sai_tunnel_api->set_tunnel_attribute(existing_tunnel_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set attribute %s with value %s\n", field.c_str(), value.c_str());
        task_process_status handle_status = handleSaiSetStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Set attribute %s with value %s\n", field.c_str(), value.c_str());
    return true;
}

/**
 * Function Description:
 *    @brief sets attributes for a tunnel （decap_dscp_to_tc_map and decap_tc_to_pg_map are supported)
 *
 * Arguments:
 *    @param[in] field - field to set the attribute for
 *    @param[in] value - value to set the attribute to (sai_object_id)
 *    @param[in] existing_tunnel_id - the id of the tunnel you want to set the attribute for
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::setTunnelAttribute(string field, sai_object_id_t value, sai_object_id_t existing_tunnel_id)
{

    sai_attribute_t attr;

    if (field == decap_dscp_to_tc_field_name)
    {
        // TC remapping.
        attr.id = SAI_TUNNEL_ATTR_DECAP_QOS_DSCP_TO_TC_MAP;
        attr.value.oid = value;

    }
    else if (field == decap_tc_to_pg_field_name)
    {
        // TC to PG remapping
        attr.id = SAI_TUNNEL_ATTR_DECAP_QOS_TC_TO_PRIORITY_GROUP_MAP;
        attr.value.oid = value;
    }

    sai_status_t status = sai_tunnel_api->set_tunnel_attribute(existing_tunnel_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set attribute %s with value %" PRIu64, field.c_str(), value);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Set attribute %s with value %" PRIu64, field.c_str(), value);
    return true;
}

/**
 * Function Description:
 *    @brief sets ips for a particular tunnel. deletes ips that are old and adds new ones
 *
 * Arguments:
 *    @param[in] tunnel_name - tunnel name from APP_DB
 *    @param[in] src_ip_str - new source ip address for the decap terms
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::setIpAttribute(string tunnel_name, string src_ip_str)
{
    SWSS_LOG_ENTER();

    if (src_ip_str.empty())
    {
        return false;
    }

    SWSS_LOG_NOTICE("Setting source IP for decap terms of tunnel %s to %s", tunnel_name.c_str(), src_ip_str.c_str());

    auto tunnel_it = tunnelTable.find(tunnel_name);
    if (tunnel_it == tunnelTable.end())
    {
        SWSS_LOG_INFO("Tunnel %s does not exist", tunnel_name.c_str());
        return true;
    }

    TunnelEntry *tunnel_info = &tunnel_it->second;
    map<swss::IpPrefix, TunnelTermEntry> decap_terms_copy(tunnel_info->tunnel_term_info.begin(), tunnel_info->tunnel_term_info.end());

    for (auto it = decap_terms_copy.begin(); it != decap_terms_copy.end(); ++it)
    {
        TunnelTermEntry &term_entry = it->second;
        if (!removeDecapTunnelTermEntry(tunnel_name, term_entry.dst_ip))
        {
            return false;
        }
    }

    for (auto it = decap_terms_copy.begin(); it != decap_terms_copy.end(); ++it)
    {
        if (tunnel_info->tunnel_term_info.find(it->first) == tunnel_info->tunnel_term_info.end())
        {
            TunnelTermEntry &term_entry = it->second;
            // add the decap term with new src ip
            if (!addDecapTunnelTermEntry(tunnel_name, src_ip_str, term_entry.dst_ip, term_entry.term_type, term_entry.subnet_type))
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * Function Description:
 *    @brief remove decap tunnel
 *
 * Arguments:
 *    @param[in] table_name - name of the table in APP_DB
 *    @param[in] key - key of the tunnel from APP_DB
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::removeDecapTunnel(string table_name, string key)
{
    sai_status_t status;
    TunnelEntry *tunnel_info = &tunnelTable.find(key)->second;

    if (tunnel_info->tunnel_term_info.size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel %s that has decap terms.", key.c_str());
        return false;
    }

    status = sai_tunnel_api->remove_tunnel(tunnel_info->tunnel_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel: %" PRIu64, tunnel_info->tunnel_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    // delete overlay loopback interface
    status = sai_router_intfs_api->remove_router_interface(tunnel_info->overlay_intf_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel overlay interface: %" PRIu64, tunnel_info->overlay_intf_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ROUTER_INTERFACE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    tunnelTable.erase(key);
    gQosOrch->removeTunnelReference(table_name, key);
    removeDecapTunnelStatus(key);
    return true;
}

/**
 * Function Description:
 *    @brief remove decap tunnel termination entry
 *
 * Arguments:
 *    @param[in] tunnel_name - tunnel name
 *    @param[in] dst_ip - destination ip address of the decap term entry
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::removeDecapTunnelTermEntry(std::string tunnel_name, std::string dst_ip_str)
{
    sai_status_t status;

    auto tunnel_it = tunnelTable.find(tunnel_name);
    if (tunnel_it == tunnelTable.end())
    {
        SWSS_LOG_ERROR("Tunnel %s does not exist.", tunnel_name.c_str());
        return false;
    }

    IpPrefix dst_ip{dst_ip_str};
    auto term_it = tunnel_it->second.tunnel_term_info.find(dst_ip);
    if (term_it == tunnel_it->second.tunnel_term_info.end())
    {
        SWSS_LOG_ERROR("Tunnel decap term entry %s does not exist.", dst_ip_str.c_str());
        return false;
    }
    auto &decap_term = term_it->second;

    status = sai_tunnel_api->remove_tunnel_term_table_entry(decap_term.tunnel_term_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel table entry: %" PRIu64, decap_term.tunnel_term_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    tunnel_it->second.tunnel_term_info.erase(term_it);
    decreaseTunnelRefCount(tunnel_name);
    removeDecapTunnelTermStatus(tunnel_name, dst_ip_str);
    SWSS_LOG_NOTICE("Removed decap tunnel term entry with ip address: %s.", dst_ip_str.c_str());
    return true;
}

sai_object_id_t TunnelDecapOrch::getNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr)
{
    auto nh = tunnelNhs.find(tunnelKey);
    if (nh == tunnelNhs.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    auto it = nh->second.find(ipAddr);
    if (it == nh->second.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    return nh->second[ipAddr].nh_id;
}

int TunnelDecapOrch::incNextHopRef(std::string tunnelKey, IpAddress& ipAddr)
{
    return (++ tunnelNhs[tunnelKey][ipAddr].ref_count);
}

int TunnelDecapOrch::decNextHopRef(std::string tunnelKey, IpAddress& ipAddr)
{
    return (-- tunnelNhs[tunnelKey][ipAddr].ref_count);
}

sai_object_id_t TunnelDecapOrch::createNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr)
{
    if (tunnelTable.find(tunnelKey) == tunnelTable.end())
    {
        SWSS_LOG_ERROR("Tunnel not found %s", tunnelKey.c_str());
        return SAI_NULL_OBJECT_ID;
    }

    sai_object_id_t nhid;
    if ((nhid = getNextHopTunnel(tunnelKey, ipAddr)) != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("NH tunnel already exist '%s'", ipAddr.to_string().c_str());
        incNextHopRef(tunnelKey, ipAddr);
        return nhid;
    }

    TunnelEntry *tunnel_info = &tunnelTable.find(tunnelKey)->second;

    std::vector<sai_attribute_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
    next_hop_attrs.push_back(next_hop_attr);

    sai_ip_address_t host_ip;
    swss::copy(host_ip, ipAddr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    next_hop_attr.value.ipaddr = host_ip;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    next_hop_attr.value.oid = tunnel_info->tunnel_id;
    next_hop_attrs.push_back(next_hop_attr);

    sai_object_id_t next_hop_id = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_next_hop_api->create_next_hop(&next_hop_id, gSwitchId,
                                            static_cast<uint32_t>(next_hop_attrs.size()),
                                            next_hop_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Tunnel NH create failed %s, ip %s", tunnelKey.c_str(),
                        ipAddr.to_string().c_str());
        handleSaiCreateStatus(SAI_API_NEXT_HOP, status);
    }
    else
    {
        SWSS_LOG_NOTICE("Tunnel NH created %s, ip %s",
                         tunnelKey.c_str(), ipAddr.to_string().c_str());

        if (ipAddr.isV4())
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
        }
        else
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
        }

        tunnelNhs[tunnelKey][ipAddr] = { next_hop_id, 1 };
    }

    return next_hop_id;
}

bool TunnelDecapOrch::removeNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr)
{
    if (tunnelTable.find(tunnelKey) == tunnelTable.end())
    {
        SWSS_LOG_ERROR("Tunnel not found %s", tunnelKey.c_str());
        return true;
    }

    sai_object_id_t nhid;
    if ((nhid = getNextHopTunnel(tunnelKey, ipAddr)) == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("NH tunnel doesn't exist '%s'", ipAddr.to_string().c_str());
        return true;
    }

    if (decNextHopRef(tunnelKey, ipAddr))
    {
        SWSS_LOG_NOTICE("Tunnel NH referenced, decremented ref count %s, ip %s",
                         tunnelKey.c_str(), ipAddr.to_string().c_str());
        return true;
    }

    sai_status_t status = sai_next_hop_api->remove_next_hop(nhid);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate next hop %s on %s, rv:%d",
                            ipAddr.to_string().c_str(), tunnelKey.c_str(), status);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove next hop %s on %s, rv:%d",
                            ipAddr.to_string().c_str(), tunnelKey.c_str(), status);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    else
    {
        SWSS_LOG_NOTICE("Tunnel NH removed %s, ip %s",
                         tunnelKey.c_str(), ipAddr.to_string().c_str());

        if (ipAddr.isV4())
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
        }
        else
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
        }
    }

    tunnelNhs[tunnelKey].erase(ipAddr);

    return true;
}

IpAddresses TunnelDecapOrch::getDstIpAddresses(std::string tunnelKey)
{
    IpAddresses dst_ips{};

    if (tunnelTable.find(tunnelKey) == tunnelTable.end())
    {
        SWSS_LOG_INFO("Tunnel not found %s", tunnelKey.c_str());
        return dst_ips;
    }

    auto &tunnel = tunnelTable[tunnelKey];
    for (auto it = tunnel.tunnel_term_info.begin(); it != tunnel.tunnel_term_info.end(); ++it)
    {
        dst_ips.add(it->first.getIp());
    }

    return dst_ips;
}

std::string TunnelDecapOrch::getDscpMode(const std::string &tunnelKey) const
{
    auto iter = tunnelTable.find(tunnelKey);
    if (iter == tunnelTable.end())
    {
        SWSS_LOG_INFO("Tunnel not found %s", tunnelKey.c_str());
        return "";
    }
    return iter->second.dscp_mode;
}

bool TunnelDecapOrch::getQosMapId(const std::string &tunnelKey, const std::string &qos_table_type, sai_object_id_t &oid) const
{
    auto iter = tunnelTable.find(tunnelKey);
    if (iter == tunnelTable.end())
    {
        SWSS_LOG_INFO("Tunnel not found %s", tunnelKey.c_str());
        return false;
    }
    if (qos_table_type == encap_tc_to_dscp_field_name)
    {
        oid = iter->second.encap_tc_to_dscp_map_id;
    }
    else if (qos_table_type == encap_tc_to_queue_field_name)
    {
        oid = iter->second.encap_tc_to_queue_map_id;
    }
    else
    {
        SWSS_LOG_ERROR("Unsupported qos type %s", qos_table_type.c_str());
        return false;
    }
    return true;
}

void TunnelDecapOrch::updateUnhandledDecapTunnelTerms(const string &tunnel_name, const string &src_ip_str)
{
    SWSS_LOG_ENTER();

    if (src_ip_str.empty())
    {
        return;
    }

    SWSS_LOG_INFO("Updating unhandled decap tunnel terms for tunnel %s with source IP %s",
                  tunnel_name.c_str(), src_ip_str.c_str());

    auto tunnel_it = unhandledDecapTerms.find(tunnel_name);
    if (tunnel_it != unhandledDecapTerms.end())
    {
        for (auto term_it = tunnel_it->second.begin(); term_it != tunnel_it->second.end(); ++term_it)
        {
            auto &term = term_it->second;
            term.src_ip = src_ip_str;
        }
    }
}

void TunnelDecapOrch::processUnhandledDecapTunnelTerms(const string &tunnel_name)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Processing unhandled decap tunnel terms for tunnel %s",
                  tunnel_name.c_str());

    auto tunnel_it = unhandledDecapTerms.find(tunnel_name);
    if (tunnel_it != unhandledDecapTerms.end())
    {
        for (auto term_it = tunnel_it->second.begin(); term_it != tunnel_it->second.end();)
        {
            auto &term = term_it->second;
            if (addDecapTunnelTermEntry(tunnel_name, term.src_ip, term.dst_ip, term.term_type, term.subnet_type))
            {
                term_it = tunnel_it->second.erase(term_it);
            }
            else
            {
                ++term_it;
            }
        }
    }
}

inline void TunnelDecapOrch::setDecapTunnelStatus(const std::string &tunnel_name)
{
    auto &tunnel = tunnelTable.at(tunnel_name);

    vector<FieldValueTuple> fv;
    APPEND_IF_NOT_EMPTY(fv, tunnel, tunnel_type);
    APPEND_IF_NOT_EMPTY(fv, tunnel, dscp_mode);
    APPEND_IF_NOT_EMPTY(fv, tunnel, ecn_mode);
    APPEND_IF_NOT_EMPTY(fv, tunnel, encap_ecn_mode);
    APPEND_IF_NOT_EMPTY(fv, tunnel, ttl_mode);
    stateTunnelDecapTable->set(tunnel_name, fv);
}

inline void TunnelDecapOrch::removeDecapTunnelStatus(const std::string &tunnel_name)
{
    stateTunnelDecapTable->del(tunnel_name);
}

inline void TunnelDecapOrch::setDecapTunnelTermStatus(
    const std::string &tunnel_name, const std::string &dst_ip_str, const std::string &src_ip_str,
    TunnelTermType term_type, const std::string &subnet_type)
{
    const static map<TunnelTermType, string> DecapTermTypeStrLookupTable = {
        {TUNNEL_TERM_TYPE_P2P, "P2P"},
        {TUNNEL_TERM_TYPE_P2MP, "P2MP"},
        {TUNNEL_TERM_TYPE_MP2MP, "MP2MP"}};

    string tunnel_term_key = tunnel_name + state_db_key_delimiter + dst_ip_str;
    string term_type_str = DecapTermTypeStrLookupTable.at(term_type);
    vector<FieldValueTuple> fv = {{ "term_type", term_type_str }};
    if (!src_ip_str.empty())
    {
        fv.emplace_back("src_ip", src_ip_str);
    }
    if (!subnet_type.empty())
    {
        fv.emplace_back("subnet_type", subnet_type);
    }

    stateTunnelDecapTermTable->set(tunnel_term_key, fv);
}

inline void TunnelDecapOrch::removeDecapTunnelTermStatus(const std::string &tunnel_name, const std::string &dst_ip_str)
{
    string tunnel_term_key = tunnel_name + state_db_key_delimiter + dst_ip_str;
    stateTunnelDecapTermTable->del(tunnel_term_key);
}

inline void TunnelDecapOrch::RemoveTunnelIfNotReferenced(const string &tunnel_name)
{
    if (getTunnelRefCount(tunnel_name) == 0)
    {
        removeDecapTunnel(APP_TUNNEL_DECAP_TABLE_NAME, tunnel_name);
        SWSS_LOG_NOTICE("Tunnel %s removed from ASIC_DB.", tunnel_name.c_str());
    }
}
