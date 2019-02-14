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
#include "netmsg.h"
#include "netdispatcher.h"
#include "swss/notificationproducer.h"
#include "mclagsyncd/mclaglink.h"
#include "mclagsyncd/mclag.h"
#include <set>
#include <algorithm>

using namespace swss;
using namespace std;

vector<string> string_split(const string& s, const string& delim)
{
    vector<string> elems;
    size_t pos = 0;
    size_t len = s.length();
    size_t delim_len = delim.length();
    
    if (delim_len == 0) 
        return elems;
    
    while (pos < len)
    {
        size_t find_pos = s.find(delim, pos);
        
        if (find_pos == string::npos)
        {
            elems.push_back(s.substr(pos, len - pos));
            break;
        }
        elems.push_back(s.substr(pos, find_pos - pos));
        pos = find_pos + delim_len;
    }
    
    return elems;
}

void mclagsyncd_get_oid_2_port_name_map(RedisClient *p_redisClient_2_counters, 
        std::unordered_map<std::string,std:: string> & port_map)
{
    std::unordered_map<std::string,std:: string>::iterator it;
    auto hash = p_redisClient_2_counters->hgetall("COUNTERS_PORT_NAME_MAP");

    for (it = hash.begin(); it != hash.end(); ++it)
        port_map.insert(pair<string, string>(it->second, it->first));
                
    return;
}

void mclagsyncd_get_bridgePortId_2_attrPortId_map(RedisClient *p_redisClient_2_asic, 
        std::map<std::string, std:: string> *oid_map)
{
    std::string bridge_port_id;
    size_t pos1 = 0;
   
    std::unordered_map<string, string>::iterator attr_port_id;

    auto keys = p_redisClient_2_asic->keys("ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT:*");
    for (auto& key : keys)
    {
        pos1 = key.find("oid:", 0);
        bridge_port_id = key.substr(pos1);
        
        auto hash = p_redisClient_2_asic->hgetall(key);
        attr_port_id = hash.find("SAI_BRIDGE_PORT_ATTR_PORT_ID");
        if (attr_port_id == hash.end())
            continue;
        
        oid_map->insert(pair<string, string>(bridge_port_id, attr_port_id->second));     
    }

    return;
}

void mclagsyncd_get_vid_by_bvid(RedisClient *p_redisClient_2_asic, 
        std::string &bvid, std::string &vlanid)
{
    
    std::unordered_map<std::string, std::string>::iterator attr_vlan_id;
    std::string pre = "ASIC_STATE:SAI_OBJECT_TYPE_VLAN:";
    std::string key = pre + bvid;
    
    auto hash = p_redisClient_2_asic->hgetall(key.c_str());

    attr_vlan_id = hash.find("SAI_VLAN_ATTR_VLAN_ID");
    if (attr_vlan_id == hash.end())
        return;
    
    vlanid = attr_vlan_id->second;
    return;
}

