/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __NATMGR__
#define __NATMGR__

#include "selectabletimer.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"
#include "notificationproducer.h"
#include "timer.h"
#include <unistd.h>
#include <set>
#include <map>
#include <string>

namespace swss {

#define STATIC_NAT_KEY_SIZE        1
#define LOCAL_IP                   "local_ip"
#define TRANSLATED_IP              "translated_ip"
#define NAT_TYPE                   "nat_type"
#define SNAT_NAT_TYPE              "snat"
#define DNAT_NAT_TYPE              "dnat"
#define TWICE_NAT_ID               "twice_nat_id"
#define TWICE_NAT_ID_MIN           1
#define TWICE_NAT_ID_MAX           9999
#define ENTRY_TYPE                 "entry_type"
#define STATIC_ENTRY_TYPE          "static"
#define DYNAMIC_ENTRY_TYPE         "dynamic" 
#define STATIC_NAPT_KEY_SIZE       3
#define LOCAL_PORT                 "local_port"
#define TRANSLATED_L4_PORT         "translated_l4_port"
#define TRANSLATED_SRC_IP          "translated_src_ip"
#define TRANSLATED_SRC_L4_PORT     "translated_src_l4_port"
#define TRANSLATED_DST_IP          "translated_dst_ip"
#define TRANSLATED_DST_L4_PORT     "translated_dst_l4_port"
#define POOL_TABLE_KEY_SIZE        1
#define NAT_IP                     "nat_ip"
#define NAT_PORT                   "nat_port"
#define BINDING_TABLE_KEY_SIZE     1
#define NAT_POOL                   "nat_pool"
#define NAT_ACLS                   "access_list"
#define VALUES                     "Values"
#define NAT_ADMIN_MODE             "admin_mode"
#define NAT_ZONE                   "nat_zone"
#define NAT_TIMEOUT                "nat_timeout"
#define NAT_TIMEOUT_MIN            300
#define NAT_TIMEOUT_MAX            432000
#define NAT_TIMEOUT_DEFAULT        600
#define NAT_TIMEOUT_LOW            0
#define NAT_TCP_TIMEOUT            "nat_tcp_timeout"
#define NAT_TCP_TIMEOUT_MIN        300
#define NAT_TCP_TIMEOUT_MAX        432000
#define NAT_TCP_TIMEOUT_DEFAULT    86400
#define NAT_UDP_TIMEOUT            "nat_udp_timeout" 
#define NAT_UDP_TIMEOUT_MIN        120
#define NAT_UDP_TIMEOUT_MAX        600
#define NAT_UDP_TIMEOUT_DEFAULT    300
#define L3_INTERFACE_KEY_SIZE      2
#define L3_INTERFACE_ZONE_SIZE     1
#define VLAN_PREFIX                "Vlan"
#define LAG_PREFIX                 "PortChannel"
#define ETHERNET_PREFIX            "Ethernet"
#define LOOPBACK_PREFIX            "Loopback"
#define ACL_TABLE_KEY_SIZE         1
#define TABLE_TYPE                 "TYPE"
#define TABLE_STAGE                "STAGE"
#define TABLE_PORTS                "PORTS"
#define TABLE_TYPE_L3              "L3"
#define TABLE_STAGE_INGRESS        "INGRESS"
#define ACL_RULE_TABLE_KEY_SIZE    2
#define ACTION_PACKET_ACTION       "PACKET_ACTION"
#define PACKET_ACTION_FORWARD      "FORWARD"
#define PACKET_ACTION_DO_NOT_NAT   "DO_NOT_NAT"
#define MATCH_IP_TYPE              "IP_TYPE"
#define IP_TYPE_IP                 "IP"
#define IP_TYPE_IPv4ANY            "IPV4ANY"
#define RULE_PRIORITY              "PRIORITY"
#define MATCH_SRC_IP               "SRC_IP"
#define MATCH_DST_IP               "DST_IP"
#define MATCH_IP_PROTOCOL          "IP_PROTOCOL"
#define MATCH_IP_PROTOCOL_ICMP     1
#define MATCH_IP_PROTOCOL_TCP      6
#define MATCH_IP_PROTOCOL_UDP      17          
#define MATCH_L4_SRC_PORT          "L4_SRC_PORT"
#define MATCH_L4_DST_PORT          "L4_DST_PORT"
#define MATCH_L4_SRC_PORT_RANGE    "L4_SRC_PORT_RANGE"
#define MATCH_L4_DST_PORT_RANGE    "L4_DST_PORT_RANGE"
#define IP_PREFIX_SIZE             2
#define IP_ADDR_MASK_LEN_MIN       1
#define IP_ADDR_MASK_LEN_MAX       32
#define IP_PROTOCOL_ICMP           "icmp"
#define IP_PROTOCOL_TCP            "tcp"
#define IP_PROTOCOL_UDP            "udp"
#define L4_PORT_MIN                1
#define L4_PORT_MAX                65535
#define L4_PORT_RANGE_SIZE         2
#define EMPTY_STRING               ""
#define NONE_STRING                "None"
#define ADD                        "A"
#define INSERT                     "I"
#define DELETE                     "D"
#define ENABLED                    "enabled"
#define DISABLED                   "disabled"
#define IS_LOOPBACK_ADDR(ipaddr)   ((ipaddr & 0xFF000000) == 0x7F000000)
#define IS_MULTICAST_ADDR(ipaddr)  ((ipaddr >= 0xE0000000) and (ipaddr <= 0xEFFFFFFF))
#define IS_RESERVED_ADDR(ipaddr)   (ipaddr >= 0xF0000000)
#define IS_ZERO_ADDR(ipaddr)       (ipaddr == 0)
#define IS_BROADCAST_ADDR(ipaddr)  (ipaddr == 0xFFFFFFFF)
#define NAT_ENTRY_REFRESH_PERIOD   86400    // 1 day
#define REDIRECT_TO_DEV_NULL       " &> /dev/null"
#define FLUSH                      " -F"

const char ip_address_delimiter = '/';

/* Pool Info */
typedef struct {
    std::string ip_range;
    std::string port_range;
} natPool_t;

/* Binding Info */
typedef struct {
    std::string pool_name;
    std::string acl_name;
    std::string nat_type;
    std::string twice_nat_id;
    std::string pool_interface;
    std::string acl_interface;
    std::string static_key;
    bool        twice_nat_added;
} natBinding_t;

/* Static NAT Entry Info */
typedef struct {
    std::string local_ip;
    std::string nat_type;
    std::string twice_nat_id;
    std::string interface;
    std::string binding_key;
    bool        twice_nat_added;
} staticNatEntry_t;

/* Static NAPT Entry Info */
typedef struct {
    std::string local_ip;
    std::string local_port;
    std::string nat_type;
    std::string twice_nat_id;
    std::string interface;
    std::string binding_key;
    bool        twice_nat_added;
} staticNaptEntry_t;

/* NAT ACL Table Rules Info */
typedef struct{
    std::string packet_action;
    uint32_t    priority;
    std::string src_ip_range;
    std::string dst_ip_range;
    std::string src_l4_port_range;
    std::string dst_l4_port_range;
    std::string ip_protocol;
} natAclRule_t;

/* Containers to store NAT Info */

/* To store NAT Pool configuration,
 * Key is "Pool_name"
 * Value is "natPool_t"
 */
typedef std::map<std::string, natPool_t> natPool_map_t;

/* To store NAT Binding configuration,
 * Key is "Binding_name"
 * Value is "natBinding_t"
 */
typedef std::map<std::string, natBinding_t> natBinding_map_t;

/* To store Static NAT configuration,
 * Key is "Global_ip" (Eg. 65.55.45.1)
 * Value is "staticNatEntry_t"
 */
typedef std::map<std::string, staticNatEntry_t> staticNatEntry_map_t;

/* To store Static NAPT configuration,
 * Key is "Global_ip|ip_protocol|Global_port" (Eg. 65.55.45.1|TCP|500)
 * Value is "staticNaptEntry_t"
 */
typedef std::map<std::string, staticNaptEntry_t> staticNaptEntry_map_t;

/* To store NAT Ip Interface configuration,
 * Key is "Port" (Eg. Ethernet1)
 * Value is "ip_address_list" (Eg. 10.0.0.1/24,20.0.0.1/24)
 */
typedef std::map<std::string, std::set<std::string>> natIpInterface_map_t;

/* To store NAT ACL Table configuration,
 * Key is "ACL_Table_Id" (Eg. 1)
 * Value is "ports" (Eg. Ethernet4,Vlan10)
 */
typedef std::map<std::string, std::string> natAclTable_map_t;

/* To store NAT ACL Rules configuration,
 * Key is "ACL_Tabel_Id|ACL_Rule_Id" (Eg. 1|1)
 * Value is "natAclRule_t"
 */
typedef std::map<std::string, natAclRule_t> natAclRule_map_t;

/* To store NAT Zone Interface configuration,
 * Key is "Port" (Eg. Ethernet1)
 * Value is "nat_zone" (Eg. "1")
 */
typedef std::map<std::string, std::string> natZoneInterface_map_t;

/* Define NatMgr Class inherited from Orch Class */
class NatMgr : public Orch
{
public:
    /* NatMgr Constructor */
    NatMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask; 

