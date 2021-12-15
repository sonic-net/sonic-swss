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

#include <string.h>
#include <errno.h>
#include <system_error>
#include <stdlib.h>
#include <iostream>
#include "logger.h"
#include "tokenize.h"
#include "netmsg.h"
#include "netdispatcher.h"
#include "swss/notificationproducer.h"
#include "mclagsyncd/mclaglink.h"
#include "mclagsyncd/mclag.h"
#include <set>
#include <algorithm>
#include "macaddress.h"
#include <string>
#include <iosfwd>
#include <iostream>
#include <sstream>
#include "table.h"

using namespace swss;
using namespace std;

void MclagLink::addVlanMbr(std::string vlan, std::string mbr_port)
{
    m_vlan_mbrship.emplace(vlan_mbr(vlan,mbr_port));
}

//returns 1 if present, else returns zero
int MclagLink::findVlanMbr(std::string vlan, std::string mbr_port)
{
    return (m_vlan_mbrship.find(vlan_mbr(vlan,mbr_port))  != m_vlan_mbrship.end());
}


void MclagLink::delVlanMbr(std::string vlan, std::string mbr_port)
{
    m_vlan_mbrship.erase(vlan_mbr(vlan,mbr_port));
}


void MclagLink::getOidToPortNameMap(std::unordered_map<std::string, std:: string> & port_map)
{
    auto hash = p_counters_db->hgetall("COUNTERS_PORT_NAME_MAP");

    for (auto it = hash.begin(); it != hash.end(); ++it)
        port_map.insert(pair<string, string>(it->second, it->first));

    return;
}

void MclagLink::getBridgePortIdToAttrPortIdMap(std::map<std::string, std:: string> *oid_map)
{
    std::string bridge_port_id;
    size_t pos1 = 0;

    auto keys = p_asic_db->keys("ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT:*");

    for (auto& key : keys)
    {
        pos1 = key.find("oid:", 0);
        bridge_port_id = key.substr(pos1);

        auto hash = p_asic_db->hgetall(key);
        auto attr_port_id = hash.find("SAI_BRIDGE_PORT_ATTR_PORT_ID");
        if (attr_port_id == hash.end())
        {
            attr_port_id = hash.find("SAI_BRIDGE_PORT_ATTR_TUNNEL_ID");
            if (attr_port_id == hash.end())
                continue;
        }

        oid_map->insert(pair<string, string>(bridge_port_id, attr_port_id->second));
    }

    return;
}

void MclagLink::getVidByBvid(std::string &bvid, std::string &vlanid)
{
    std::string pre = "ASIC_STATE:SAI_OBJECT_TYPE_VLAN:";
    std::string key = pre + bvid;

    auto hash = p_asic_db->hgetall(key.c_str());

    auto attr_vlan_id = hash.find("SAI_VLAN_ATTR_VLAN_ID");
    if (attr_vlan_id == hash.end())
        return;

    vlanid = attr_vlan_id->second;
    return;
}

void MclagLink::mclagsyncdFetchSystemMacFromConfigdb()
{
    vector<FieldValueTuple> fvs; 
    p_device_metadata_tbl->get("localhost",fvs);
    auto it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) {
            return fv.first == "mac";
            });


    if (it == fvs.end())
    {
        SWSS_LOG_ERROR("mclagsyncd: Failed to get MAC address from configuration database");
        return;
    }

    m_system_mac = it->second;  
    SWSS_LOG_NOTICE("mclagysncd: system_mac:%s ",m_system_mac.c_str());
    return;
}


void MclagLink::mclagsyncdFetchMclagConfigFromConfigdb()
{
    TableDump mclag_cfg_dump;
    SWSS_LOG_NOTICE("mclag cfg dump....");
    p_mclag_cfg_table->dump(mclag_cfg_dump);


    std::deque<KeyOpFieldsValuesTuple> entries;
    for (const auto&key: mclag_cfg_dump)
    {
        KeyOpFieldsValuesTuple cfgentry;
        SWSS_LOG_NOTICE("Key: %s", key.first.c_str());
        kfvKey(cfgentry) = key.first;
        kfvOp(cfgentry) = "SET";
        SWSS_LOG_NOTICE("Value:");
        for (const auto& val : key.second) {
            SWSS_LOG_NOTICE("%s: %s", val.first.c_str(), val.second.c_str());
            FieldValueTuple value;
            fvField(value) = val.first;
            fvValue(value) = val.second;
            kfvFieldsValues(cfgentry).push_back(value);
        }
        entries.push_back(cfgentry);
        processMclagDomainCfg(entries);
    }
}

void MclagLink::mclagsyncdFetchMclagInterfaceConfigFromConfigdb()
{
    TableDump mclag_intf_cfg_dump;
    SWSS_LOG_NOTICE("mclag cfg dump....");
    p_mclag_intf_cfg_table->dump(mclag_intf_cfg_dump);

    std::deque<KeyOpFieldsValuesTuple> entries;
    for (const auto&key: mclag_intf_cfg_dump)
    {
        KeyOpFieldsValuesTuple cfgentry;
        SWSS_LOG_NOTICE("Key: %s", key.first.c_str());
        kfvKey(cfgentry) = key.first;
        kfvOp(cfgentry) = "SET";
        SWSS_LOG_NOTICE("Value:");
        for (const auto& val : key.second) {
            SWSS_LOG_NOTICE("%s: %s", val.first.c_str(), val.second.c_str());
            FieldValueTuple value;
            fvField(value) = val.first;
            fvValue(value) = val.second;
            kfvFieldsValues(cfgentry).push_back(value);
        }
        entries.push_back(cfgentry);
        mclagsyncdSendMclagIfaceCfg(entries);
    }
}

void MclagLink::setPortIsolate(char *msg)
{
    char *platform = getenv("platform");
    if ((NULL != platform) && (strstr(platform, BRCM_PLATFORM_SUBSTRING)))
    {
        mclag_sub_option_hdr_t *op_hdr = NULL;
        string isolate_src_port;
        string isolate_dst_port;
        char * cur = NULL;
        vector<FieldValueTuple> fvts;

        cur = msg;


        /*get isolate src port infor*/
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
        isolate_src_port.insert(0, (const char*)cur, op_hdr->op_len);

        cur = cur + op_hdr->op_len;

        /*get isolate dst ports infor*/
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        cur = cur + MCLAG_SUB_OPTION_HDR_LEN;


        if (op_hdr->op_len == 0)
        {
            /*Destination port can be empty when all remote MLAG interfaces
             * goes down down or ICCP session goes down. Do not delete the
             * isolation group when all remote interfaces are down. Just remove
             * all destination ports from the group
             */

            if (is_iccp_up)
            {
                fvts.emplace_back("DESCRIPTION", "Isolation group for MCLAG");
                fvts.emplace_back("TYPE", "bridge-port");
                fvts.emplace_back("PORTS", isolate_src_port);
                fvts.emplace_back("MEMBERS", isolate_dst_port);
                p_iso_grp_tbl->set("MCLAG_ISO_GRP", fvts);
                SWSS_LOG_NOTICE("Delete all isolation group destination ports");
            }
            else
            {
                p_iso_grp_tbl->del("MCLAG_ISO_GRP");
                SWSS_LOG_NOTICE("Isolation group deleted");
            }
        }
        else
        {
            string temp;

            isolate_dst_port.insert(0, (const char*)cur, op_hdr->op_len);
            istringstream dst_ss(isolate_dst_port);

            isolate_dst_port.clear();
            while (getline(dst_ss, temp, ','))
            {
                if (0 == temp.find("Ethernet"))
                {
                    continue;
                }
                if (isolate_dst_port.length())
                {
                    isolate_dst_port = isolate_dst_port + ',' + temp;
                }
                else
                {
                    isolate_dst_port = temp;
                }
            }

            fvts.emplace_back("DESCRIPTION", "Isolation group for MCLAG");
            fvts.emplace_back("TYPE", "bridge-port");
            fvts.emplace_back("PORTS", isolate_src_port);
            fvts.emplace_back("MEMBERS", isolate_dst_port);

            p_iso_grp_tbl->set("MCLAG_ISO_GRP", fvts);
            SWSS_LOG_NOTICE("Isolation group created with ports %s and members %s",
                    isolate_src_port.c_str(),
                    isolate_dst_port.c_str());
        }
    }
    else
    {
        mclag_sub_option_hdr_t *op_hdr = NULL;
        string isolate_src_port;
        string isolate_dst_port;
        char * cur = NULL;
        string acl_name = "mclag";
        string acl_rule_name = "mclag:mclag";
        vector<FieldValueTuple> acl_attrs;
        vector<FieldValueTuple> acl_rule_attrs;
        std::string acl_key = std::string("") + APP_ACL_TABLE_TABLE_NAME + ":" + acl_name;
        std::string acl_rule_key = std::string("") + APP_ACL_RULE_TABLE_NAME + ":" + acl_rule_name;
        static int acl_table_is_added = 0;

        cur = msg;

        /*get isolate src port infor*/
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
        isolate_src_port.insert(0, (const char*)cur, op_hdr->op_len);

        cur = cur + op_hdr->op_len;

        /*get isolate dst ports infor*/
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
        isolate_dst_port.insert(0, (const char*)cur, op_hdr->op_len);

        if (op_hdr->op_len == 0)
        {
            /* If dst port is NULL, delete the acl table 'mclag' */
            p_acl_table_tbl->del(acl_name);
            acl_table_is_added = 0;
            SWSS_LOG_NOTICE("set port isolate, src port: %s, dst port is NULL",
                    isolate_src_port.c_str());
            return;
        }

        SWSS_LOG_NOTICE("set port isolate, src port: %s, dst port: %s",
                isolate_src_port.c_str(), isolate_dst_port.c_str());

        if (acl_table_is_added == 0)
        {
            /*First create ACL table*/
            FieldValueTuple desc_attr("policy_desc", "Mclag egress port isolate acl");
            acl_attrs.push_back(desc_attr);

            FieldValueTuple type_attr("type", "L3");
            acl_attrs.push_back(type_attr);

            FieldValueTuple port_attr("ports", isolate_src_port);
            acl_attrs.push_back(port_attr);

            p_acl_table_tbl->set(acl_name, acl_attrs);

            acl_table_is_added = 1;
            /*End create ACL table*/
        }

        /*Then create ACL rule table*/
        FieldValueTuple ip_type_attr("IP_TYPE", "ANY");
        acl_rule_attrs.push_back(ip_type_attr);

        string temp;
        istringstream dst_ss(isolate_dst_port);

        isolate_dst_port.clear();
        while (getline(dst_ss, temp, ','))
        {
            if (0 == temp.find("PortChannel"))
            {
                continue;
            }

            if (isolate_dst_port.length())
            {
                isolate_dst_port = isolate_dst_port + ',' + temp;
            }
            else
            {
                isolate_dst_port = temp;
            }
        }

        FieldValueTuple out_port_attr("OUT_PORTS", isolate_dst_port);
        acl_rule_attrs.push_back(out_port_attr);

        FieldValueTuple packet_attr("PACKET_ACTION", "DROP");
        acl_rule_attrs.push_back(packet_attr);

        p_acl_rule_tbl->set(acl_rule_name, acl_rule_attrs);
        /*End create ACL rule table*/
    }

    return;
}

