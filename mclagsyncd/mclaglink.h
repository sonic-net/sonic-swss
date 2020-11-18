 /* Copyright(c) 2016-2019 Nephos.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 *  Maintainer: Jim Jiang from nephos
 */

#ifndef __MCLAGLINK__
#define __MCLAGLINK__

#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <exception>
#include <string>
#include <map>
#include <set>

#include "producerstatetable.h"
#include "subscriberstatetable.h"
#include "select.h"
#include "selectable.h"
#include "redisclient.h"
#include "mclagsyncd/mclag.h"
#include "notificationconsumer.h"
#include "notificationproducer.h"

#define ETHER_ADDR_LEN 6

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif /* INET_ADDRSTRLEN */

#define MAX_L_PORT_NAME 20
#define BRCM_PLATFORM_SUBSTRING "broadcom"

namespace swss {

struct mclag_fdb_info
{
    uint8_t     mac[ETHER_ADDR_LEN];
    unsigned int vid;
    char port_name[MAX_L_PORT_NAME];
    short type;/*dynamic or static*/
    short op_type;/*add or del*/
};

struct mclag_domain_cfg_info
{
    int op_type;/*add/del domain; add/del mclag domain */
    int domain_id;
    int keepalive_time;
    int session_timeout;
    char local_ip[INET_ADDRSTRLEN];
    char peer_ip[INET_ADDRSTRLEN];
    char peer_ifname[MAX_L_PORT_NAME];
    uint8_t  system_mac[ETHER_ADDR_LEN];
    int attr_bmap;
};

struct mclag_iface_cfg_info
{
    int op_type;/*add/del domain; add/del mclag iface */
    int domain_id;
    char mclag_iface[MAX_L_PORT_NAME];
};

struct mclag_unique_ip_cfg_info
{
    int op_type;/*add/del mclag unique ip iface */
    char mclag_unique_ip_ifname[MAX_L_PORT_NAME];
};

struct mclag_vlan_mbr_info
{
    int op_type;/*add/del vlan_member */
    unsigned int vid;
    char mclag_iface[MAX_L_PORT_NAME];
};


struct mclag_fdb
{
    std::string mac;
    unsigned int vid;
    std::string port_name;
    std::string type;/*dynamic or static*/

    mclag_fdb(std::string val_mac, unsigned int val_vid, std::string val_pname,
              std::string val_type) : mac(val_mac), vid(val_vid), port_name(val_pname), type(val_type)
    {
    }
    mclag_fdb()
    {
    }

    bool operator <(const mclag_fdb &fdb) const
    {
        if (mac != fdb.mac)
            return mac < fdb.mac;
        else if (vid != fdb.vid)
            return vid < fdb.vid;
        else
            return port_name < fdb.port_name;
        //else if (port_name != fdb.port_name) return port_name < fdb.port_name;
        //else return type <fdb.type;
    }

    bool operator ==(const mclag_fdb &fdb) const
    {
        if (mac != fdb.mac)
            return 0;
        if (vid != fdb.vid)
            return 0;
        return 1;
    }

};


//MCLAG Domain Key 
struct mclagDomainEntry
{
   unsigned int domain_id;

   mclagDomainEntry() {}
   mclagDomainEntry(unsigned int id):domain_id(id) {}

   bool operator <(const mclagDomainEntry &domain) const
   {
       return domain_id < domain.domain_id;
   }

   bool operator ==(const mclagDomainEntry &domain) const
   {
       if (domain_id != domain.domain_id)
           return 0;
       return 1;
   }
};


//MCLAG Domain Data
struct mclagDomainData
{
    std::string source_ip;
    std::string peer_ip;
    std::string peer_link;
    int keepalive_interval;
    int session_timeout;

    mclagDomainData()
    {
        keepalive_interval = -1;
        session_timeout    = -1;
    }

    bool mandatoryFieldsPresent() const
    {
        if (!source_ip.empty() && !peer_ip.empty())
            return 1;
        return 0;
    }

