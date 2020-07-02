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

void MclagLink::getOidToPortNameMap(std::unordered_map<std::string, std:: string> & port_map)
{
    std::unordered_map<std::string, std:: string>::iterator it;
    auto hash = p_redisClient_to_counters->hgetall("COUNTERS_PORT_NAME_MAP");

    for (it = hash.begin(); it != hash.end(); ++it)
        port_map.insert(pair<string, string>(it->second, it->first));

    return;
}

void MclagLink::getBridgePortIdToAttrPortIdMap(std::map<std::string, std:: string> *oid_map)
{
    std::string bridge_port_id;
    size_t pos1 = 0;

    std::unordered_map<string, string>::iterator attr_port_id;

    auto keys = p_redisClient_to_asic->keys("ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT:*");

    for (auto& key : keys)
    {
        pos1 = key.find("oid:", 0);
        bridge_port_id = key.substr(pos1);

        auto hash = p_redisClient_to_asic->hgetall(key);
        attr_port_id = hash.find("SAI_BRIDGE_PORT_ATTR_PORT_ID");
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
    std::unordered_map<std::string, std::string>::iterator attr_vlan_id;
    std::string pre = "ASIC_STATE:SAI_OBJECT_TYPE_VLAN:";
    std::string key = pre + bvid;

    auto hash = p_redisClient_to_asic->hgetall(key.c_str());

    attr_vlan_id = hash.find("SAI_VLAN_ATTR_VLAN_ID");
    if (attr_vlan_id == hash.end())
        return;

    vlanid = attr_vlan_id->second;
    return;
}

void MclagLink::getFdbSet(std::set<mclag_fdb> *fdb_set)
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
    std::unordered_map<std::string, std:: string> oid_to_portname_map;
    std::map<std::string, std:: string> brPortId_to_attrPortId_map;
    std::unordered_map<std::string, std::string>::iterator type_it;
    std::unordered_map<std::string, std::string>::iterator brPortId_it;
    std::map<std::string, std::string>::iterator brPortId_to_attrPortId_it;
    std::unordered_map<std::string, std::string>::iterator oid_to_portName_it;

    auto keys = p_redisClient_to_asic->keys("ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY:*");

    for (auto& key : keys)
    {
        /*get vid*/
        pos1 = key.find("vlan", 0);
        if (pos1 != key.npos)
        {
            pos1 = pos1 + 7;
            pos2 = key.find(",", pos1) - 2;
            vlanid = key.substr(pos1, pos2 - pos1 + 1);
        }
        else
        {
            pos1 = key.find("oid:", 0);
            pos2 = key.find(",", 0) - 2;
            bvid = key.substr(pos1, pos2 - pos1 + 1);
            getVidByBvid(bvid, vlanid);
        }

        vid = atoi(vlanid.c_str());
        /*get mac*/
        pos1 = key.find("mac", 0) + 6;
        pos2 = key.find(",", pos1) - 2;
        mac = key.substr(pos1, pos2 - pos1 + 1);

        /*get type*/
        auto hash = p_redisClient_to_asic->hgetall(key);
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
        getOidToPortNameMap(oid_to_portname_map);
        getBridgePortIdToAttrPortIdMap(&brPortId_to_attrPortId_map);
        brPortId_it = hash.find("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID");
        if (brPortId_it == hash.end())
        {
            continue;
        }
        bri_port_id = brPortId_it->second;

        brPortId_to_attrPortId_it = brPortId_to_attrPortId_map.find(bri_port_id);
        if (brPortId_to_attrPortId_it == brPortId_to_attrPortId_map.end())
        {
            continue;
        }

        oid_to_portName_it = oid_to_portname_map.find(brPortId_to_attrPortId_it->second);
        if (oid_to_portName_it == oid_to_portname_map.end())
        {
            continue;
        }

        port_name = oid_to_portName_it->second;

        /*insert set*/
        SWSS_LOG_DEBUG("Read one fdb entry(MAC:%s, vid:%d, port_name:%s, type:%s) from ASIC_DB and insert new_set.", mac.c_str(), vid, port_name.c_str(), type.c_str());
        fdb_set->insert(mclag_fdb(mac, vid, port_name, type));
    }

    return;
}