void MclagLink::setPortMacLearnMode(char *msg)
{
    string learn_port;
    string learn_mode;
    mclag_sub_option_hdr_t *op_hdr = NULL;
    char * cur = NULL;

    cur = msg;

    /*get port learning mode info*/
    op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
    if (op_hdr->op_type == MCLAG_SUB_OPTION_TYPE_MAC_LEARN_ENABLE)
    {
        learn_mode = "hardware";
    }
    else if (op_hdr->op_type == MCLAG_SUB_OPTION_TYPE_MAC_LEARN_DISABLE)
    {
        learn_mode = "disable";
    }

    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;

    learn_port.insert(0, (const char*)cur, op_hdr->op_len);

    vector<FieldValueTuple> attrs;
    FieldValueTuple learn_attr("learn_mode", learn_mode);
    attrs.push_back(learn_attr);
    if (strncmp(learn_port.c_str(), PORTCHANNEL_PREFIX, strlen(PORTCHANNEL_PREFIX)) == 0)
        p_lag_tbl->set(learn_port, attrs);
    /* vxlan tunnel is currently not supported, for src_ip is the mandatory attribute */
    /* else if(strncmp(learn_port.c_str(),VXLAN_TUNNEL_PREFIX,5)==0)
       p_tnl_tbl->set(learn_port, attrs); */
    else
        p_port_tbl->set(learn_port, attrs);

    SWSS_LOG_NOTICE("set port mac learn mode, port: %s, learn-mode: %s",
            learn_port.c_str(), learn_mode.c_str());

    return;
}

void MclagLink::setFdbFlush()
{
    swss::NotificationProducer flushFdb(p_appl_db.get(), "FLUSHFDBREQUEST");

    vector<FieldValueTuple> values;

    SWSS_LOG_NOTICE("send fdb flush notification");

    flushFdb.send("ALL", "ALL", values);

    return;
}


void MclagLink::setIntfMac(char *msg)
{
    mclag_sub_option_hdr_t *op_hdr = NULL;
    string intf_key;
    string mac_value;
    char *cur = NULL;

    cur = msg;

    /*get intf key name*/
    op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    intf_key.insert(0, (const char*)cur, op_hdr->op_len);

    cur = cur + op_hdr->op_len;

    /*get mac*/
    op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    mac_value.insert(0, (const char*)cur, op_hdr->op_len);

    SWSS_LOG_NOTICE("set mac to chip, intf key name: %s, mac: %s", intf_key.c_str(), mac_value.c_str());
    vector<FieldValueTuple> attrs;
    FieldValueTuple mac_attr("mac_addr", mac_value);
    attrs.push_back(mac_attr);
    p_intf_tbl->set(intf_key, attrs);

    return;
}

void MclagLink::setFdbEntry(char *msg, int msg_len)
{
    struct mclag_fdb_info * fdb_info = NULL;
    struct mclag_fdb fdb;
    string fdb_key;
    char key[64] = { 0 };
    char *cur = NULL;
    short count = 0;
    int index = 0;

    cur = msg;           
    count = (short)(msg_len/sizeof(struct mclag_fdb_info));

    for (index =0; index < count; index ++)
    {
        memset(key, 0, 64);

        fdb_info = reinterpret_cast<struct mclag_fdb_info *>(static_cast<void *>(cur + index * sizeof(struct mclag_fdb_info)));

        fdb.mac  = MacAddress::to_string(fdb_info->mac);
        fdb.port_name = fdb_info->port_name;
        fdb.vid = fdb_info->vid;
        if (fdb_info->type == MCLAG_FDB_TYPE_STATIC)
            fdb.type = "static";
        else if (fdb_info->type == MCLAG_FDB_TYPE_DYNAMIC)
            fdb.type = "dynamic";
        else if (fdb_info->type == MCLAG_FDB_TYPE_DYNAMIC_LOCAL)
            fdb.type = "dynamic_local";

        snprintf(key, 64, "%s%d:%s", "Vlan", fdb_info->vid, fdb.mac.c_str());
        fdb_key = key;

        SWSS_LOG_DEBUG("Received MAC key: %s, op_type: %d, mac type: %s , port: %s",
                fdb_key.c_str(), fdb_info->op_type, fdb.type.c_str(), fdb.port_name.c_str());

        if (fdb_info->op_type == MCLAG_FDB_OPER_ADD)
        {
            vector<FieldValueTuple> attrs;

            /*set port attr*/
            FieldValueTuple port_attr("port", fdb.port_name);
            attrs.push_back(port_attr);

            /*set type attr*/
            FieldValueTuple type_attr("type", fdb.type);
            attrs.push_back(type_attr);
            p_fdb_tbl->set(fdb_key, attrs);
            SWSS_LOG_NOTICE("add fdb entry into ASIC_DB:key =%s, type =%s", fdb_key.c_str(),  fdb.type.c_str());
        }
        else if (fdb_info->op_type == MCLAG_FDB_OPER_DEL)
        {
            p_fdb_tbl->del(fdb_key);
            SWSS_LOG_NOTICE("del fdb entry from ASIC_DB:key =%s", fdb_key.c_str());
        }
    }
    return;
}

