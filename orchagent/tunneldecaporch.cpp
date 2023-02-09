#include <string.h>
#include <inttypes.h>
#include "tunneldecaporch.h"
#include "portsorch.h"
#include "crmorch.h"
#include "logger.h"
#include "swssnet.h"
#include "qosorch.h"

#define OVERLAY_RIF_DEFAULT_MTU 9100

extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_next_hop_api_t* sai_next_hop_api;

extern sai_object_id_t  gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;
extern CrmOrch*         gCrmOrch;
extern QosOrch*         gQosOrch;

TunnelDecapOrch::TunnelDecapOrch(DBConnector *db, string tableName) : Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

void TunnelDecapOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }
    string table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        IpAddresses ip_addresses;
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
                else if (fvField(i) == "dst_ip")
                {
                    try
                    {
                        ip_addresses = IpAddresses(fvValue(i));
                    }
                    catch (const std::invalid_argument &e)
                    {
                        SWSS_LOG_ERROR("%s", e.what());
                        valid = false;
                        break;
                    }
                    if (exists)
                    {
                        setIpAttribute(key, ip_addresses, tunnelTable.find(key)->second.tunnel_id);
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
            }

            if (task_status == task_process_status::task_need_retry)
            {
                ++it;
                continue;
            }

            //create new tunnel if it doesn't exists already
            if (valid && !exists)
            {

                if (addDecapTunnel(key, tunnel_type, ip_addresses, p_src_ip, dscp_mode, ecn_mode, encap_ecn_mode, ttl_mode,
                                    dscp_to_tc_map_id, tc_to_pg_map_id))
                {
                    // Record only
                    tunnelTable[key].encap_tc_to_dscp_map_id = tc_to_dscp_map_id;
                    tunnelTable[key].encap_tc_to_queue_map_id = tc_to_queue_map_id;
                    SWSS_LOG_NOTICE("Tunnel(s) added to ASIC_DB.");
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to add tunnels to ASIC_DB.");
                }
            }
        }

        if (op == DEL_COMMAND)
        {
            if (exists)
            {
                removeDecapTunnel(table_name, key);
            }
            else
            {
                SWSS_LOG_ERROR("Tunnel cannot be removed since it doesn't exist.");
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

/**
 * Function Description:
 *    @brief adds a decap tunnel to ASIC_DB
 *
 * Arguments:
 *    @param[in] type - type of tunnel
 *    @param[in] dst_ip - destination ip address to decap
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
    IpAddresses dst_ip,
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
    IpAddress src_ip("0.0.0.0");
    // adding tunnel attributes to array and writing to ASIC_DB
    sai_attribute_t attr;
    vector<sai_attribute_t> tunnel_attrs;
    sai_object_id_t overlayIfId;
    TunnelTermType term_type = TUNNEL_TERM_TYPE_P2MP;

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
        src_ip = *p_src_ip;
        term_type = TUNNEL_TERM_TYPE_P2P;
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
        SWSS_LOG_ERROR("Failed to create tunnel");
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    tunnelTable[key] = { tunnel_id, overlayIfId, dst_ip, {}, dscp, SAI_NULL_OBJECT_ID, SAI_NULL_OBJECT_ID };

    // create a decap tunnel entry for every source_ip - dest_ip pair
    if (!addDecapTunnelTermEntries(key, src_ip, dst_ip, tunnel_id, term_type))
    {
        return false;
    }

    return true;
}

/**
 * Function Description:
 *    @brief adds a decap tunnel termination entry to ASIC_DB
 *
 * Arguments:
 *    @param[in] tunnelKey - key of the tunnel from APP_DB
 *    @param[in] src_ip - source ip address of decap tunnel
 *    @param[in] dst_ips - destination ip addresses to decap
 *    @param[in] tunnel_id - the id of the tunnel
 *    @param[in] term_type - P2P or P2MP. Other types (MP2P and MP2MP) not supported yet
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::addDecapTunnelTermEntries(string tunnelKey, swss::IpAddress src_ip, swss::IpAddresses dst_ips, sai_object_id_t tunnel_id, TunnelTermType tunnel_type)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    // adding tunnel table entry attributes to array and writing to ASIC_DB
    vector<sai_attribute_t> tunnel_table_entry_attrs;
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = gVirtualRouterId;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    if (tunnel_type == TUNNEL_TERM_TYPE_P2P)
    {
        attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P;
    }
    else
    {
        attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
    }
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_table_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel_id;
    tunnel_table_entry_attrs.push_back(attr);

    if (tunnel_type == TUNNEL_TERM_TYPE_P2P)
    {
        // Set src ip for P2P only
        attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP;
        copy(attr.value.ipaddr, src_ip);
        tunnel_table_entry_attrs.push_back(attr);
    }

    TunnelEntry *tunnel_info = &tunnelTable.find(tunnelKey)->second;

    // loop through the IP list and create a new tunnel table entry for every IP (in network byte order)
    set<IpAddress> tunnel_ips = dst_ips.getIpAddresses();
    for (auto it = tunnel_ips.begin(); it != tunnel_ips.end(); ++it)
    {
        const IpAddress& ia = *it;
        string dst_ip = ia.to_string();
        // The key will be src_ip-dst_ip (like 10.1.1.1-20.2.2.2) if src_ip is not 0,
        // or the key will contain dst_ip only
        string key;
        if (!src_ip.isZero())
        {
            key = src_ip.to_string() + '-' + dst_ip;
        }
        else
        {
            key = dst_ip;
        }

        // check if the there's an entry already for the key pair
        if (existingIps.find(key) != existingIps.end())
        {
            SWSS_LOG_NOTICE("%s already exists. Did not create entry.", key.c_str());
        }
        else
        {
            attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
            copy(attr.value.ipaddr, ia);
            tunnel_table_entry_attrs.push_back(attr);

            // create the tunnel table entry
            sai_object_id_t tunnel_term_table_entry_id;
            sai_status_t status = sai_tunnel_api->create_tunnel_term_table_entry(&tunnel_term_table_entry_id, gSwitchId, (uint32_t)tunnel_table_entry_attrs.size(), tunnel_table_entry_attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create tunnel entry table for ip: %s", key.c_str());
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_TUNNEL, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }

            // insert into ip to entry mapping
            existingIps.insert(key);

            // insert entry id and ip into tunnel mapping
            tunnel_info->tunnel_term_info.push_back({ tunnel_term_table_entry_id, src_ip.to_string(), dst_ip, tunnel_type });

            // pop the last element for the next loop
            tunnel_table_entry_attrs.pop_back();

            SWSS_LOG_NOTICE("Created tunnel entry for ip: %s", dst_ip.c_str());
        }

    }
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
 *    @param[in] key - key of the tunnel from APP_DB
 *    @param[in] new_ip_addresses - new destination ip addresses to decap (comes from APP_DB)
 *    @param[in] tunnel_id - the id of the tunnel
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::setIpAttribute(string key, IpAddresses new_ip_addresses, sai_object_id_t tunnel_id)
{
    TunnelEntry *tunnel_info = &tunnelTable.find(key)->second;

    // make a copy of tunnel_term_info to loop through
    vector<TunnelTermEntry> tunnel_term_info_copy(tunnel_info->tunnel_term_info);

    tunnel_info->tunnel_term_info.clear();
    tunnel_info->dst_ip_addrs = new_ip_addresses;

    // loop through original ips and remove ips not in the new ip_addresses
    for (auto it = tunnel_term_info_copy.begin(); it != tunnel_term_info_copy.end(); ++it)
    {
        TunnelTermEntry tunnel_entry_info = *it;
        string ip = tunnel_entry_info.dst_ip;
        if (!new_ip_addresses.contains(ip))
        {
            if (!removeDecapTunnelTermEntry(tunnel_entry_info.tunnel_term_id, ip))
            {
                return false;
            }
        }
        else
        {
            // add the data into the tunnel_term_info
            tunnel_info->tunnel_term_info.push_back({ tunnel_entry_info.tunnel_term_id, "0.0.0.0", ip, TUNNEL_TERM_TYPE_P2MP });
        }
    }

    // add all the new ip addresses
    if(!addDecapTunnelTermEntries(key, IpAddress(0), new_ip_addresses, tunnel_id, TUNNEL_TERM_TYPE_P2MP))
    {
        return false;
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

    // loop through the tunnel entry ids related to the tunnel and remove them before removing the tunnel
    for (auto it = tunnel_info->tunnel_term_info.begin(); it != tunnel_info->tunnel_term_info.end(); ++it)
    {
        TunnelTermEntry tunnel_entry_info = *it;
        string term_key;
        swss::IpAddress src_ip(tunnel_entry_info.src_ip);
        if (!src_ip.isZero())
        {
            term_key = src_ip.to_string() + '-' + tunnel_entry_info.dst_ip;
        }
        else
        {
            term_key = tunnel_entry_info.dst_ip;
        }
        if (!removeDecapTunnelTermEntry(tunnel_entry_info.tunnel_term_id, term_key))
        {
            return false;
        }
    }

    tunnel_info->tunnel_term_info = {};

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
    return true;
}

/**
 * Function Description:
 *    @brief remove decap tunnel termination entry
 *
 * Arguments:
 *    @param[in] key - key of the tunnel from APP_DB
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::removeDecapTunnelTermEntry(sai_object_id_t tunnel_term_id, string key)
{
    sai_status_t status;

    status = sai_tunnel_api->remove_tunnel_term_table_entry(tunnel_term_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel table entry: %" PRIu64, tunnel_term_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    // making sure to remove all instances of the ip address
    existingIps.erase(key);
    SWSS_LOG_NOTICE("Removed decap tunnel term entry with ip address: %s", key.c_str());
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
    if (tunnelTable.find(tunnelKey) == tunnelTable.end())
    {
        SWSS_LOG_INFO("Tunnel not found %s", tunnelKey.c_str());
        return IpAddresses();
    }

    return tunnelTable[tunnelKey].dst_ip_addrs;
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