void mclagsyncd_get_fdb_set(RedisClient *p_redisClient_2_asic, 
        RedisClient *p_redisClient_2_counters, std::set<mclag_fdb> *fdb_set)
{
    string bvid;
    string bri_port_id;
    string port_name;
    string mac;
    string type;
    string vlanid;
    int vid;
    size_t pos1 = 0;
    size_t pos2 = 0;
    std::unordered_map<std::string, std:: string> oid_2_portname_map;
    std::map<std::string,std:: string> brPortId_2_attrPortId_map;
    std::unordered_map<std::string, std::string>::iterator type_it;
    std::unordered_map<std::string, std::string>::iterator brPortId_it;
    std::map<std::string, std::string>::iterator brPortId_2_attrPortId_it;
    std::unordered_map<std::string, std::string>::iterator oid_2_portName_it;
    
    auto keys = p_redisClient_2_asic->keys("ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY:*");

    for (auto& key : keys)
    {
        /*get vid*/
        pos1 = key.find("vlan", 0);
        if (pos1 != key.npos)
        {
            pos1 = pos1 + 7;
            pos2 = key.find(",", pos1) -2;
            vlanid = key.substr(pos1, pos2 - pos1 + 1);
        }
        else
        {
            pos1 = key.find("oid:", 0);
            pos2 = key.find(",", 0) - 2;
            bvid = key.substr(pos1, pos2 - pos1 + 1);            
            mclagsyncd_get_vid_by_bvid(p_redisClient_2_asic, bvid, vlanid);            
        }
   
        vid = atoi(vlanid.c_str());
        /*get mac*/
        pos1 = key.find("mac", 0) + 6;
        pos2 = key.find(",", pos1) -2;
        mac = key.substr(pos1, pos2 - pos1 + 1);

        /*get type*/
        auto hash = p_redisClient_2_asic->hgetall(key);
        type_it = hash.find("SAI_FDB_ENTRY_ATTR_TYPE");
        if (type_it == hash.end())
        {
            continue;  
        }

        if (memcmp(type_it->second.c_str(), "SAI_FDB_ENTRY_TYPE_DYNAMIC", type_it->second.length()) == 0)
            type = "dynamic";
        else
            type = "static";
     
        /*get port name*/
        mclagsyncd_get_oid_2_port_name_map(p_redisClient_2_counters, 
                oid_2_portname_map);
#if 0       
        SWSS_LOG_NOTICE("oid_2_portname_map:");
        {
            std::unordered_map<std::string,std:: string>::iterator it;
            for (it = oid_2_portname_map.begin(); it != oid_2_portname_map.end(); ++it)
                SWSS_LOG_NOTICE("%s vs %s", it->first.c_str(), it->second.c_str());
         }
#endif
        mclagsyncd_get_bridgePortId_2_attrPortId_map(p_redisClient_2_asic, 
                &brPortId_2_attrPortId_map);
#if 0        
        SWSS_LOG_NOTICE("brPortId_2_attrPortId_map:");
        {
            std::map<std::string,std:: string>::iterator it;
            for (it = brPortId_2_attrPortId_map.begin(); it != brPortId_2_attrPortId_map.end(); ++it)
                SWSS_LOG_NOTICE("%s vs %s", it->first.c_str(), it->second.c_str());
        }
#endif     
        brPortId_it = hash.find("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID");
        if (brPortId_it == hash.end())
        {  
            continue; 
        }
        bri_port_id = brPortId_it->second; 

        brPortId_2_attrPortId_it = brPortId_2_attrPortId_map.find(bri_port_id);
        if (brPortId_2_attrPortId_it == brPortId_2_attrPortId_map.end())
        {
            continue;      
        }
        
        oid_2_portName_it = oid_2_portname_map.find(brPortId_2_attrPortId_it->second);
        if (oid_2_portName_it == oid_2_portname_map.end())
        {
            continue;
        }
        
        port_name = oid_2_portName_it->second;

        /*insert set*/
        SWSS_LOG_NOTICE("read one fdb entry(mac:%s, vid:%d, port_name:%s, type:%s) from ASIC_DB and insert new_set.", mac.c_str(), vid, port_name.c_str(), type.c_str());
        fdb_set->insert(mclag_fdb(mac, vid, port_name, type));
    }

    return;
}