void MclagLink::mclagsyncdSendFdbEntries(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    size_t infor_len = sizeof(mclag_msg_hdr_t);
    struct mclag_fdb_info info;
    mclag_msg_hdr_t * msg_head = NULL;
    int count = 0;
    ssize_t write = 0;

    char *infor_start = m_messageBuffer_send;

    /* Nothing popped */
    if (entries.empty())
    {
        return;
    }

    for (auto entry: entries)
    {
        memset(&info, 0, sizeof(struct mclag_fdb_info));
        count++;
        std::string key = kfvKey(entry);
        std::string op = kfvOp(entry);

        std::size_t delimiter = key.find_first_of(":");
        auto vlan_name = key.substr(0, delimiter);
        const auto mac_address_str = key.substr(delimiter+1);
        uint8_t mac_address[ETHER_ADDR_LEN];

        MacAddress::parseMacString(mac_address_str, mac_address);

        info.vid = (unsigned int) stoi(vlan_name.substr(4));
        memcpy(info.mac, mac_address , ETHER_ADDR_LEN);

        if (op == "SET")
            info.op_type = MCLAG_FDB_OPER_ADD;
        else
            info.op_type = MCLAG_FDB_OPER_DEL;

        for (auto i : kfvFieldsValues(entry))
        {
            if (fvField(i) == "port")
            {
                memcpy(info.port_name, fvValue(i).c_str(), fvValue(i).length());
            }
            if (fvField(i) == "type")
            {
                if (fvValue(i) == "dynamic")
                    info.type = MCLAG_FDB_TYPE_DYNAMIC;
                else if (fvValue(i) == "static")
                    info.type = MCLAG_FDB_TYPE_STATIC;
                else
                    SWSS_LOG_ERROR("MCLAGSYNCD STATE FDB updates key=%s, invalid MAC type %s\n", key.c_str(), fvValue(i).c_str());
            }
        }
        SWSS_LOG_NOTICE("MCLAGSYNCD STATE FDB updates key=%s, operation=%s, type: %d, port: %s \n",
                key.c_str(), op.c_str(), info.type, info.port_name);

        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < sizeof(struct mclag_fdb_info))
        {
            msg_head = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
            msg_head->version = 1;
            msg_head->msg_len = (unsigned short)infor_len;
            msg_head ->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

            SWSS_LOG_DEBUG("mclagsycnd buffer full send msg to iccpd, msg_len =%d, msg_type =%d count : %d",
                    msg_head->msg_len, msg_head->msg_type, count);
            write = ::write(m_connection_socket, infor_start, msg_head->msg_len);

            if (write <= 0)
            {
                SWSS_LOG_ERROR("mclagsycnd update FDB to ICCPD Buffer full, write to m_connection_socket failed");
            }

            infor_len = sizeof(mclag_msg_hdr_t);
            count = 0;
        }
        memcpy((char*)(infor_start + infor_len), (char*)&info, sizeof(struct mclag_fdb_info));
        infor_len = infor_len +  sizeof(struct mclag_fdb_info);
    }


    if (infor_len <= sizeof(mclag_msg_hdr_t)) /*no fdb entry need notifying iccpd*/
        return;

    msg_head = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));

    msg_head->version = 1;
    msg_head->msg_len = (unsigned short)infor_len;
    msg_head ->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

    SWSS_LOG_DEBUG("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d count : %d",
            msg_head->msg_len, msg_head->msg_type, count);
    write = ::write(m_connection_socket, infor_start, msg_head->msg_len);

    if (write <= 0)
    {
        SWSS_LOG_ERROR("mclagsycnd update FDB to ICCPD, write to m_connection_socket failed");
    }

    return;
}


void MclagLink::processMclagDomainCfg(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    char *infor_start = getSendMsgBuffer();
    size_t infor_len = sizeof(mclag_msg_hdr_t);
    uint8_t system_mac[ETHER_ADDR_LEN];
    int add_cfg_dependent_selectables = 0;

    int count = 0;
    ssize_t write = 0;

    struct mclag_domain_cfg_info cfg_info;
    mclag_msg_hdr_t *cfg_msg_hdr = NULL;

    /* Nothing popped */
    if (entries.empty())
    {
        return;
    }

    MacAddress::parseMacString(m_system_mac, system_mac);

    for (auto entry: entries)
    {

        std::string domain_id_str = kfvKey(entry);
        std::string op = kfvOp(entry);
        int entryExists = 0;
        int attrBmap = MCLAG_CFG_ATTR_NONE;
        int attrDelBmap = MCLAG_CFG_ATTR_NONE;
        enum MCLAG_DOMAIN_CFG_OP_TYPE cfgOpType = MCLAG_CFG_OPER_NONE;

        memset(&cfg_info, 0, sizeof(mclag_domain_cfg_info));
        cfg_info.domain_id  = stoi(domain_id_str);
        memcpy(cfg_info.system_mac, system_mac, ETHER_ADDR_LEN);


        SWSS_LOG_INFO("Key(mclag domain_id):%s;  op:%s ", domain_id_str.c_str(), op.c_str()); 

        const struct mclagDomainEntry domain(stoi(domain_id_str));
        auto it = m_mclag_domains.find(domain);
        if (it != m_mclag_domains.end())
        {
            entryExists = 1;
        }

        if (op == "SET")
        {
            struct mclagDomainData  domainData;

            for (auto i : kfvFieldsValues(entry))
            {
                SWSS_LOG_DEBUG(" MCLAGSYNCD CFG Table Updates : "   "Field %s, Value: %s  EntryExits:%d \n", 
                        fvField(i).c_str(), fvValue(i).c_str(), entryExists);

                if (fvField(i) == "source_ip")
                {
                    domainData.source_ip = fvValue(i); 

                    if(!entryExists)
                    {
                        attrBmap = (attrBmap | MCLAG_CFG_ATTR_SRC_ADDR);
                        memcpy(cfg_info.local_ip, domainData.source_ip.c_str(), INET_ADDRSTRLEN);
                    }
                }
                if (fvField(i) == "peer_ip")
                {
                    domainData.peer_ip = fvValue(i); 
                    if(!entryExists)
                    {
                        attrBmap = (attrBmap | MCLAG_CFG_ATTR_PEER_ADDR);
                        memcpy(cfg_info.peer_ip, domainData.peer_ip.c_str(), INET_ADDRSTRLEN);
                    }
                }
                if (fvField(i) == "peer_link")
                {
                    domainData.peer_link = fvValue(i); 
                    if(!entryExists)
                    {
                        attrBmap = (attrBmap | MCLAG_CFG_ATTR_PEER_LINK);
                        memcpy(cfg_info.peer_ifname, domainData.peer_link.c_str(), MAX_L_PORT_NAME);
                    }
                }
                if (fvField(i) == "keepalive_interval")
                {
                    if (fvValue(i).empty())
                    {
                        domainData.keepalive_interval = -1;
                    }
                    else
                    {
                        domainData.keepalive_interval = stoi(fvValue(i).c_str()); 
                    }
                    if(!entryExists)
                    {
                        attrBmap = (attrBmap | MCLAG_CFG_ATTR_KEEPALIVE_INTERVAL);
                        cfg_info.keepalive_time = domainData.keepalive_interval;
                    }
                }
                if (fvField(i) == "session_timeout")
                {
                    if (fvValue(i).empty())
                    {
                        domainData.session_timeout = -1;
                    }
                    else
                    {
                        domainData.session_timeout = stoi(fvValue(i).c_str()); 
                    }
                    if(!entryExists)
                    {
                        attrBmap = (attrBmap | MCLAG_CFG_ATTR_SESSION_TIMEOUT);
                        cfg_info.session_timeout = domainData.session_timeout;
                    }
                }
            }


            //If entry present send only the diff
            if (entryExists)
            {
                if( (it->second.source_ip.compare(domainData.source_ip)) != 0)
                {
                    attrBmap = (attrBmap | MCLAG_CFG_ATTR_SRC_ADDR);
                    memcpy(cfg_info.local_ip, domainData.source_ip.c_str(), INET_ADDRSTRLEN);
                    if (domainData.source_ip.empty())
                    {
                        attrDelBmap = attrDelBmap | MCLAG_CFG_ATTR_SRC_ADDR;
                    }
                }
                if( (it->second.peer_ip.compare(domainData.peer_ip)) != 0)
                {
                    attrBmap |= MCLAG_CFG_ATTR_PEER_ADDR;
                    memcpy(cfg_info.peer_ip, domainData.peer_ip.c_str(), INET_ADDRSTRLEN);

                    if (domainData.peer_ip.empty())
                    {
                        attrDelBmap = attrDelBmap | MCLAG_CFG_ATTR_PEER_ADDR;
                    }
                }
                if( (it->second.peer_link.compare(domainData.peer_link)) != 0)
                {
                    attrBmap |= MCLAG_CFG_ATTR_PEER_LINK;
                    memcpy(cfg_info.peer_ifname, domainData.peer_link.c_str(), MAX_L_PORT_NAME);
                    if (domainData.peer_link.empty())
                    {
                        attrDelBmap = attrDelBmap | MCLAG_CFG_ATTR_PEER_LINK;
                    }
                }

                if(it->second.keepalive_interval != domainData.keepalive_interval)
                {
                    attrBmap |= MCLAG_CFG_ATTR_KEEPALIVE_INTERVAL;
                    cfg_info.keepalive_time = domainData.keepalive_interval;
                    if (domainData.keepalive_interval == -1)
                    {
                        attrDelBmap = attrDelBmap | MCLAG_CFG_ATTR_KEEPALIVE_INTERVAL;
                    }
                }

                if(it->second.session_timeout != domainData.session_timeout)
                {
                    attrBmap |= MCLAG_CFG_ATTR_SESSION_TIMEOUT;
                    cfg_info.session_timeout = domainData.session_timeout;
                    if (domainData.session_timeout == -1)
                    {
                        attrDelBmap = attrDelBmap | MCLAG_CFG_ATTR_SESSION_TIMEOUT;
                    }
                }
            }

            //nothing changed no need to update
            if (!attrBmap && !attrDelBmap)
            {
                //no need to update
                SWSS_LOG_NOTICE("mclagsycnd: domain cfg processing ; no change - duplicate update");
                return;
            }

            SWSS_LOG_NOTICE("mclagsycnd: domain cfg processing; mandatory args present; Domain [%d] send to iccpd", domain.domain_id);

            //Add/update domain map
            m_mclag_domains[domain] = domainData;

            //send config msg to iccpd
            SWSS_LOG_DEBUG(" MCLAGSYNCD CFG Table Updates : domain_id:%d op_type:%d attrBmap:0x%x attrDelBmap:0x%x cfg_info.local_ip %s, peer_ip: %s peer_link:%s system_mac:%s session_timeout:%d keepalive_time:%d ",
                    domain.domain_id, cfgOpType, attrBmap, attrDelBmap, cfg_info.local_ip, cfg_info.peer_ip, cfg_info.peer_ifname, m_system_mac.c_str(), cfg_info.session_timeout, cfg_info.keepalive_time);

            //Entry not found previously and got created now - do add operation
            if (!entryExists)
            {
                cfgOpType = MCLAG_CFG_OPER_ADD;

                add_cfg_dependent_selectables = 1;
            }
            else //entry found
            {
                //entry found and one attribute is deleted
                if ( attrDelBmap && (attrBmap == attrDelBmap) )
                {
                    cfgOpType = MCLAG_CFG_OPER_ATTR_DEL;
                }
                else //entry found and attribute are getting updated
                {
                    cfgOpType = MCLAG_CFG_OPER_UPDATE;
                }

            }
        }
        else
        {
            //Entry not found - error deletion
            if (!entryExists)
            {
                SWSS_LOG_WARN("mclagsycnd to ICCPD, cfg processing ; Domain [%d] deletion - domain not found", domain.domain_id);
                return;
            }
            else
            {
                cfgOpType = MCLAG_CFG_OPER_DEL;
                SWSS_LOG_NOTICE(" Del dependent selectables from select ");
                delDomainCfgDependentSelectables();
                add_cfg_dependent_selectables = 0;
                m_mclag_domains.erase(domain);
                SWSS_LOG_NOTICE("mclagsycnd to ICCPD, cfg processing ; Domain [%d] deletion", domain.domain_id);
            }
        }

        if (cfgOpType == MCLAG_CFG_OPER_NONE)
        {
            SWSS_LOG_NOTICE("mclagsycnd to ICCPD, cfg processing ; Domain [%d] op type not set", domain.domain_id);
            return;
        }


        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < (sizeof(struct mclag_domain_cfg_info)) )
        {
            cfg_msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
            cfg_msg_hdr->version = 1;
            cfg_msg_hdr->msg_len = (unsigned short)infor_len;
            cfg_msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_DOMAIN;

            SWSS_LOG_DEBUG("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d version=%d count : %d", cfg_msg_hdr->msg_len, cfg_msg_hdr->msg_type, cfg_msg_hdr->version, count);
            write = ::write(getConnSocket(), infor_start, cfg_msg_hdr->msg_len);

            if (write <= 0)
            {
                SWSS_LOG_ERROR("mclagsycnd to ICCPD, domain cfg send, buffer full; write to m_connection_socket failed");
            }

            infor_len = sizeof(mclag_msg_hdr_t);
        }

        cfg_info.op_type = cfgOpType;
        cfg_info.attr_bmap = attrBmap;
        memcpy((char*)(infor_start + infor_len), (char*)&cfg_info, sizeof(struct mclag_domain_cfg_info));
        infor_len = infor_len +  sizeof(struct mclag_domain_cfg_info);
        SWSS_LOG_DEBUG(" MCLAGSYNCD CFG Table Updates: domain_id:%d infor_len:%d infor_start:%p ", cfg_info.domain_id, (int)infor_len, infor_start);  
    }

    /*no config info notification reqd */
    if (infor_len <= sizeof(mclag_msg_hdr_t))
        return;

    cfg_msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
    cfg_msg_hdr->version = 1;
    cfg_msg_hdr->msg_len = (unsigned short)infor_len;
    cfg_msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_DOMAIN;

    SWSS_LOG_DEBUG("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d    count : %d ver = %d ", cfg_msg_hdr->msg_len, cfg_msg_hdr->msg_type, count, cfg_msg_hdr->version);
    write = ::write(getConnSocket(), infor_start, cfg_msg_hdr->msg_len);


    if (write <= 0)
    {
        SWSS_LOG_ERROR("mclagsycnd to ICCPD, domain cfg send; write to m_connection_socket failed");
    }

    if (add_cfg_dependent_selectables)
    {
        SWSS_LOG_NOTICE(" Add dependent selectables to select ");
        addDomainCfgDependentSelectables();
    }
}