    /* Function to be called from signal handler on nat docker stop */
    void cleanupPoolIpTable();
    void cleanupMangleIpTables();
    bool isPortInitDone(DBConnector *app_db);
    void timeoutNotifications(std::string op, std::string data);
    void flushNotifications(std::string op, std::string data);
    void removeStaticNatIptables(const std::string port = NONE_STRING);
    void removeStaticNaptIptables(const std::string port = NONE_STRING);
    void removeDynamicNatRules(const std::string port = NONE_STRING, const std::string ipPrefix = NONE_STRING);

private:
    /* Declare APPL_DB, CFG_DB and STATE_DB tables */
    ProducerStateTable m_appNatTableProducer, m_appNaptTableProducer, m_appNatGlobalTableProducer;
    ProducerStateTable m_appTwiceNatTableProducer, m_appTwiceNaptTableProducer;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateInterfaceTable, m_appNaptPoolIpTable;
    Table m_stateWarmRestartEnableTable, m_stateWarmRestartTable;

    /* Declare containers to store NAT Info */
    int          m_natTimeout;
    int          m_natTcpTimeout;
    int          m_natUdpTimeout;
    std::string  natAdminMode;

    natPool_map_t            m_natPoolInfo;
    natBinding_map_t         m_natBindingInfo;
    staticNatEntry_map_t     m_staticNatEntry;
    staticNaptEntry_map_t    m_staticNaptEntry;
    natIpInterface_map_t     m_natIpInterfaceInfo;
    natZoneInterface_map_t   m_natZoneInterfaceInfo;
    natAclTable_map_t        m_natAclTableInfo;
    natAclRule_map_t         m_natAclRuleInfo;
    SelectableTimer          *m_natRefreshTimer;