void MclagLink::setPortIsolate(char *msg)
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
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    isolate_src_port.insert(0, (const char*)cur, op_hdr->op_len);

    cur = cur + op_hdr->op_len;

    /*get isolate dst ports infor*/
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    isolate_dst_port.insert(0, (const char*)cur, op_hdr->op_len);

    if (op_hdr->op_len == 0)
    {
        /* If dst port is NULL, delete the acl table 'mclag' */
        p_acl_table_tbl->del(acl_name);
        acl_table_is_added = 0;
        SWSS_LOG_DEBUG("Disable port isolate, src port: %s, dst port is NULL",
                        isolate_src_port.c_str());
        return;
    }

    SWSS_LOG_DEBUG("Set port isolate, src port: %s, dst port: %s",
                    isolate_src_port.c_str(), isolate_dst_port.c_str());

    if (acl_table_is_added == 0)
    {
        /*First create ACL table*/
        FieldValueTuple desc_attr("policy_desc", "Mclag egress port isolate acl");
        acl_attrs.push_back(desc_attr);

        FieldValueTuple type_attr("type", "MCLAG");
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

    FieldValueTuple out_port_attr("OUT_PORTS", isolate_dst_port);
    acl_rule_attrs.push_back(out_port_attr);

    FieldValueTuple packet_attr("PACKET_ACTION", "DROP");
    acl_rule_attrs.push_back(packet_attr);

    p_acl_rule_tbl->set(acl_rule_name, acl_rule_attrs);
    /*End create ACL rule table*/

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
    op_hdr = (mclag_sub_option_hdr_t *)cur;
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
    /*vxlan tunnel dont supported currently, for src_ip is the mandatory attribute*/
    /*else if(strncmp(learn_port.c_str(),VXLAN_TUNNEL_PREFIX,5)==0)
        p_tnl_tbl->set(learn_port, attrs); */
    else
        p_port_tbl->set(learn_port, attrs);

    SWSS_LOG_DEBUG("Set port mac learn mode, port: %s, learn-mode: %s",
                    learn_port.c_str(), learn_mode.c_str());

    return;
}

void MclagLink::setFdbFlush()
{
    swss::NotificationProducer flushFdb(p_appl_db, "FLUSHFDBREQUEST");

    vector<FieldValueTuple> values;

    SWSS_LOG_DEBUG("Send fdb flush notification");

    flushFdb.send("ALL", "ALL", values);

    return;
}

void MclagLink::setFdbFlushByPort(char *msg)
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

    SWSS_LOG_DEBUG("Send fdb flush by port %s notification", port.c_str());

    flushFdb.send("ALL", port, values);

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
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    intf_key.insert(0, (const char*)cur, op_hdr->op_len);

    cur = cur + op_hdr->op_len;

    /*get mac*/
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    mac_value.insert(0, (const char*)cur, op_hdr->op_len);

    SWSS_LOG_DEBUG("Set mac to chip, intf key name: %s, mac: %s", intf_key.c_str(), mac_value.c_str());
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
    int exist = 0;
    set <mclag_fdb>::iterator it;

    cur = msg;
    count = (short)(msg_len / sizeof(struct mclag_fdb_info));

    for (index = 0; index < count; index++)
    {
        memset(key, 0, 64);

        fdb_info = (struct mclag_fdb_info *)(cur + index * sizeof(struct mclag_fdb_info));

        fdb.mac = fdb_info->mac;
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
                SWSS_LOG_DEBUG("Insert node(portname =%s, mac =%s, vid =%d, type =%s) into old_fdb_set",
                                fdb.port_name.c_str(), fdb.mac.c_str(), fdb.vid, fdb.type.c_str());
            }
            else
            {
                if (it->port_name == fdb.port_name && it->type == fdb.type)
                {
                    SWSS_LOG_DEBUG("All items of mac is same (mac =%s, vid =%d, portname :%s ==> %s, type:%s ==>%s), return.",
                                fdb.mac.c_str(), fdb.vid, it->port_name.c_str(), fdb.port_name.c_str(), it->type.c_str(), fdb.type.c_str());
                    return;
                }
                SWSS_LOG_DEBUG("Modify node(mac =%s, vid =%d, portname :%s ==> %s, type:%s ==>%s)",
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
            SWSS_LOG_DEBUG("Add fdb entry into ASIC_DB:key =%s, type =%s", fdb_key.c_str(),  fdb.type.c_str());
        }
        else if (fdb_info->op_type == MCLAG_FDB_OPER_DEL)
        {
            if (exist)
            {
                SWSS_LOG_DEBUG("Erase node(portname =%s, mac =%s, vid =%d, type =%s) from old_fdb_set",
                                it->port_name.c_str(), it->mac.c_str(), it->vid, it->type.c_str());
                p_old_fdb->erase(it);
            }
            p_fdb_tbl->del(fdb_key);
            SWSS_LOG_DEBUG("Del fdb entry from ASIC_DB:key =%s", fdb_key.c_str());
        }
    }

    return;
}