void MclagLink::addDomainCfgDependentSelectables()
{
    p_state_fdb_tbl = new SubscriberStateTable(p_state_db.get(), STATE_FDB_TABLE_NAME);
    SWSS_LOG_INFO(" MCLAGSYNCD create state fdb table");

    p_state_vlan_mbr_subscriber_table = new SubscriberStateTable(p_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);
    SWSS_LOG_INFO(" MCLAGSYNCD create state vlan member table");

    p_mclag_intf_cfg_tbl      = new SubscriberStateTable(p_config_db.get(), CFG_MCLAG_INTF_TABLE_NAME);
    SWSS_LOG_INFO(" MCLAGSYNCD create cfg mclag intf table");

    p_mclag_unique_ip_cfg_tbl = new SubscriberStateTable(p_config_db.get(), CFG_MCLAG_UNIQUE_IP_TABLE_NAME);
    SWSS_LOG_INFO(" MCLAGSYNCD create cfg unique ip table");

    if (p_state_fdb_tbl) 
    {
        m_select->addSelectable(p_state_fdb_tbl);
        SWSS_LOG_INFO(" MCLAGSYNCD Add state_fdb_tbl to selectable");
    }


    if (p_state_vlan_mbr_subscriber_table) 
    {
        m_select->addSelectable(p_state_vlan_mbr_subscriber_table);
        SWSS_LOG_NOTICE(" MCLAGSYNCD Add p_state_vlan_mbr_subscriber_table  to selectable");
    }

    //add mclag interface table to selectable
    if (p_mclag_intf_cfg_tbl)
    {
        m_select->addSelectable(p_mclag_intf_cfg_tbl);
        SWSS_LOG_NOTICE("MCLagSYNCD Adding mclag_intf_cfg_tbl to selectable");
    }

    //add mclag unique ip table to selectable
    if (p_mclag_unique_ip_cfg_tbl)
    {
        m_select->addSelectable(getMclagUniqueCfgTable());
        SWSS_LOG_NOTICE("MCLagSYNCD Adding mclag_unique_ip_cfg_tbl to selectable");
    }
}

void MclagLink::delDomainCfgDependentSelectables()
{
    if (p_mclag_intf_cfg_tbl)
    {
        m_select->removeSelectable(getMclagIntfCfgTable());
        SWSS_LOG_NOTICE("MCLagSYNCD remove mclag_intf_cfg_tbl to selectable");
        delete p_mclag_intf_cfg_tbl;
        p_mclag_intf_cfg_tbl = NULL;
    }

    if (p_mclag_unique_ip_cfg_tbl)
    {
        m_select->removeSelectable(getMclagUniqueCfgTable());
        SWSS_LOG_NOTICE("MCLagSYNCD remove mclag_unique_ip_cfg_tbl to selectable");
        delete p_mclag_unique_ip_cfg_tbl;
        p_mclag_unique_ip_cfg_tbl = NULL;
    }

    if (p_state_fdb_tbl)
    {
        m_select->removeSelectable(p_state_fdb_tbl);
        SWSS_LOG_INFO(" MCLAGSYNCD remove state_fdb_tbl from selectable");
        delete p_state_fdb_tbl;
        p_state_fdb_tbl = NULL;
    }

    if (p_state_vlan_mbr_subscriber_table)
    {
        m_select->removeSelectable(p_state_vlan_mbr_subscriber_table);
        SWSS_LOG_INFO(" MCLAGSYNCD remove p_state_vlan_mbr_subscriber_table selectable");

        delete p_state_vlan_mbr_subscriber_table;
        p_state_vlan_mbr_subscriber_table = NULL;
    }
}


