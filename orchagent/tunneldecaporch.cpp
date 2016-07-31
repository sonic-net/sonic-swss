#include "tunneldecaporch.h"
#include <string.h>
#include "logger.h"

extern sai_tunnel_api_t* sai_tunnel_api;

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t overlayIfId;
extern sai_object_id_t underlayIfId;

TunnelDecapOrch::TunnelDecapOrch(DBConnector *db, string tableName) : Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

/**
 * Function Description:
 *    @brief reads from APP_DB and creates tunnel 
 */
void TunnelDecapOrch::doTask(Consumer& consumer)
{

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        IpAddresses ip_addresses;
        string tunnel_type;
        string dscp_mode;
        string ecn_mode;
        string ttl_mode;
        bool valid = true;

        // checking to see if the tunnel already exists
        bool exists = (tunnelTable.find(key) != tunnelTable.end());

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
                if (fvField(i) == "dst_ip") 
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
                if (fvField(i) == "dscp_mode")
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
                        setTunnelAttribute(fvField(i), dscp_mode, tunnelTable.find(key)->second.tunnel_id);
                    }
                }
                if (fvField(i) == "ecn_mode")
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
                        setTunnelAttribute(fvField(i), ecn_mode, tunnelTable.find(key)->second.tunnel_id);
                    }
                }
                if (fvField(i) == "ttl_mode")
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
                        setTunnelAttribute(fvField(i), ttl_mode, tunnelTable.find(key)->second.tunnel_id);
                    }
                }
            }
            // create new tunnel if it doesn't exists already
            if (valid && !exists)
            {
                if (addDecapTunnel(key, tunnel_type, ip_addresses, dscp_mode, ecn_mode, ttl_mode)) 
                {
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
                removeDecapTunnel(key);
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
 *    @param[in] dscp - dscp mode (uniform/pipe)
 *    @param[in] ecn - ecn mode (copy_from_outer/standard)
 *    @param[in] ttl - ttl mode (uniform/pipe)
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::addDecapTunnel(string key, string type, IpAddresses dst_ip, string dscp, string ecn, string ttl)
{

    SWSS_LOG_ENTER();

    // adding tunnel attributes to array and writing to ASIC_DB
    sai_attribute_t attr;
    vector<sai_attribute_t> tunnel_attrs;

    // tunnel type (only ipinip for now)
    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_IPINIP;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    attr.value.oid = overlayIfId;
    tunnel_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = underlayIfId;
    tunnel_attrs.push_back(attr);

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

    // ttl mode (uniform/pipe)
    attr.id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
    if (ttl == "uniform") 
    {
        attr.value.s32 = SAI_TUNNEL_TTL_UNIFORM_MODEL;
    }
    else if (ttl == "pipe") 
    {
        attr.value.s32 = SAI_TUNNEL_TTL_PIPE_MODEL;
    }
    tunnel_attrs.push_back(attr);

    // dscp mode (uniform/pipe)
    attr.id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
    if (dscp == "uniform") 
    {
        attr.value.s32 = SAI_TUNNEL_DSCP_UNIFORM_MODEL;
    }
    else if (dscp == "pipe") 
    {
        attr.value.s32 = SAI_TUNNEL_DSCP_PIPE_MODEL;
    }
    tunnel_attrs.push_back(attr);

    // write attributes to ASIC_DB
    sai_object_id_t tunnel_id;
    sai_status_t status = sai_tunnel_api->create_tunnel(&tunnel_id, tunnel_attrs.size(), tunnel_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel");
        return false;
    }

    tunnelTable[key] = { tunnel_id, {} };

    // TODO:
    // check if the database is already populated with the correct data
    // don't create duplicate entries
    // there should also be "business logic" for netbouncer in the "tunnel application" code, which is a different source file and daemon process

    // create a decap tunnel entry for every ip
    if (!addDecapTunnelTermEntries(key, dst_ip, tunnel_id))
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
 *    @param[in] dst_ip - destination ip addresses to decap
 *    @param[in] tunnel_id - the id of the tunnel 
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::addDecapTunnelTermEntries(string tunnelKey, IpAddresses dst_ip, sai_object_id_t tunnel_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    // adding tunnel table entry attributes to array and writing to ASIC_DB
    vector<sai_attribute_t> tunnel_table_entry_attrs;
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = gVirtualRouterId;
    tunnel_table_entry_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    attr.value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_P2MP;
    tunnel_table_entry_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_IPINIP;
    tunnel_table_entry_attrs.push_back(attr);
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel_id;
    tunnel_table_entry_attrs.push_back(attr);

    sai_ip_address_t tunnel_dst_ip;
    tunnel_dst_ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;

    set<IpAddress> tunnel_ips = dst_ip.getIpAddresses();
    struct sockaddr_in tunnel_ip_struct;
    string ip;

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;

    // loop through the IP list and create a new tunnel table entry for every IP (in network byte order)
    for (auto it = tunnel_ips.begin(); it != tunnel_ips.end(); ++it)
    {
        ip = it->to_string();

        // check if the there's an entry already for the ip
        if (existingIps.find(ip) != existingIps.end())
        {
            SWSS_LOG_ERROR("%s already exists. Did not create entry.", ip.c_str());
        }

        else
        {
            // turn string ip into network byte order
            inet_pton(AF_INET, ip.c_str(), &(tunnel_ip_struct.sin_addr));
            tunnel_dst_ip.addr.ip4 = tunnel_ip_struct.sin_addr.s_addr;

            attr.value.ipaddr = tunnel_dst_ip;
            tunnel_table_entry_attrs.push_back(attr);

            // create the tunnel table entry
            sai_object_id_t tunnel_term_table_entry_id;
            sai_status_t status = sai_tunnel_api->create_tunnel_term_table_entry(&tunnel_term_table_entry_id, tunnel_table_entry_attrs.size(), tunnel_table_entry_attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create tunnel entry table for ip: %s", ip.c_str());
                return false;
            }

            // insert into ip to entry mapping
            existingIps.insert(ip);

            // insert entry id and ip into tunnel mapping
            tunnelTable[tunnelKey].tunnel_term_info.push_back({ tunnel_term_table_entry_id, ip });
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

    SWSS_LOG_NOTICE("tunnel id: %llu", existing_tunnel_id);

    if (field == "ecn_mode")
    {
        // decap ecn mode (copy from outer/standard)
        attr.id = SAI_TUNNEL_ATTR_DECAP_ECN_MODE;
        if (value == "copy_from_outer") 
        {
            attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER;
        }
        else if (value == "standard") 
        {
            attr.value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_STANDARD;
        }
    }

    if (field == "ttl_mode")
    {
        // ttl mode (uniform/pipe)
        attr.id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
        if (value == "uniform") 
        {
            attr.value.s32 = SAI_TUNNEL_TTL_UNIFORM_MODEL;
        }
        else if (value == "pipe") 
        {
            attr.value.s32 = SAI_TUNNEL_TTL_PIPE_MODEL;
        }
    }

    if (field == "dscp_mode")
    {
        // dscp mode (uniform/pipe)
        attr.id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
        if (value == "uniform") 
        {
            attr.value.s32 = SAI_TUNNEL_DSCP_UNIFORM_MODEL;
        }
        else if (value == "pipe") 
        {
            attr.value.s32 = SAI_TUNNEL_DSCP_PIPE_MODEL;
        }
    }

    sai_status_t status = sai_tunnel_api->set_tunnel_attribute(existing_tunnel_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set attribute %s with value %s\n", field.c_str(), value.c_str());
        return false;
    }
    else
    {
        SWSS_LOG_NOTICE("Set attribute %s with value %s\n", field.c_str(), value.c_str());
    }
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

    // loop through original ips and remove ips not in the new ip_addresses
    for (auto it = tunnel_term_info_copy.begin(); it != tunnel_term_info_copy.end(); ++it)
    {
        TunnelTermEntry tunnel_entry_info = *it;
        string ip = tunnel_entry_info.ip_address;
        if (!new_ip_addresses.contains(ip))
        {
            if (!removeDecapTunnelTermEntry(key, tunnel_entry_info))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_ERROR("%s already exists. Will not create entry.", ip.c_str());

            // remove the ip from new_ip_addresses since it already exists (saves some performance time)
            new_ip_addresses.remove(ip);

            // add the data into the tunnel_term_info
            tunnel_info->tunnel_term_info.push_back({ tunnel_entry_info.tunnel_term_id, ip });
        }
    }

    set<IpAddress> new_tunnel_ips = new_ip_addresses.getIpAddresses();

    // loop through new ips and add ips that do not already exist
    for (auto it = new_tunnel_ips.begin(); it != new_tunnel_ips.end(); ++it)
    {
        string ip = it->to_string();
        if(!addDecapTunnelTermEntries(key, IpAddresses(ip), tunnel_id))
        {
            return false;
        }
    }

    return true;
}

/**
 * Function Description:
 *    @brief remove decap tunnel
 *
 * Arguments:
 *    @param[in] key - key of the tunnel from APP_DB
 *
 * Return Values:
 *    @return true on success and false if there's an error
 */
bool TunnelDecapOrch::removeDecapTunnel(string key)
{
    sai_status_t status;
    TunnelEntry *tunnel_info = &tunnelTable.find(key)->second;

    // loop through the tunnel entry ids related to the tunnel and remove them before removing the tunnel
    for (auto it = tunnel_info->tunnel_term_info.begin(); it != tunnel_info->tunnel_term_info.end(); ++it)
    {
        if (!removeDecapTunnelTermEntry(key, *it))
        {
            return false;
        }
    }

    tunnel_info->tunnel_term_info = {};

    status = sai_tunnel_api->remove_tunnel(tunnel_info->tunnel_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel: %llu", tunnel_info->tunnel_id);
        return false;
    }
    tunnelTable.erase(key);
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
bool TunnelDecapOrch::removeDecapTunnelTermEntry(string key, TunnelTermEntry &tunnel_term_info)
{
    sai_status_t status;
    
    sai_object_id_t tunnel_term_id = tunnel_term_info.tunnel_term_id;
    string ip = tunnel_term_info.ip_address;

    status = sai_tunnel_api->remove_tunnel_term_table_entry(tunnel_term_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel table entry: %llu", tunnel_term_id);
        return false;
    }

    // making sure to remove all instances of the ip address
    existingIps.erase(ip);
    SWSS_LOG_NOTICE("Removed ip address: %s", ip.c_str());
    return true;
}