ssize_t  MclagLink::getFdbChange(char *msg_buf)
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

    getFdbSet(p_new_fdb);

    set_difference(p_old_fdb->begin(), p_old_fdb->end(), p_new_fdb->begin(),
                   p_new_fdb->end(), inserter(del_fdb, del_fdb.begin()));
    set_difference(p_new_fdb->begin(), p_new_fdb->end(), p_old_fdb->begin(),
                   p_old_fdb->end(), inserter(add_fdb, add_fdb.begin()));

    p_old_fdb->swap(*p_new_fdb);

    /*Remove the same item from del set, this may be MAC move*/
    auto itdel = del_fdb.begin();
    while (itdel != del_fdb.end())
    {
        auto ittmp = itdel;
        itdel++;
        for (auto itadd = add_fdb.begin(); itadd != add_fdb.end(); itadd++)
        {
            if (ittmp->mac == itadd->mac && ittmp->vid == itadd->vid)
            {
                SWSS_LOG_DEBUG("Mac move: mac %s, vid %d, portname %s, type %s",
                        ittmp->mac.c_str(), ittmp->vid, ittmp->port_name.c_str(), ittmp->type.c_str());
                del_fdb.erase(ittmp);
                break;
            }
        }
    }

    for (auto it = del_fdb.begin(); it != del_fdb.end(); it++)
    {
        if (MCLAG_MAX_SEND_MSG_LEN - infor_len < sizeof(struct mclag_fdb_info))
        {
            msg_head = (mclag_msg_hdr_t *)infor_start;
            msg_head->version = 1;
            msg_head->msg_len = (unsigned short)infor_len;
            msg_head->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

            SWSS_LOG_DEBUG("Mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d",
                            msg_head->msg_len, msg_head->msg_type);
            write = ::write(m_connection_socket, infor_start, msg_head->msg_len);
            if (write <= 0)
                return write;

            infor_len = sizeof(mclag_msg_hdr_t);
        }
        SWSS_LOG_DEBUG("Notify iccpd to del fdb_entry:mac:%s, vid:%d, portname:%s, type:%s",
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
            msg_head->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

            /*SWSS_LOG_DEBUG("Mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d",
                            msg_head->msg_len, msg_head->msg_type);*/
            write = ::write(m_connection_socket, infor_start, msg_head->msg_len);
            if (write <= 0)
                return write;

            infor_len = sizeof(mclag_msg_hdr_t);
        }
        SWSS_LOG_DEBUG("Notify iccpd to add fdb_entry:mac:%s, vid:%d, portname:%s, type:%s",
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
    msg_head->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

    /*SWSS_LOG_DEBUG("Mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d",
                    msg_head->msg_len, msg_head->msg_type);*/
    write = ::write(m_connection_socket, infor_start, msg_head->msg_len);

    return write;
}

MclagLink::MclagLink(uint16_t port) :
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

    memset(&addr, 0, sizeof(addr));
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
    delete[] m_messageBuffer;
    delete[] m_messageBuffer_send;
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

uint64_t MclagLink::readData()
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
    m_pos += (uint32_t)read;

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

            case MCLAG_MSG_TYPE_FLUSH_FDB_BY_PORT:
                setFdbFlushByPort(msg);
                break;

            case MCLAG_MSG_TYPE_SET_INTF_MAC:
                setIntfMac(msg);
                break;

            case MCLAG_MSG_TYPE_SET_FDB:
                setFdbEntry(msg, (int)(hdr->msg_len - sizeof(mclag_msg_hdr_t)));
                break;

            case MCLAG_MSG_TYPE_GET_FDB_CHANGES:
                write = getFdbChange(m_messageBuffer_send);
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

    return 0;
}