void MclagLink::mclagsyncdSendMclagIfaceCfg(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    struct mclag_iface_cfg_info cfg_info;
    mclag_msg_hdr_t *cfg_msg_hdr = NULL;
    size_t infor_len = sizeof(mclag_msg_hdr_t);
    int count = 0;
    vector<string> po_names;

    ssize_t write = 0;
    char *infor_start = getSendMsgBuffer();

    /* Nothing popped */
    if (entries.empty())
    {
        return;
    }

    for (auto entry: entries)
    {
        std::string key = kfvKey(entry);
        std::string op = kfvOp(entry);

        std::size_t delimiter_pos = key.find_first_of("|");
        auto domain_id_str = key.substr(0, delimiter_pos);
        std::string mclag_ifaces;

        memset(&cfg_info, 0, sizeof(mclag_iface_cfg_info));

        count++;
        SWSS_LOG_DEBUG("mclag iface cfg ; Key %s passed", key.c_str()); 

        cfg_info.domain_id = stoi(domain_id_str);

        mclag_ifaces = key.substr(delimiter_pos+1);
        if (mclag_ifaces.empty())
        {
            SWSS_LOG_ERROR("Invalid Key %s Format. No mclag iface specified", key.c_str()); 
            continue;
        }

        if(op == "SET")
        {
            cfg_info.op_type = MCLAG_CFG_OPER_ADD;
        }
        else
        {
            cfg_info.op_type = MCLAG_CFG_OPER_DEL;

            /* Delete local interface port isolation setting from STATE_DB */
            deleteLocalIfPortIsolate(mclag_ifaces);
        }

        memcpy(cfg_info.mclag_iface, mclag_ifaces.c_str(), mclag_ifaces.size());
        po_names.push_back(mclag_ifaces);

        SWSS_LOG_DEBUG("domain_id:%d optype:%d mclag_ifaces:%s", cfg_info.domain_id, cfg_info.op_type, cfg_info.mclag_iface); 

        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < (sizeof(struct mclag_iface_cfg_info)) )
        {
            cfg_msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
            cfg_msg_hdr->version = 1;
            cfg_msg_hdr->msg_len = (unsigned short)infor_len;
            cfg_msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_IFACE;

            SWSS_LOG_DEBUG("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d count : %d",
                    cfg_msg_hdr->msg_len, cfg_msg_hdr->msg_type, count);
            write = ::write(getConnSocket(), infor_start, cfg_msg_hdr->msg_len);

            if (write <= 0)
            {
                SWSS_LOG_ERROR("mclagsycnd to ICCPD, mclag iface cfg send, buffer full; write to m_connection_socket failed");
            }

            infor_len = sizeof(mclag_msg_hdr_t);
        }
        memcpy((char*)(infor_start + infor_len), (char*)&cfg_info, sizeof(struct mclag_iface_cfg_info)); 
        infor_len +=  sizeof(struct mclag_iface_cfg_info) ;
    }

    /*no config info notification reqd */
    if (infor_len <= sizeof(mclag_msg_hdr_t))
        return; 

    cfg_msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
    cfg_msg_hdr->version = 1;
    cfg_msg_hdr->msg_len = (unsigned short)infor_len;
    cfg_msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_IFACE;

    SWSS_LOG_DEBUG("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d count : %d ver:%d ", cfg_msg_hdr->msg_len, cfg_msg_hdr->msg_type, cfg_msg_hdr->version, count);
    write = ::write(getConnSocket(), infor_start, cfg_msg_hdr->msg_len);


    if (write <= 0)
    {
        SWSS_LOG_ERROR("mclagsycnd to ICCPD, mclag iface cfg send; write to m_connection_socket failed");
    }
    return;
}

void MclagLink::mclagsyncdSendMclagUniqueIpCfg(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    struct mclag_unique_ip_cfg_info cfg_info;
    mclag_msg_hdr_t *cfg_msg_hdr = NULL;
    size_t infor_len = sizeof(mclag_msg_hdr_t);
    int count = 0;

    ssize_t write = 0;
    char *infor_start = getSendMsgBuffer();

    /* Nothing popped */
    if (entries.empty())
    {
        return;
    }

    for (auto entry: entries)
    {
        std::string key = kfvKey(entry);
        std::string op = kfvOp(entry);

        std::size_t delimiter_pos = key.find_first_of("|");
        auto domain_id_str = key.substr(0, delimiter_pos);
        std::string unique_ip_ifnames;

        memset(&cfg_info, 0, sizeof(mclag_unique_ip_cfg_info));

        count++;
        SWSS_LOG_NOTICE("mclag unique ip interface Key %s passed", key.c_str());

        unique_ip_ifnames = key.substr(delimiter_pos+1);
        if (unique_ip_ifnames.empty())
        {
            SWSS_LOG_ERROR("Invalid Key %s Format. No unique ip ifname specified", key.c_str());
            continue;
        }

        if(op == "SET")
        {
            cfg_info.op_type = MCLAG_CFG_OPER_ADD;
        }
        else
        {
            cfg_info.op_type = MCLAG_CFG_OPER_DEL;
        }

        memcpy(cfg_info.mclag_unique_ip_ifname, unique_ip_ifnames.c_str(), unique_ip_ifnames.size());

        SWSS_LOG_NOTICE("optype:%d mclag_unique_ip_ifname:%s", cfg_info.op_type, cfg_info.mclag_unique_ip_ifname);

        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < (sizeof(struct mclag_unique_ip_cfg_info)) )
        {
            cfg_msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
            cfg_msg_hdr->version = 1;
            cfg_msg_hdr->msg_len = (unsigned short)infor_len;
            cfg_msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_UNIQUE_IP;

            SWSS_LOG_NOTICE("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d count : %d",
                    cfg_msg_hdr->msg_len, cfg_msg_hdr->msg_type, count);

            write = ::write(getConnSocket(), infor_start, cfg_msg_hdr->msg_len);

            if (write <= 0)
            {
                SWSS_LOG_ERROR("mclagsycnd to ICCPD, mclag unique ip cfg send, buffer full; write to m_connection_socket failed");
            }

            infor_len = sizeof(mclag_msg_hdr_t);
        }
        memcpy((char*)(infor_start + infor_len), (char*)&cfg_info, sizeof(struct mclag_unique_ip_cfg_info));
        infor_len +=  sizeof(struct mclag_unique_ip_cfg_info);
    }

    /*no config info notification reqd */
    if (infor_len <= sizeof(mclag_msg_hdr_t))
        return;

    cfg_msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
    cfg_msg_hdr->version = 1;
    cfg_msg_hdr->msg_len = (unsigned short)infor_len;
    cfg_msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_UNIQUE_IP;

    SWSS_LOG_NOTICE("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d count : %d ver:%d ",
            cfg_msg_hdr->msg_len, cfg_msg_hdr->msg_type, cfg_msg_hdr->version, count);

    write = ::write(getConnSocket(), infor_start, cfg_msg_hdr->msg_len);

    if (write <= 0)
    {
        SWSS_LOG_ERROR("mclagsycnd to ICCPD, mclag unique ip cfg send; write to m_connection_socket failed");
    }

    return;
}

void MclagLink::processVlanMemberTableUpdates(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    struct mclag_vlan_mbr_info vlan_mbr_info;
    mclag_msg_hdr_t *msg_hdr = NULL;
    size_t infor_len = sizeof(mclag_msg_hdr_t);
    int count = 0;

    ssize_t write = 0;
    char *infor_start = getSendMsgBuffer();

    /* Nothing popped */
    if (entries.empty())
    {
        return;
    }

    for (auto entry: entries)
    {
        std::string key = kfvKey(entry);
        std::string op = kfvOp(entry);
        int vlan_mbrship_found = 0;

        std::size_t delimiter_pos = key.find_first_of("|");
        std::string vlan_mbr_iface;
        unsigned int vlan_id;

        auto vlan_id_str = key.substr(0, delimiter_pos);
        vlan_id = (unsigned int) stoi(vlan_id_str.substr(4));
        vlan_mbr_iface  = key.substr(delimiter_pos+1);

        memset(&vlan_mbr_info, 0, sizeof(vlan_mbr_info));

        SWSS_LOG_DEBUG("%s: vlan_id:%d vlan_mbr:%s ", __FUNCTION__, vlan_id, vlan_mbr_iface.c_str()); 

        vlan_mbrship_found = findVlanMbr(vlan_id_str.c_str(), vlan_mbr_iface.c_str());
        if(op == "SET")
        {
            vlan_mbr_info.op_type = MCLAG_CFG_OPER_ADD;
            //found already no need to add and send again 
            if(vlan_mbrship_found)
            {
                continue;
            }
            addVlanMbr(vlan_id_str.c_str(), vlan_mbr_iface.c_str());
        }
        else
        {
            //if member not found - skip delete
            if(!vlan_mbrship_found)
            {
                SWSS_LOG_NOTICE("%s: duplicate vlan member delete; vlan_id:%d vlan_mbr:%s ", __FUNCTION__, vlan_id, vlan_mbr_iface.c_str()); 
                continue;
            }
            vlan_mbr_info.op_type = MCLAG_CFG_OPER_DEL;
            delVlanMbr(vlan_id_str.c_str(), vlan_mbr_iface.c_str());
        }

        count++;
        vlan_mbr_info.vid = vlan_id;
        memcpy(vlan_mbr_info.mclag_iface, vlan_mbr_iface.c_str(), sizeof(vlan_mbr_info.mclag_iface));


        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < (sizeof(struct mclag_vlan_mbr_info)) )
        {
            msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
            msg_hdr->version = 1;
            msg_hdr->msg_len = (unsigned short)infor_len;
            msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_VLAN_MBR_UPDATES;

            SWSS_LOG_NOTICE("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d count : %d", msg_hdr->msg_len, msg_hdr->msg_type, (count -1));
            write = ::write(getConnSocket(), infor_start, msg_hdr->msg_len);

            count = 0;
            if (write <= 0)
            {
                SWSS_LOG_ERROR("mclagsycnd to ICCPD, mclag vlan member updates send, buffer full; write to m_connection_socket failed");
            }

            infor_len = sizeof(mclag_msg_hdr_t);
        }
        memcpy((char*)(infor_start + infor_len), (char*)&vlan_mbr_info,    sizeof(struct mclag_vlan_mbr_info)); 
        infor_len += sizeof(struct mclag_vlan_mbr_info) ;
    }

    /*no config info notification reqd */
    if (infor_len <= sizeof(mclag_msg_hdr_t))
        return; 

    msg_hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(infor_start));
    msg_hdr->version  = 1;
    msg_hdr->msg_len  = (unsigned short)infor_len;
    msg_hdr->msg_type = MCLAG_SYNCD_MSG_TYPE_VLAN_MBR_UPDATES;

    SWSS_LOG_NOTICE("mclagsycnd send msg to iccpd,mclag vlan member updates; msg_len =%d, msg_type =%d count : %d ver:%d ", msg_hdr->msg_len, msg_hdr->msg_type, msg_hdr->version, count);
    write = ::write(getConnSocket(), infor_start, msg_hdr->msg_len);


    if (write <= 0)
    {
        SWSS_LOG_ERROR("mclagsycnd to ICCPD, mclag vlan member updates send; write to m_connection_socket failed");
    }
    return;
}