    bool allFieldsEmpty() const
    {
        if (source_ip.empty() && peer_ip.empty() && peer_link.empty()
                && keepalive_interval == -1 && session_timeout == -1)
            return 1;
        return 0;
    }
};

typedef  std::tuple<std::string, std::string> vlan_mbr;

class MclagLink : public Selectable {
public:
    const int MSG_BATCH_SIZE;
    std::map<std::string, std:: string> *p_learn;
    ProducerStateTable * p_port_tbl;
    ProducerStateTable * p_lag_tbl;
    ProducerStateTable * p_tnl_tbl;
    ProducerStateTable * p_intf_tbl;
    ProducerStateTable *p_fdb_tbl;
    ProducerStateTable *p_acl_table_tbl;
    ProducerStateTable *p_acl_rule_tbl;
    Table *p_mclag_tbl;
    Table *p_mclag_local_intf_tbl;
    Table *p_mclag_remote_intf_tbl;
    Table *p_device_metadata_tbl;
    Table *p_mclag_cfg_table;
    Table *p_mclag_intf_cfg_table;
    Table *p_state_vlan_mbr_table;
    Table *p_state_fdb_table;
    DBConnector *p_appl_db;
    DBConnector *p_asic_db; /*redis client access to ASIC_DB*/
    DBConnector *p_counters_db; /*redis client access to COUNTERS_DB*/

    ProducerStateTable *p_iso_grp_tbl;

    SubscriberStateTable *p_mclag_intf_cfg_tbl;
    SubscriberStateTable *p_state_fdb_tbl;
    SubscriberStateTable *p_state_vlan_mbr_subscriber_table;

    std::map<mclagDomainEntry, mclagDomainData> m_mclag_domains;

    MclagLink(Select* select, int port = MCLAG_DEFAULT_PORT);

    virtual ~MclagLink();

    /* Wait for connection (blocking) */
    void accept();

    int getFd() override;
    char* getSendMsgBuffer();
    int getConnSocket();
    uint64_t readData() override; 
    void mclagsyncd_fetch_system_mac_from_configdb();
    void mclagsyncd_fetch_mclag_config_from_configdb();
    void mclagsyncd_fetch_mclag_interface_config_from_configdb();
    void mclagsyncd_fetch_fdb_entries_from_statedb();
    void mclagsyncd_fetch_vlan_mbr_table_from_statedb();

    void mclagsyncd_send_fdb_entries(std::deque<KeyOpFieldsValuesTuple> &entries);
    void mclagsyncd_send_mclag_iface_cfg(std::deque<KeyOpFieldsValuesTuple> &entries);
    void mclagsyncd_send_mclag_unique_ip_cfg(std::deque<KeyOpFieldsValuesTuple> &entries);

    void processMclagDomainCfg(std::deque<KeyOpFieldsValuesTuple> &entries);
    void processVlanMemberTableUpdates(std::deque<KeyOpFieldsValuesTuple> &entries);

    void addDomainCfgDependentSelectables();

    void delDomainCfgDependentSelectables();

    /* readMe throws MclagConnectionClosedException when connection is lost */
    class MclagConnectionClosedException : public std::exception
    {
    };

private:
    Select *m_select;
    unsigned int m_bufSize;
    char *m_messageBuffer;
    char *m_messageBuffer_send;
    unsigned int m_pos;

    bool m_connected;
    bool m_server_up;
    int m_server_socket;
    int m_connection_socket;
    
    bool is_iccp_up = false;
    std::string m_system_mac;
    std::set<vlan_mbr> m_vlan_mbrship; //set of vlan,mbr tuples

    void getOidToPortNameMap(std::unordered_map<std::string, std:: string> & port_map);
    void getBridgePortIdToAttrPortIdMap(std::map<std::string, std:: string> *oid_map);
    void getVidByBvid(std::string &bvid, std::string &vlanid);
    void getFdbSet(std::set<mclag_fdb> *fdb_set);
    void setLocalIfPortIsolate(std::string mclag_if, bool is_enable);
    void deleteLocalIfPortIsolate(std::string mclag_if);
    void setPortIsolate(char *msg);
    void setPortMacLearnMode(char *msg);
    void setPortMacLearnNLAPI(char *msg);
    void setFdbFlush();
    void setFdbFlushByPort(char *msg);
    void setIntfMac(char *msg);
    void setFdbEntry(char *msg, int msg_len);

    void addVlanMbr(std::string, std::string);
    void delVlanMbr(std::string, std::string);
    int  findVlanMbr(std::string, std::string);

    void mclagsyncd_set_traffic_disable(char *msg_buf, uint8_t msg_type);
    void mclagsyncd_set_iccp_state(char *msg, size_t msg_size);
    void mclagsyncd_set_iccp_role(char *msg, size_t msg_size);
    void mclagsyncd_set_system_id(char *msg, size_t msg_size);
    void mclagsyncd_del_iccp_info(char *msg);
    void mclagsyncd_set_remote_if_state(char *msg, size_t msg_size);
    void mclagsyncd_del_remote_if_info(char *msg, size_t msg_size);
    void mclagsyncd_set_peer_link_isolation(char *msg, size_t msg_size);
    void mclagsyncd_set_peer_system_id(char *msg, size_t msg_size);
};

}
#endif