    /* Declare doTask related fucntions */
    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);
    void doNatRefreshTimerTask();
    void doStaticNatTask(Consumer &consumer);
    void doStaticNaptTask(Consumer &consumer);
    void doNatPoolTask(Consumer &consumer);
    void doNatBindingTask(Consumer &consumer);
    void doNatGlobalTask(Consumer &consumer);
    void doNatIpInterfaceTask(Consumer &consumer);
    void doNatAclTableTask(Consumer &consumer);
    void doNatAclRuleTask(Consumer &consumer);

    /* Declare all NAT functionality member functions*/
    void enableNatFeature(void);
    void disableNatFeature(void);
    bool warmBootingInProgress(void);
    void flushAllNatEntries(void);
    void addAllStaticConntrackEntries(void);
    void addConntrackStaticSingleNatEntry(const std::string &key);
    void addConntrackStaticSingleNaptEntry(const std::string &key);
    void updateConntrackStaticSingleNatEntry(const std::string &key);
    void updateConntrackStaticSingleNaptEntry(const std::string &key);
    void deleteConntrackStaticSingleNatEntry(const std::string &key);
    void deleteConntrackStaticSingleNaptEntry(const std::string &key);
    void addConntrackStaticTwiceNatEntry(const std::string &snatKey, const std::string &dnatKey);
    void addConntrackStaticTwiceNaptEntry(const std::string &snatKey, const std::string &dnatKey);
    void updateConntrackStaticTwiceNatEntry(const std::string &snatKey, const std::string &dnatKey);
    void updateConntrackStaticTwiceNaptEntry(const std::string &snatKey, const std::string &dnatKey);
    void deleteConntrackStaticTwiceNatEntry(const std::string &snatKey, const std::string &dnatKey);
    void deleteConntrackStaticTwiceNaptEntry(const std::string &snatKey, const std::string &dnatKey);
    void deleteConntrackDynamicEntries(const std::string &ip_range);
    void updateDynamicSingleNatConnTrackTimeout(std::string key, int timeout);
    void updateDynamicSingleNaptConnTrackTimeout(std::string key, int timeout);
    void updateDynamicTwiceNatConnTrackTimeout(std::string key, int timeout);
    void updateDynamicTwiceNaptConnTrackTimeout(std::string key, int timeout);
    void addStaticNatEntry(const std::string &key);
    void addStaticNaptEntry(const std::string &key);
    void addStaticSingleNatEntry(const std::string &key);
    void addStaticSingleNaptEntry(const std::string &key);
    void addStaticSingleNatIptables(const std::string &key);
    void addStaticSingleNaptIptables(const std::string &key);
    void addStaticTwiceNatEntry(const std::string &key);
    void addStaticTwiceNaptEntry(const std::string &key);
    void addStaticTwiceNatIptables(const std::string &key);
    void addStaticTwiceNaptIptables(const std::string &key);
    void removeStaticNatEntry(const std::string &key);
    void removeStaticNaptEntry(const std::string &key);
    void removeStaticSingleNatEntry(const std::string &key);
    void removeStaticSingleNaptEntry(const std::string &key);
    void removeStaticSingleNatIptables(const std::string &key);
    void removeStaticSingleNaptIptables(const std::string &key);
    void removeStaticTwiceNatEntry(const std::string &key);
    void removeStaticTwiceNaptEntry(const std::string &key);
    void removeStaticTwiceNatIptables(const std::string &key);
    void removeStaticTwiceNaptIptables(const std::string &key);
    void addStaticNatEntries(const std::string port = NONE_STRING, const std::string ipPrefix = NONE_STRING);
    void addStaticNaptEntries(const std::string port = NONE_STRING, const std::string ipPrefix = NONE_STRING);
    void removeStaticNatEntries(const std::string port = NONE_STRING, const std::string ipPrefix = NONE_STRING);
    void removeStaticNaptEntries(const std::string port= NONE_STRING, const std::string ipPrefix = NONE_STRING);
    void addStaticNatIptables(const std::string port);
    void addStaticNaptIptables(const std::string port);
    void setStaticNatConntrackEntries(std::string mode);
    void setStaticSingleNatConntrackEntry(const std::string &key, std::string &mode);
    void setStaticTwiceNatConntrackEntry(const std::string &key, std::string &mode);
    void setStaticNaptConntrackEntries(std::string mode);
    void setStaticSingleNaptConntrackEntry(const std::string &key, std::string &mode);
    void setStaticTwiceNaptConntrackEntry(const std::string &key, std::string &mode);
    void addDynamicNatRule(const std::string &key);
    void removeDynamicNatRule(const std::string &key);
    void addDynamicNatRuleByAcl(const std::string &key, bool isRuleId = false);
    void removeDynamicNatRuleByAcl(const std::string &key, bool isRuleId = false);
    void addDynamicNatRules(const std::string port = NONE_STRING, const std::string ipPrefix = NONE_STRING);
    void addDynamicTwiceNatRule(const std::string &key);
    void deleteDynamicTwiceNatRule(const std::string &key);
    void setDynamicAllForwardOrAclbasedRules(const std::string &opCmd, const std::string &pool_interface, const std::string &ip_range,
                                             const std::string &port_range, const std::string &acls_name, const std::string &dynamicKey);