/* Enable/Disable traffic distribution mode for LAG member port */
void MclagLink::mclagsyncdSetTrafficDisable(
        char                      *msg,
        uint8_t                   msg_type)
{
    string                    lag_name;
    string                    traffic_dist_disable;
    mclag_sub_option_hdr_t    *op_hdr = NULL;
    vector<FieldValueTuple>   fvVector;

    /* Get port-channel name */
    op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(msg));
    if (op_hdr->op_type != MCLAG_SUB_OPTION_TYPE_MCLAG_INTF_NAME)
    {
        SWSS_LOG_ERROR("Invalid option type %u", op_hdr->op_type);
        return;
    }
    lag_name.insert(0, (const char*)op_hdr->data, op_hdr->op_len);

    if (msg_type == MCLAG_MSG_TYPE_SET_TRAFFIC_DIST_DISABLE)
        traffic_dist_disable = "true";
    else
        traffic_dist_disable = "false";

    fvVector.push_back(make_pair("traffic_disable", traffic_dist_disable));
    p_lag_tbl->set(lag_name, fvVector);
    SWSS_LOG_NOTICE("Set traffic %s for %s",
            (msg_type == MCLAG_MSG_TYPE_SET_TRAFFIC_DIST_DISABLE) ?
            "disable" : "enable", lag_name.c_str());
}

/* Set the oper_status field in the STATE_MCLAG_TABLE */
void MclagLink::mclagsyncdSetIccpState(
        char                      *msg,
        size_t                    msg_len)
{
    int                       mlag_id = 0;
    bool                      is_oper_up = false;
    char                      *cur;
    size_t                    cur_len = 0;
    mclag_sub_option_hdr_t    *op_hdr;
    vector<FieldValueTuple>   fvVector;

    while (cur_len < msg_len)
    {
        cur = msg + cur_len;
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        switch(op_hdr->op_type)
        {
            case MCLAG_SUB_OPTION_TYPE_MCLAG_ID:
                memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_OPER_STATUS:
                memcpy(&is_oper_up, op_hdr->data, op_hdr->op_len);
                fvVector.push_back(
                        make_pair("oper_status", is_oper_up ? "up" : "down"));
                break;

            default:
                SWSS_LOG_WARN("Invalid option type %u", op_hdr->op_type);
                break;
        }
        cur_len += (MCLAG_SUB_OPTION_HDR_LEN + op_hdr->op_len);
    }
    if ((mlag_id > 0) && (fvVector.size() > 0))
    {
        is_iccp_up = is_oper_up;
        /* Update MLAG table: key = mlag_id, value = oper_status */
        p_mclag_tbl->set(to_string(mlag_id), fvVector);
        SWSS_LOG_NOTICE("Set mlag %d ICCP state to %s",
                mlag_id, is_oper_up ? "up" : "down");
    }
    else
    {
        SWSS_LOG_ERROR("Invalid parameter, mlag %d", mlag_id);
    }
}

/* Set the role field in the STATE_MCLAG_TABLE */
void MclagLink::mclagsyncdSetIccpRole(
        char                      *msg,
        size_t                    msg_len)
{
    int                       mlag_id = 0;
    bool                      is_active_role;
    bool                      valid_system_id = false;
    string                    system_id_str;
    char                      *cur;
    size_t                    cur_len = 0;
    mclag_sub_option_hdr_t    *op_hdr;
    vector<FieldValueTuple>   fvVector;

    while (cur_len < msg_len)
    {
        cur = msg + cur_len;
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));

        switch(op_hdr->op_type)
        {
            case MCLAG_SUB_OPTION_TYPE_MCLAG_ID:
                memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_ICCP_ROLE:
                memcpy(&is_active_role, op_hdr->data, op_hdr->op_len);
                fvVector.push_back(
                        make_pair("role", is_active_role ? "active" : "standby"));
                break;

            case MCLAG_SUB_OPTION_TYPE_SYSTEM_ID:
                valid_system_id = true;
                system_id_str = MacAddress::to_string(op_hdr->data);
                fvVector.push_back(make_pair("system_mac", system_id_str));
                break;
            default:
                SWSS_LOG_ERROR("Invalid option type %u", op_hdr->op_type);
                break;
        }
        cur_len += (MCLAG_SUB_OPTION_HDR_LEN + op_hdr->op_len);
    }
    if ((mlag_id > 0) && (fvVector.size() > 0))
    {
        /* Update MLAG table: key = mlag_id, value = role */
        p_mclag_tbl->set(to_string(mlag_id), fvVector);
        SWSS_LOG_NOTICE("Set mlag %d ICCP role to %s, system_id(%s)",
                mlag_id, is_active_role ? "active" : "standby",
                valid_system_id ? system_id_str.c_str() : "None");
    }
    else
    {
        SWSS_LOG_ERROR("Invalid parameter, mlag %d", mlag_id);
    }
}

/* Set the system_mac field in the STATE_MCLAG_TABLE */
void MclagLink::mclagsyncdSetSystemId(
        char                      *msg,
        size_t                    msg_len)
{
    int                       mlag_id = 0;
    string                    system_id_str;
    char                      *cur;
    size_t                    cur_len = 0;
    mclag_sub_option_hdr_t    *op_hdr;
    vector<FieldValueTuple>   fvVector;

    while (cur_len < msg_len)
    {
        cur = msg + cur_len;
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));

        switch(op_hdr->op_type)
        {
            case MCLAG_SUB_OPTION_TYPE_MCLAG_ID:
                memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_SYSTEM_ID:
                system_id_str = MacAddress::to_string(op_hdr->data);
                fvVector.push_back(make_pair("system_mac", system_id_str));
                break;

            default:
                SWSS_LOG_ERROR("Invalid option type %u", op_hdr->op_type);
                break;
        }
        cur_len += (MCLAG_SUB_OPTION_HDR_LEN + op_hdr->op_len);
    }
    if ((mlag_id > 0) && (fvVector.size() > 0))
    {
        /* Update MLAG table: key = mlag_id, value = system_mac */
        p_mclag_tbl->set(to_string(mlag_id), fvVector);
        SWSS_LOG_NOTICE("Set mlag %d system mac to %s",
                mlag_id, system_id_str.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("Invalid parameter, mlag %d", mlag_id);
    }
}

void MclagLink::processStateFdb(SubscriberStateTable *stateFdbTbl)
{
    SWSS_LOG_INFO("MCLAGSYNCD: Process State Fdb events ");
    std::deque<KeyOpFieldsValuesTuple> entries;
    stateFdbTbl->pops(entries);
    mclagsyncdSendFdbEntries(entries);
}

void MclagLink::processStateVlanMember(SubscriberStateTable *stateVlanMemberTbl)
{
    SWSS_LOG_INFO("MCLAGSYNCD: Process State Vlan Member events ");
    std::deque<KeyOpFieldsValuesTuple> entries;
    stateVlanMemberTbl->pops(entries);
    processVlanMemberTableUpdates(entries);
}

