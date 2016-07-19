#include "tunneldecaporch.h"
#include <string.h>
#include "logger.h"

extern sai_tunnel_api_t* sai_tunnel_api;

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t overlayIfId;

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
        
        if (op == SET_COMMAND)
        {

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "tunnel_type") 
                {
                    tunnel_type = fvValue(i);
                }
                if (fvField(i) == "dst_ip") 
                {
                    ip_addresses = IpAddresses(fvValue(i));
                }
                if (fvField(i) == "dscp_mode")
                {
                    dscp_mode = fvValue(i);
                }
                if (fvField(i) == "ecn_mode")
                {
                    ecn_mode = fvValue(i);
                }
                if (fvField(i) == "ttl_mode")
                {
                    ttl_mode = fvValue(i);
                }
            }
            if (addDecapTunnel(tunnel_type, ip_addresses, dscp_mode, ecn_mode, ttl_mode)) 
            {
                SWSS_LOG_NOTICE("Tunnel(s) added to ASIC_DB.");
            }
            else
            {
                SWSS_LOG_ERROR("Failed to add tunnels to ASIC_DB.");
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

/**
 * Function Description:
 *    @brief adds a decap tunnel entry to ASIC_DB
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
bool TunnelDecapOrch::addDecapTunnel(string type, IpAddresses dst_ip, string dscp, string ecn, string ttl)
{

    SWSS_LOG_ENTER();

    int length_of_tunnel_attrs = 5;

    // adding tunnel attributes to array and writing to ASIC_DB
    sai_attribute_t tunnel_attrs[length_of_tunnel_attrs];

    // tunnel type (only ipinip for now)
    tunnel_attrs[0].id = SAI_TUNNEL_ATTR_TYPE;
    tunnel_attrs[0].value.s32 = SAI_TUNNEL_IPINIP;
    tunnel_attrs[1].id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    tunnel_attrs[1].value.oid = overlayIfId;

    // decap ecn mode (copy from outer/standard)
    tunnel_attrs[2].id = SAI_TUNNEL_ATTR_DECAP_ECN_MODE;
    if (ecn == "copy_from_outer") 
    {
        tunnel_attrs[2].value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER;
    }
    else if (ecn == "standard") 
    {
        tunnel_attrs[2].value.s32 = SAI_TUNNEL_DECAP_ECN_MODE_STANDARD;
    }

    // ttl mode (uniform/pipe)
    tunnel_attrs[3].id = SAI_TUNNEL_ATTR_DECAP_TTL_MODE;
    if (ttl == "uniform") 
    {
        tunnel_attrs[3].value.s32 = SAI_TUNNEL_TTL_UNIFORM_MODEL;
    }
    else if (ttl == "pipe") 
    {
        tunnel_attrs[3].value.s32 = SAI_TUNNEL_TTL_PIPE_MODEL;
    }

    // dscp mode (uniform/pipe)
    tunnel_attrs[4].id = SAI_TUNNEL_ATTR_DECAP_DSCP_MODE;
    if (dscp == "uniform") 
    {
        tunnel_attrs[4].value.s32 = SAI_TUNNEL_DSCP_UNIFORM_MODEL;
    }
    else if (dscp == "pipe") 
    {
        tunnel_attrs[4].value.s32 = SAI_TUNNEL_DSCP_PIPE_MODEL;
    }

    // write attributes to ASIC_DB
    sai_object_id_t tunnel_id;
    sai_status_t status = sai_tunnel_api->create_tunnel(&tunnel_id, length_of_tunnel_attrs, tunnel_attrs);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel");
        return false;
    }

    // TODO:
    // check if the database is already populated with the correct data
    // don't create duplicate entries
    // there should also be "business logic" for netbouncer in the "tunnel application" code, which is a different source file and daemon process

    // adding tunnel table entry attributes to array and writing to ASIC_DB
    int length_of_tunnel_table_entry_attrs = 5;
    sai_attribute_t tunnel_table_entry_attrs[length_of_tunnel_table_entry_attrs];
    tunnel_table_entry_attrs[0].id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    tunnel_table_entry_attrs[0].value.oid = gVirtualRouterId;
    tunnel_table_entry_attrs[1].id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    tunnel_table_entry_attrs[1].value.u32 = SAI_TUNNEL_TERM_TABLE_ENTRY_P2MP;
    tunnel_table_entry_attrs[2].id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
    tunnel_table_entry_attrs[3].id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    tunnel_table_entry_attrs[3].value.s32 = SAI_TUNNEL_IPINIP;
    tunnel_table_entry_attrs[4].id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    tunnel_table_entry_attrs[4].value.oid = tunnel_id;

    sai_ip_address_t tunnel_dst_ip;
    tunnel_dst_ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;

    set<IpAddress> tunnel_ips = dst_ip.getIpAddresses();
    struct sockaddr_in tunnel_ip_struct;
    string ip;

    // loop through the IP list and create a new tunnel table entry for every IP (in network byte order)
    for (auto it = tunnel_ips.begin(); it != tunnel_ips.end(); ++it)
    {
        ip = it->to_string();
        // turn string ip into network byte order
        inet_pton(AF_INET, ip.c_str(), &(tunnel_ip_struct.sin_addr));
        tunnel_dst_ip.addr.ip4 = tunnel_ip_struct.sin_addr.s_addr;

        tunnel_table_entry_attrs[2].value.ipaddr = tunnel_dst_ip;

        // create the tunnel table entry
        sai_object_id_t tunnel_term_table_entry_id;
        status = sai_tunnel_api->create_tunnel_term_table_entry(&tunnel_term_table_entry_id, length_of_tunnel_table_entry_attrs, tunnel_table_entry_attrs);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create tunnel entry table for ip: %s", ip.c_str());
            return false;
        }

    }
    return true;
}