void mclagsyncd_set_port_isolate(RedisClient *p_redisClient_2_cfg, ProducerStateTable *p_acl_tbl,  
        ProducerStateTable *p_acl_rule_tbl, std::map<std::string,std:: string> *p_isolate, char *msg)
{
    mclag_sub_option_hdr_t *op_hdr = NULL;
    string isolate_src_port;
    string isolate_dst_port;
    char * cur = NULL;
    string acl_name = "mclag";
    string acl_rule_name = "mclag|mclag";
    vector<FieldValueTuple> acl_attrs;
    vector<FieldValueTuple> acl_rule_attrs;
    std::string acl_key = "ACL_TABLE|" + acl_name;
    std::string acl_rule_key = "ACL_RULE|" + acl_rule_name;

    cur = msg;
    
    /*get isolate src port infor*/
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    isolate_src_port.insert(0, (const char*)cur, op_hdr->op_len);
            
    cur = cur + op_hdr->op_len;

    /*get isolate dst ports infor*/
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    isolate_dst_port.insert(0, (const char*)cur, op_hdr->op_len);

    SWSS_LOG_NOTICE("Enter mclagsyncd_set_port_isolate");
             
    if (op_hdr->op_len == 0)
    {
        /* If dst port is NULL, delete the acl table 'mclag' */
        p_redisClient_2_cfg->del(acl_key);
        SWSS_LOG_NOTICE("set port isolate, src port: %s, dst port is NULL", 
             isolate_src_port.c_str());
        return;
    }

    SWSS_LOG_NOTICE("set port isolate, src port: %s, dst port: %s", 
             isolate_src_port.c_str(), isolate_dst_port.c_str());  

    /*First create ACL table*/
    FieldValueTuple desc_attr("POLICY_DESC","Mclag egress port isolate acl");
    acl_attrs.push_back(desc_attr);

    FieldValueTuple type_attr("TYPE","L3");
    acl_attrs.push_back(type_attr);

    FieldValueTuple port_attr("PORTS",isolate_src_port);
    acl_attrs.push_back(port_attr);

    p_redisClient_2_cfg->hmset(acl_key, acl_attrs.begin(), acl_attrs.end());
    /*End create ACL table*/

    /*Then create ACL rule table*/
    FieldValueTuple ip_type_attr("IP_TYPE","ANY");
    acl_rule_attrs.push_back(ip_type_attr);
    
    FieldValueTuple out_port_attr("OUT_PORTS",isolate_dst_port);
    acl_rule_attrs.push_back(out_port_attr);

    FieldValueTuple packet_attr("PACKET_ACTION","DROP");
    acl_rule_attrs.push_back(packet_attr);

    p_redisClient_2_cfg->hmset(acl_rule_key, acl_rule_attrs.begin(), acl_rule_attrs.end());
    /*End create ACL rule table*/
    
    return;
}

void mclagsyncd_set_port_mac_learn_mode(ProducerStateTable *p_port_tbl, 
        ProducerStateTable *p_lag_tbl, std::map<std::string,std:: string> *p_learn, char *msg)
{
    string learn_port;
    string learn_mode;
    mclag_sub_option_hdr_t *op_hdr = NULL;
    char * cur = NULL;
    map<string, string>::iterator it;
    size_t index = 0;

    cur = msg;
            
    /*get port learning mode info*/
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    if (op_hdr->op_type == MCLAG_SUB_OPTION_TYPE_MAC_LEARN_ENABLE)
    {
        learn_mode = "enabled";
    }
    else if (op_hdr->op_type == MCLAG_SUB_OPTION_TYPE_MAC_LEARN_DISABLE)
    {
        learn_mode = "disabled";
    }

    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;

    learn_port.insert(0, (const char*)cur, op_hdr->op_len);

    std::vector<string> res = string_split(learn_port, ",");
    for (index= 0; index < res.size(); ++index)
    {
        it = p_learn->find(res[index]);
        if (it == p_learn->end())
            p_learn->insert(pair<string, string>(res[index], learn_mode));
        else
            it->second = learn_mode;

        vector<FieldValueTuple> attrs;
        FieldValueTuple learn_attr("learn_mode", learn_mode);
        attrs.push_back(learn_attr);
        if(strncmp(res[index].c_str(),"PortC",5)==0)
            p_lag_tbl->set(res[index], attrs);            
        else
            p_port_tbl->set(res[index], attrs); 

        SWSS_LOG_NOTICE("set port mac learn mode, port: %s, learn-mode: %s", 
             res[index].c_str(), learn_mode.c_str()); 
    }
    
    return;
}