/* Delete Mlag entry in the STATE_MCLAG_TABLE */
void MclagLink::mclagsyncdDelIccpInfo(
        char                      *msg)
{
    int                       mlag_id;
    mclag_sub_option_hdr_t    *op_hdr = NULL;
    vector<FieldValueTuple>   fvVector;

    /* Get MLAG ID */
    op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(msg));
    if (op_hdr->op_type != MCLAG_SUB_OPTION_TYPE_MCLAG_ID)
    {
        SWSS_LOG_ERROR("Invalid option type %u", op_hdr->op_type);
    }
    else
    {
        memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
        p_mclag_tbl->del(to_string(mlag_id));
        SWSS_LOG_NOTICE("Delete mlag %d", mlag_id);
    }
}

/* Set local interface portisolate field enable/disable in the
 * STATE_MCLAG_LOCAL_INTF_TABLE.
 * Key = "interface"
 */
void MclagLink::setLocalIfPortIsolate(std::string mclag_if, bool is_enable)
{
    vector<FieldValueTuple>   fvVector;
    std::string                    key;

    /* Update MLAG Local Interface table: key = interface, value = * enable/disable */
    key =  mclag_if;
    fvVector.push_back(make_pair("port_isolate_peer_link", is_enable ? "true" : "false"));
    p_mclag_local_intf_tbl->set(key, fvVector);
    SWSS_LOG_NOTICE("Set local interface %s to %s", mclag_if.c_str(), is_enable ? "true" : "false");
}

/* Delete local interface
 * STATE_MCLAG_LOCAL_INTF_TABLE.
 * Key = "interface"
 */
void MclagLink::deleteLocalIfPortIsolate(std::string mclag_if)
{
    vector<FieldValueTuple>   fvVector;
    std::string                    key;

    p_mclag_local_intf_tbl->del(mclag_if);
    SWSS_LOG_NOTICE("Delete local interface %s", mclag_if.c_str());
}

/* Set remote interface state field oper_status in the
 * STATE_MCLAG_REMOTE_INTF_TABLE.
 * Key = "Mclag<id>|interface"
 */