    bool isNatEnabled(void);
    bool isPortStateOk(const std::string &alias);
    bool isIntfStateOk(const std::string &alias); 
    bool isPoolMappedtoBinding(const std::string &pool_name, std::string &binding_name); 
    bool isMatchesWithStaticNat(const std::string &global_ip, std::string &local_ip);
    bool isMatchesWithStaticNapt(const std::string &global_ip, std::string &local_ip);
    bool isGlobalIpMatching(const std::string &intf_keys, const std::string &global_ip);
    bool getIpEnabledIntf(const std::string &global_ip, std::string &interface);
    void setNaptPoolIpTable(const std::string &opCmd, const std::string &nat_ip, const std::string &nat_port);
    bool setFullConeDnatIptablesRule(const std::string &opCmd);
    bool setMangleIptablesRules(const std::string &opCmd, const std::string &interface, const std::string &nat_zone);
    bool setStaticNatIptablesRules(const std::string &opCmd, const std::string &interface, const std::string &external_ip, const std::string &internal_ip, const std::string &nat_type);
    bool setStaticNaptIptablesRules(const std::string &opCmd, const std::string &interface, const std::string &prototype, const std::string &external_ip, 
                                    const std::string &external_port, const std::string &internal_ip, const std::string &internal_port, const std::string &nat_type);
    bool setStaticTwiceNatIptablesRules(const std::string &opCmd, const std::string &interface, const std::string &src_ip, const std::string &translated_src_ip,
                                        const std::string &dest_ip, const std::string &translated_dest_ip);
    bool setStaticTwiceNaptIptablesRules(const std::string &opCmd, const std::string &interface, const std::string &prototype, const std::string &src_ip, const std::string &src_port,
                                         const std::string &translated_src_ip, const std::string &translated_src_port, const std::string &dest_ip, const std::string &dest_port,
                                         const std::string &translated_dest_ip, const std::string &translated_dest_port);
    bool setDynamicNatIptablesRulesWithAcl(const std::string &opCmd, const std::string &interface, const std::string &external_ip,
                                           const std::string &external_port_range, natAclRule_t &natAclRuleId, const std::string &static_key);
    bool setDynamicNatIptablesRulesWithoutAcl(const std::string &opCmd, const std::string &interface, const std::string &external_ip,
                                              const std::string &external_port_range, const std::string &static_key);

};

}

#endif