void mclagsyncd_set_fdb_flush(DBConnector *p_appl_db)
{
     swss::NotificationProducer flushFdb(p_appl_db, "FLUSHFDBREQUEST");
         
     vector<FieldValueTuple> values;
         
     SWSS_LOG_NOTICE("send fdb flush notification");
         
     flushFdb.send("ALL", "ALL", values);

     return;
}

void mclagsyncd_set_fdb_flush_by_port(DBConnector *p_appl_db, char *msg)
{
     string port;
     char *cur = NULL;
     mclag_sub_option_hdr_t *op_hdr = NULL;
     swss::NotificationProducer flushFdb(p_appl_db, "FLUSHFDBREQUEST");
     vector<FieldValueTuple> values;

     cur = msg;
      /*get port infor*/
     op_hdr = (mclag_sub_option_hdr_t *)cur;
     cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
     port.insert(0, (const char*)cur, op_hdr->op_len);
         
     SWSS_LOG_NOTICE("send fdb flush by port %s notification", port.c_str());
         
     flushFdb.send("ALL", port, values);

     return;
}

void mclagsyncd_set_intf_mac(ProducerStateTable *p_intf_tbl, char *msg)
{
    mclag_sub_option_hdr_t *op_hdr = NULL;
    string intf_key;
    string mac_value;
    char *cur = NULL;

    cur = msg;

    /*get intf key name*/
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    intf_key.insert(0, (const char*)cur, op_hdr->op_len);
            
    cur = cur + op_hdr->op_len;

    /*get mac*/
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    mac_value.insert(0, (const char*)cur, op_hdr->op_len);

    SWSS_LOG_NOTICE("set mac to chip, intf key name: %s, mac: %s", intf_key.c_str(), mac_value.c_str());        
    vector<FieldValueTuple> attrs;
    FieldValueTuple mac_attr("mac_addr",mac_value);
    attrs.push_back(mac_attr);
    p_intf_tbl->set(intf_key, attrs);

    return;
}