void MclagLink::mclagsyncdSetRemoteIfState(
        char                      *msg,
        size_t                    msg_len)
{
    int                       mlag_id = 0;
    bool                      is_oper_up;
    string                    lag_name;
    char                      *cur;
    size_t                    cur_len = 0;
    mclag_sub_option_hdr_t    *op_hdr;
    string                    key;
    vector<FieldValueTuple>   fvVector;

    while (cur_len < msg_len)
    {
        cur = msg + cur_len;
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        switch(op_hdr->op_type)
        {
            case MCLAG_SUB_OPTION_TYPE_MCLAG_ID:
                memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_MCLAG_INTF_NAME:
                lag_name.insert(0, (const char*)op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_OPER_STATUS:
                memcpy(&is_oper_up, op_hdr->data, op_hdr->op_len);
                fvVector.push_back(
                        make_pair("oper_status", is_oper_up ? "up" : "down"));
                break;

            default:
                SWSS_LOG_WARN("Invalid option type %u", op_hdr->op_type);
                break;
        }
        cur_len += (MCLAG_SUB_OPTION_HDR_LEN + op_hdr->op_len);
    }
    if ((mlag_id > 0) && (!lag_name.empty()) && (fvVector.size() > 0))
    {
        /* Update MLAG table: key = mclag<id>|interface, value = oper_status */
        key =  to_string(mlag_id) + "|" + lag_name;
        p_mclag_remote_intf_tbl->set(key, fvVector);
        SWSS_LOG_NOTICE("Set mlag %d, remote interface %s to %s",
                mlag_id, lag_name.c_str(), is_oper_up ? "up" : "down");
    }
    else
    {
        SWSS_LOG_ERROR("Invalid parameter, mlag %d, remote interface %s",
                mlag_id, lag_name.empty() ? "None" : lag_name.c_str());
    }
}

/* Delete remote interface state entry in the STATE_MCLAG_REMOTE_INTF_TABLE
 * Key = "Mclag<id>|interface"
 */
void MclagLink::mclagsyncdDelRemoteIfInfo(
        char                      *msg,
        size_t                    msg_len)
{
    int                       mlag_id = 0;
    string                    lag_name;
    char                      *cur;
    size_t                    cur_len = 0;
    mclag_sub_option_hdr_t    *op_hdr;
    string                    key;
    vector<FieldValueTuple>   fvVector;

    while (cur_len < msg_len)
    {
        cur = msg + cur_len;
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        switch(op_hdr->op_type)
        {
            case MCLAG_SUB_OPTION_TYPE_MCLAG_ID:
                memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_MCLAG_INTF_NAME:
                lag_name.insert(0, (const char*)op_hdr->data, op_hdr->op_len);
                break;

            default:
                SWSS_LOG_WARN("Invalid option type %u", op_hdr->op_type);
                break;
        }
        cur_len += (MCLAG_SUB_OPTION_HDR_LEN + op_hdr->op_len);
    }
    if ((mlag_id > 0) && (!lag_name.empty()))
    {
        key = to_string(mlag_id) + "|" + lag_name;
        p_mclag_remote_intf_tbl->del(key);
        SWSS_LOG_NOTICE("Delete mlag %d, remote interface %s",
                mlag_id, lag_name.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("Invalid parameter, mlag %d", mlag_id);
    }
}

/* Set peer-link isolation for the specified Mlag interface
 * Notes: Mlag-ID is not used currently for the local interface table
 */
void MclagLink::mclagsyncdSetPeerLinkIsolation(
        char                      *msg,
        size_t                    msg_len)
{
    int                       mlag_id = 0;
    bool                      is_isolation_enable;
    bool                      rx_isolation_setting = false;
    string                    mclag_if_name;
    char                      *cur;
    size_t                    cur_len = 0;
    mclag_sub_option_hdr_t    *op_hdr;

    while (cur_len < msg_len)
    {
        cur = msg + cur_len;
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));
        switch(op_hdr->op_type)
        {
            case MCLAG_SUB_OPTION_TYPE_MCLAG_ID:
                memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_MCLAG_INTF_NAME:
                mclag_if_name.insert(0, (const char*)op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_ISOLATION_STATE:
                memcpy(&is_isolation_enable, op_hdr->data, op_hdr->op_len);
                rx_isolation_setting = true;
                break;

            default:
                SWSS_LOG_WARN("Invalid option type %u", op_hdr->op_type);
                break;
        }
        cur_len += (MCLAG_SUB_OPTION_HDR_LEN + op_hdr->op_len);
    }
    if ((!mclag_if_name.empty()) && rx_isolation_setting)
    {
        setLocalIfPortIsolate(mclag_if_name, is_isolation_enable);
        SWSS_LOG_NOTICE("%s %s isolation from peer-link",
                is_isolation_enable ? "Enable" : "Disable", mclag_if_name.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("Missing parameter, mclag interface %s, ",
                mclag_if_name.empty() ? "None" : mclag_if_name.c_str());
    }
}

/* Set the remote system mac field in the STATE_MCLAG_TABLE */
void MclagLink::mclagsyncdSetPeerSystemId(
        char                      *msg,
        size_t                    msg_len)
{
    int                       mlag_id = 0;
    string                    system_id_str;
    char                      *cur;
    size_t                    cur_len = 0;
    mclag_sub_option_hdr_t    *op_hdr;
    vector<FieldValueTuple>   fvVector;

    while (cur_len < msg_len)
    {
        cur = msg + cur_len;
        op_hdr = reinterpret_cast<mclag_sub_option_hdr_t *>(static_cast<void *>(cur));

        switch(op_hdr->op_type)
        {
            case MCLAG_SUB_OPTION_TYPE_MCLAG_ID:
                memcpy(&mlag_id, op_hdr->data, op_hdr->op_len);
                break;

            case MCLAG_SUB_OPTION_TYPE_PEER_SYSTEM_ID:
                system_id_str = MacAddress::to_string(op_hdr->data);
                fvVector.push_back(make_pair("peer_mac", system_id_str));
                break;

            default:
                SWSS_LOG_ERROR("Invalid option type %u", op_hdr->op_type);
                break;
        }
        cur_len += (MCLAG_SUB_OPTION_HDR_LEN + op_hdr->op_len);
    }
    if ((mlag_id > 0) && (fvVector.size() > 0))
    {
        /* Update MLAG table: key = mlag_id, value = system_mac */
        p_mclag_tbl->set(to_string(mlag_id), fvVector);
        SWSS_LOG_NOTICE("Set mlag %d peer system mac to %s", mlag_id, system_id_str.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("Invalid parameter, mlag %d", mlag_id);
    }
}

MclagLink::MclagLink(Select *select, int port) :
    MSG_BATCH_SIZE(256),
    m_bufSize(MCLAG_MAX_MSG_LEN * MSG_BATCH_SIZE),
    m_messageBuffer(NULL),
    m_pos(0),
    m_connected(false),
    m_server_up(false),
    m_select(select)
{
    struct sockaddr_in addr;
    int true_val = 1;

    m_server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_server_socket < 0)
        throw system_error(errno, system_category());

    if (setsockopt(m_server_socket, SOL_SOCKET, SO_REUSEADDR, &true_val,
                sizeof(true_val)) < 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    if (setsockopt(m_server_socket, SOL_SOCKET, SO_KEEPALIVE, &true_val,
                sizeof(true_val)) < 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short int)port);
    addr.sin_addr.s_addr = htonl(MCLAG_DEFAULT_IP);

    if (bind(m_server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    if (listen(m_server_socket, 2) != 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    m_server_up = true;
    m_messageBuffer = new char[m_bufSize];
    m_messageBuffer_send = new char[MCLAG_MAX_SEND_MSG_LEN];

    p_learn = NULL;

    p_state_db    = unique_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    p_appl_db     = unique_ptr<DBConnector>(new DBConnector("APPL_DB", 0));
    p_config_db   = unique_ptr<DBConnector>(new DBConnector("CONFIG_DB", 0));
    p_asic_db     = unique_ptr<DBConnector>(new DBConnector("ASIC_DB", 0));
    p_counters_db = unique_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));
    p_notificationsDb = unique_ptr<DBConnector>(new DBConnector("STATE_DB", 0));

    p_device_metadata_tbl          = unique_ptr<Table>(new Table(p_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME));
    p_mclag_cfg_table              = unique_ptr<Table>(new Table(p_config_db.get(), CFG_MCLAG_TABLE_NAME)); 
    p_mclag_intf_cfg_table         = unique_ptr<Table>(new Table(p_config_db.get(), CFG_MCLAG_INTF_TABLE_NAME));

    p_mclag_tbl                    = unique_ptr<Table>(new Table(p_state_db.get(), STATE_MCLAG_TABLE_NAME));
    p_mclag_local_intf_tbl         = unique_ptr<Table>(new Table(p_state_db.get(), STATE_MCLAG_LOCAL_INTF_TABLE_NAME));
    p_mclag_remote_intf_tbl        = unique_ptr<Table>(new Table(p_state_db.get(), STATE_MCLAG_REMOTE_INTF_TABLE_NAME));


    p_intf_tbl      = unique_ptr<ProducerStateTable>(new ProducerStateTable(p_appl_db.get(), APP_INTF_TABLE_NAME));
    p_iso_grp_tbl   = unique_ptr<ProducerStateTable>(new ProducerStateTable(p_appl_db.get(), APP_ISOLATION_GROUP_TABLE_NAME));
    p_fdb_tbl       = unique_ptr<ProducerStateTable>(new ProducerStateTable(p_appl_db.get(), APP_MCLAG_FDB_TABLE_NAME));
    p_acl_table_tbl = unique_ptr<ProducerStateTable>(new ProducerStateTable(p_appl_db.get(), APP_ACL_TABLE_TABLE_NAME));
    p_acl_rule_tbl  = unique_ptr<ProducerStateTable>(new ProducerStateTable(p_appl_db.get(), APP_ACL_RULE_TABLE_NAME));
    p_lag_tbl       = unique_ptr<ProducerStateTable>(new ProducerStateTable(p_appl_db.get(), APP_LAG_TABLE_NAME));
    p_port_tbl      = unique_ptr<ProducerStateTable>(new ProducerStateTable(p_appl_db.get(), APP_PORT_TABLE_NAME));

    p_state_fdb_tbl                   = NULL;
    p_state_vlan_mbr_subscriber_table = NULL;
    p_mclag_intf_cfg_tbl              = NULL;
    p_mclag_unique_ip_cfg_tbl         = NULL;
}

MclagLink::~MclagLink()
{
    delete[] m_messageBuffer;
    delete[] m_messageBuffer_send;
    if (m_connected)
        close(m_connection_socket);
    if (m_server_up)
        close(m_server_socket);

    if (p_state_fdb_tbl) 
        delete p_state_fdb_tbl;

    if (p_state_vlan_mbr_subscriber_table) 
        delete p_state_vlan_mbr_subscriber_table;

    if (p_mclag_unique_ip_cfg_tbl) 
        delete p_mclag_unique_ip_cfg_tbl;

    if (p_mclag_intf_cfg_tbl) 
        delete p_mclag_intf_cfg_tbl;
}

void MclagLink::accept()
{
    struct sockaddr_in client_addr;
    socklen_t client_len;

    m_connection_socket = ::accept(m_server_socket, (struct sockaddr *)&client_addr,
            &client_len);
    if (m_connection_socket < 0)
        throw system_error(errno, system_category());

    SWSS_LOG_NOTICE("New connection accepted from: %s", inet_ntoa(client_addr.sin_addr));
}

int MclagLink::getFd()
{
    return m_connection_socket;
}

char* MclagLink::getSendMsgBuffer()
{
    return m_messageBuffer_send;
}

int MclagLink::getConnSocket()
{
    return m_connection_socket;
}


uint64_t MclagLink::readData()
{
    mclag_msg_hdr_t *hdr = NULL;
    size_t msg_len = 0;
    size_t start = 0, left = 0;
    ssize_t read = 0;
    char * msg = NULL;

    read = ::read(m_connection_socket, m_messageBuffer + m_pos, m_bufSize - m_pos);
    if (read == 0)
        throw MclagConnectionClosedException();
    if (read < 0)
        throw system_error(errno, system_category());
    m_pos += (uint32_t)read;

    while (true)
    {
        hdr = reinterpret_cast<mclag_msg_hdr_t *>(static_cast<void *>(m_messageBuffer + start));
        left = m_pos - start;
        if (left < MCLAG_MSG_HDR_LEN)
            break;

        msg_len = mclag_msg_len(hdr);
        if (left < msg_len)
            break;

        if (!mclag_msg_ok(hdr, left))
            throw system_error(make_error_code(errc::bad_message), "Malformed MCLAG message received");

        msg = ((char*)hdr) + MCLAG_MSG_HDR_LEN;

        switch (hdr->msg_type)
        {
            case MCLAG_MSG_TYPE_PORT_ISOLATE:
                setPortIsolate(msg);
                break;

            case MCLAG_MSG_TYPE_PORT_MAC_LEARN_MODE:
                setPortMacLearnMode(msg);
                break;

            case MCLAG_MSG_TYPE_FLUSH_FDB:
                setFdbFlush();
                break;

            case MCLAG_MSG_TYPE_SET_INTF_MAC:
                setIntfMac(msg);
                break;

            case MCLAG_MSG_TYPE_SET_FDB:
                setFdbEntry(msg, (int)(hdr->msg_len - sizeof(mclag_msg_hdr_t)));
                break;
            case MCLAG_MSG_TYPE_SET_TRAFFIC_DIST_ENABLE:
            case MCLAG_MSG_TYPE_SET_TRAFFIC_DIST_DISABLE:
                mclagsyncdSetTrafficDisable(msg, hdr->msg_type);
                break;
            case MCLAG_MSG_TYPE_SET_ICCP_STATE:
                mclagsyncdSetIccpState(msg, mclag_msg_data_len(hdr));
                break;
            case MCLAG_MSG_TYPE_SET_ICCP_ROLE:
                mclagsyncdSetIccpRole(msg, mclag_msg_data_len(hdr));
                break;
            case MCLAG_MSG_TYPE_SET_ICCP_SYSTEM_ID:
                mclagsyncdSetSystemId(msg, mclag_msg_data_len(hdr));
                break;
            case MCLAG_MSG_TYPE_DEL_ICCP_INFO:
                mclagsyncdDelIccpInfo(msg);
                break;
            case MCLAG_MSG_TYPE_SET_REMOTE_IF_STATE:
                mclagsyncdSetRemoteIfState(msg, mclag_msg_data_len(hdr));
                break;
            case MCLAG_MSG_TYPE_DEL_REMOTE_IF_INFO:
                mclagsyncdDelRemoteIfInfo(msg, mclag_msg_data_len(hdr));
                break;
            case MCLAG_MSG_TYPE_SET_PEER_LINK_ISOLATION:
                mclagsyncdSetPeerLinkIsolation(msg, mclag_msg_data_len(hdr));
                break;
            case MCLAG_MSG_TYPE_SET_ICCP_PEER_SYSTEM_ID:
                mclagsyncdSetPeerSystemId(msg, mclag_msg_data_len(hdr));
                break;
            default:
                break;
        }

        start += msg_len;
    }
    memmove(m_messageBuffer, m_messageBuffer + start, m_pos - start);
    m_pos = m_pos - (uint32_t)start;
    return 0;
}