void mclagsyncd_set_fdb_entry(ProducerStateTable *p_fdb_tbl, std::set <mclag_fdb> *p_old_fdb, char *msg, int msg_len)
{
    struct mclag_fdb_info * fdb_info = NULL;
    struct mclag_fdb fdb;
    string fdb_key;
    char key[64] = {0};
    char *cur = NULL;
    short count = 0;
    int index = 0;
    int exist = 0;
    set <mclag_fdb>::iterator it;

    cur = msg;           
    count = (short)(msg_len/sizeof(struct mclag_fdb_info));

    for (index =0; index < count; index ++)
    {
        memset(key, 0, 64);
        
        fdb_info = (struct mclag_fdb_info *)(cur + index * sizeof(struct mclag_fdb_info));

        fdb.mac  = fdb_info->mac;
        fdb.port_name = fdb_info->port_name;
        fdb.vid = fdb_info->vid;
        if (fdb_info->type == MCLAG_FDB_TYPE_STATIC)
            fdb.type = "static";
        else
            fdb.type = "dynamic";

        if ((it = find(p_old_fdb->begin(), p_old_fdb->end(), fdb)) == p_old_fdb->end())
            exist = 0;
        else
            exist = 1;

        if (exist)
            SWSS_LOG_NOTICE("found fdb entry from old_set");

        snprintf(key, 64, "%s%d:%s", "Vlan", fdb_info->vid, fdb_info->mac);
        fdb_key = key;

        if (fdb_info->op_type == MCLAG_FDB_OPER_ADD)
        {
            vector<FieldValueTuple> attrs;
        
            /*set port attr*/
            FieldValueTuple port_attr("port", fdb.port_name);
            attrs.push_back(port_attr);

            /*set type attr*/
            FieldValueTuple type_attr("type", fdb.type);
            attrs.push_back(type_attr);

            if (exist == 0)
            {
                p_old_fdb->insert(fdb);
                SWSS_LOG_NOTICE("insert node(portname =%s, mac =%s, vid =%d, type =%s) into old_fdb_set", 
                    fdb.port_name.c_str(), fdb.mac.c_str(), fdb.vid, fdb.type.c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("modify node(mac =%s, vid =%d, portname :%s ==> %s, type:%s ==>%s)", 
                    fdb.mac.c_str(), fdb.vid, it->port_name.c_str(), fdb.port_name.c_str(), it->type.c_str(), fdb.type.c_str());
                p_old_fdb->erase(it);
                p_old_fdb->insert(fdb);
                #if 0
                fdb_entry = &(*it);
                fdb_entry->port_name = fdb.port_name;
                fdb_entry->type = fdb.type;
                #endif
            }

            p_fdb_tbl->set(fdb_key, attrs);
            SWSS_LOG_NOTICE("add fdb entry into ASIC_DB:key =%s, type =%s", fdb_key.c_str(),  fdb.type.c_str());           
        }
        else if (fdb_info->op_type == MCLAG_FDB_OPER_DEL)
        {
            if (exist)
            {
                p_old_fdb->erase(it);
                SWSS_LOG_NOTICE("erase node(portname =%s, mac =%s, vid =%d, type =%s) from old_fdb_set", 
                    it->port_name.c_str(), it->mac.c_str(), it->vid, it->type.c_str());
            }
            p_fdb_tbl->del(fdb_key);
            SWSS_LOG_NOTICE("del fdb entry from ASIC_DB:key =%s", fdb_key.c_str());  
        }
    }
    return;
}

ssize_t  mclagsyncd_get_fdb_changes(RedisClient *p_redisClient_2_asic,
        RedisClient *p_redisClient_2_counters, std::set <mclag_fdb> *p_old_fdb,
        char *msg_buf, int m_connection_socket)
{
    set <mclag_fdb> new_fdb;
    set <mclag_fdb> del_fdb;
    set <mclag_fdb> add_fdb;
    struct mclag_fdb_info info;
    mclag_msg_hdr_t * msg_head = NULL;
    ssize_t write = 0;
    size_t infor_len = 0;
    char *infor_start = msg_buf;
    set <mclag_fdb> *p_new_fdb = &new_fdb;

    del_fdb.clear();
    add_fdb.clear();
    p_new_fdb->clear();

    infor_len = infor_len + sizeof(mclag_msg_hdr_t);
                
    mclagsyncd_get_fdb_set(p_redisClient_2_asic, p_redisClient_2_counters ,p_new_fdb);
 
    set_difference(p_old_fdb->begin(), p_old_fdb->end(), p_new_fdb->begin(), 
                p_new_fdb->end(), inserter(del_fdb, del_fdb.begin()));
    set_difference(p_new_fdb->begin(), p_new_fdb->end(), p_old_fdb->begin(), 
                p_old_fdb->end(), inserter(add_fdb, add_fdb.begin()));

    p_old_fdb->swap(*p_new_fdb);

    for (auto it = del_fdb.begin(); it != del_fdb.end(); it++)
    {
        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < sizeof(struct mclag_fdb_info))
        {
            msg_head = (mclag_msg_hdr_t *)infor_start;
            msg_head->version = 1;
            msg_head->msg_len = (unsigned short)infor_len;
            msg_head ->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

            SWSS_LOG_NOTICE("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d", 
                msg_head->msg_len, msg_head->msg_type);
            write = ::write(m_connection_socket, infor_start, msg_head->msg_len);
            if (write <= 0)
                return write;
            
            infor_len = 0;
        }
        SWSS_LOG_NOTICE("notify iccpd to del fdb_entry:mac:%s, vid:%d, portname:%s, type:%s", 
                    it->mac.c_str(), it->vid, it->port_name.c_str(), it->type.c_str());
        memset(&info, 0, sizeof(struct mclag_fdb_info));
        info.op_type = MCLAG_FDB_OPER_DEL;
        memcpy(info.mac, it->mac.c_str(), it->mac.length());
        info.vid = it->vid;
        memcpy(info.port_name, it->port_name.c_str(), it->port_name.length());
        if (memcmp(it->type.c_str(), "SAI_FDB_ENTRY_TYPE_DYNAMIC", it->type.length()) == 0)
            info.type = MCLAG_FDB_TYPE_DYNAMIC;
        else
            info.type = MCLAG_FDB_TYPE_STATIC;

        memcpy((char*)(infor_start + infor_len), (char*)&info, sizeof(struct mclag_fdb_info));
        infor_len = infor_len + sizeof(struct mclag_fdb_info);
                
    }

    for (auto it = add_fdb.begin(); it != add_fdb.end(); it++)
    {
        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < sizeof(struct mclag_fdb_info))
        {
            msg_head = (mclag_msg_hdr_t *)infor_start;
            msg_head->version = 1;
            msg_head->msg_len = (unsigned short)infor_len;
            msg_head ->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

            SWSS_LOG_NOTICE("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d", 
                msg_head->msg_len, msg_head->msg_type);
            write = ::write(m_connection_socket, infor_start, msg_head->msg_len);
            if (write <= 0)
                return write;

            infor_len = 0;
        }
        SWSS_LOG_NOTICE("notify iccpd to add fdb_entry:mac:%s, vid:%d, portname:%s, type:%s", 
                    it->mac.c_str(), it->vid, it->port_name.c_str(), it->type.c_str());
        memset(&info, 0, sizeof(struct mclag_fdb_info));
        info.op_type = MCLAG_FDB_OPER_ADD;
        memcpy(info.mac, it->mac.c_str(), it->mac.length());
        info.vid = it->vid;
        memcpy(info.port_name, it->port_name.c_str(), it->port_name.length());
        if (memcmp(it->type.c_str(), "dynamic", it->type.length()) == 0)
            info.type = MCLAG_FDB_TYPE_DYNAMIC;
        else
            info.type = MCLAG_FDB_TYPE_STATIC;

        memcpy((char*)(infor_start + infor_len), (char*)&info, sizeof(struct mclag_fdb_info));
        infor_len = infor_len +  sizeof(struct mclag_fdb_info); 
    }

    if (infor_len <= sizeof(mclag_msg_hdr_t)) /*no fdb entry need notifying iccpd*/ 
        return 1;

    msg_head = (mclag_msg_hdr_t *)infor_start;
    msg_head->version = 1;
    msg_head->msg_len = (unsigned short)infor_len;
    msg_head ->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

    SWSS_LOG_NOTICE("mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d", 
                msg_head->msg_len, msg_head->msg_type);
    write = ::write(m_connection_socket, infor_start, msg_head->msg_len);

    return write;
}

MclagLink::MclagLink(int port) :
    MSG_BATCH_SIZE(256),
    m_bufSize(MCLAG_MAX_MSG_LEN * MSG_BATCH_SIZE),
    m_messageBuffer(NULL),
    m_pos(0),
    m_connected(false),
    m_server_up(false)
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

    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
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
}

MclagLink::~MclagLink()
{
    delete m_messageBuffer;
    delete m_messageBuffer_send;
    if (m_connected)
        close(m_connection_socket);
    if (m_server_up)
        close(m_server_socket);
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

void MclagLink::readData()
{
    mclag_msg_hdr_t *hdr = NULL;
    size_t msg_len = 0;
    size_t start = 0, left = 0;
    ssize_t read = 0;
    ssize_t write = 0;
    char * msg = NULL;

    read = ::read(m_connection_socket, m_messageBuffer + m_pos, m_bufSize - m_pos);
    if (read == 0)
        throw MclagConnectionClosedException();
    if (read < 0)
        throw system_error(errno, system_category());
    m_pos+= (uint32_t)read;

    while (true)
    { 
        hdr = (mclag_msg_hdr_t *)(m_messageBuffer + start);
        left = m_pos - start;
        if (left < MCLAG_MSG_HDR_LEN)
            break;
  
        msg_len = mclag_msg_len(hdr);
        if (left < msg_len)
            break;

        if (!mclag_msg_ok(hdr, left))
            throw system_error(make_error_code(errc::bad_message), "Malformed MCLAG message received");

        msg = ((char*) hdr) + MCLAG_MSG_HDR_LEN;

        switch (hdr->msg_type)
        {
            case MCLAG_MSG_TYPE_PORT_ISOLATE:
                    /*mclagsyncd_set_port_isolate(p_port_tbl, p_isolate, msg);*/
                    mclagsyncd_set_port_isolate(p_redisClient_2_cfg, p_acl_tbl, p_acl_rule_tbl, p_isolate, msg);
                    break;
            case MCLAG_MSG_TYPE_PORT_MAC_LEARN_MODE:
                    mclagsyncd_set_port_mac_learn_mode(p_port_tbl, p_lag_tbl, p_learn, msg);
                    break;
            case MCLAG_MSG_TYPE_FLUSH_FDB:
                    mclagsyncd_set_fdb_flush(p_appl_db);
                    break;
            case MCLAG_MSG_TYPE_FLUSH_FDB_BY_PORT:
                    mclagsyncd_set_fdb_flush_by_port(p_appl_db, msg);
                    break;
            case MCLAG_MSG_TYPE_SET_INTF_MAC:
                    mclagsyncd_set_intf_mac(p_intf_tbl, msg);
                    break;
            case MCLAG_MSG_TYPE_SET_FDB:
                    mclagsyncd_set_fdb_entry(p_fdb_tbl, p_old_fdb, msg, (int)(hdr->msg_len - sizeof(mclag_msg_hdr_t)));
                    break;
            case MCLAG_MSG_TYPE_GET_FDB_CHANGES:
                    write = mclagsyncd_get_fdb_changes(p_redisClient_2_asic, p_redisClient_2_counters, 
                    p_old_fdb, m_messageBuffer_send, m_connection_socket);
                    if (write == 0)
                        throw MclagConnectionClosedException();
                    if (write < 0)
                        throw system_error(errno, system_category());
                    break;
            default:
                    break;
        }

        start += msg_len;
    }
    memmove(m_messageBuffer, m_messageBuffer + start, m_pos - start);
    m_pos = m_pos - (uint32_t)start;
    return;
}

 void mclag_connection_lost_handle_port_Isolate(RedisClient *p_redisClient_2_cfg)
{
     string acl_name = "mclag";
     std::string acl_key = "ACL_TABLE|" + acl_name;
     
     p_redisClient_2_cfg->del(acl_key);
     
     return;
}

  void mclag_connection_lost_handle_port_learn_mode(std::map<std::string,
        std:: string> *p_learn,  ProducerStateTable * p_port_tbl, ProducerStateTable * p_lag_tbl)
 {
     vector<FieldValueTuple> attrs;
     std::map<std::string,std:: string>::iterator it;
     
     it = p_learn->begin();
     
     while (it != p_learn->end())
     {
        it->second = "enabled";

        FieldValueTuple block_attr("learn_mode",it->second);

        attrs.push_back(block_attr);
        if(strncmp(it->first.c_str(),"PortC",5) == 0)
            p_lag_tbl->set(it->first, attrs);
        else
            p_port_tbl->set(it->first, attrs);     

        it ++;
     }

     p_learn->clear();
     
     return;
 }

 void mclag_connection_lost( std::map<std::string,std:: string> *p_isolate, std::map<std::string,std:: string> *p_learn, 
            ProducerStateTable * p_port_tbl, ProducerStateTable * p_lag_tbl, RedisClient *p_redisClient_2_cfg)
 {
     mclag_connection_lost_handle_port_Isolate(p_redisClient_2_cfg);     
     mclag_connection_lost_handle_port_learn_mode(p_learn, p_port_tbl, p_lag_tbl);
     return;
 }


