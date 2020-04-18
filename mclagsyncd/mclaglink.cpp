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
#include <stdio.h>
#include <iostream>
#include "logger.h"
#include "netmsg.h"
#include "netdispatcher.h"
#include "swss/notificationproducer.h"
#include "mclaglink.h"
#include "mclag.h"
#include <algorithm>
#include "schema.h"
#include "tokenize.h"

using namespace swss;
using namespace std;

void MclagLink::setPortIsolate(char *msg)
{
    mclag_sub_option_hdr_t *op_hdr = NULL;
    string isolate_src_port;
    string isolate_dst_port;
    char *cur = NULL;
    string acl_name = "mclag";
    string acl_rule_name = "mclag:mclag";
    vector<FieldValueTuple> acl_attrs;
    vector<FieldValueTuple> acl_rule_attrs;
    string acl_key = string("") + APP_ACL_TABLE_TABLE_NAME + ":" + acl_name;
    string acl_rule_key = string("") + APP_ACL_RULE_TABLE_NAME + ":" + acl_rule_name;
    static int acl_table_is_added = 0;

    cur = msg;

    /* get isolate src port infor */
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    isolate_src_port.insert(0, (const char *)cur, op_hdr->op_len);

    cur = cur + op_hdr->op_len;

    /* get isolate dst ports infor */
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    isolate_dst_port.insert(0, (const char *)cur, op_hdr->op_len);

    if (op_hdr->op_len == 0)
    {
        /* If dst port is NULL, delete the acl table 'mclag' */
        m_aclTable.del(acl_name);
        acl_table_is_added = 0;
        SWSS_LOG_NOTICE("Set port isolate, src port: %s, dst port is NULL", isolate_src_port.c_str());
        return;
    }

    SWSS_LOG_NOTICE("Set port isolate, src port: %s, dst port: %s", isolate_src_port.c_str(), isolate_dst_port.c_str());

    if (acl_table_is_added == 0)
    {
        /* First create ACL table */
        FieldValueTuple desc_attr("policy_desc", "Mclag egress port isolate acl");
        acl_attrs.push_back(desc_attr);

        FieldValueTuple type_attr("type", "MCLAG");
        acl_attrs.push_back(type_attr);

        FieldValueTuple port_attr("ports", isolate_src_port);
        acl_attrs.push_back(port_attr);

        m_aclTable.set(acl_name, acl_attrs);
        acl_table_is_added = 1;
        /* End create ACL table */
    }

    /* Then create ACL rule table */
    FieldValueTuple ip_type_attr("IP_TYPE", "ANY");
    acl_rule_attrs.push_back(ip_type_attr);

    FieldValueTuple out_port_attr("OUT_PORTS", isolate_dst_port);
    acl_rule_attrs.push_back(out_port_attr);

    FieldValueTuple packet_attr("PACKET_ACTION", "DROP");
    acl_rule_attrs.push_back(packet_attr);

    m_aclRuleTable.set(acl_rule_name, acl_rule_attrs);
    /* End create ACL rule table */

    return;
}

void MclagLink::setPortLearnMode(char *msg)
{
    string learn_port;
    string learn_mode;
    mclag_sub_option_hdr_t *op_hdr = NULL;
    char *cur = NULL;

    cur = msg;

    /* get port learning mode info */
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

    learn_port.insert(0, (const char *)cur, op_hdr->op_len);

    vector<FieldValueTuple> attrs;
    FieldValueTuple learn_attr("learn_mode", learn_mode);
    attrs.push_back(learn_attr);
    if (strncmp(learn_port.c_str(), PORTCHANNEL_PREFIX, strlen(PORTCHANNEL_PREFIX)) == 0)
        m_lagTable.set(learn_port, attrs);
    /* vxlan tunnel dont supported currently, for src_ip is the mandatory attribute */
    /* else if(strncmp(learn_port.c_str(),VXLAN_TUNNEL_PREFIX,5) == 0) p_tnl_tbl->set(learn_port, attrs); */
    else
        m_portTable.set(learn_port, attrs);

    SWSS_LOG_NOTICE("Set port mac learn mode, port: %s, learn-mode: %s", learn_port.c_str(), learn_mode.c_str());

    return;
}

void MclagLink::flushFdb()
{
    swss::NotificationProducer flushFdb(&m_applDb, "FLUSHFDBREQUEST");

    vector<FieldValueTuple> values;

    SWSS_LOG_NOTICE("Send fdb flush notification");

    flushFdb.send("ALL", "ALL", values);

    return;
}

void MclagLink::flushFdbByPort(char *msg)
{
    string port;
    char *cur = NULL;
    mclag_sub_option_hdr_t *op_hdr = NULL;
    swss::NotificationProducer flushFdb(&m_applDb, "FLUSHFDBREQUEST");
    vector<FieldValueTuple> values;

    cur = msg;
    /* get port infor */
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    port.insert(0, (const char *)cur, op_hdr->op_len);

    SWSS_LOG_NOTICE("Send fdb flush by port %s notification", port.c_str());

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

    /* get intf key name */
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    intf_key.insert(0, (const char *)cur, op_hdr->op_len);

    cur = cur + op_hdr->op_len;

    /* get mac */
    op_hdr = (mclag_sub_option_hdr_t *)cur;
    cur = cur + MCLAG_SUB_OPTION_HDR_LEN;
    mac_value.insert(0, (const char *)cur, op_hdr->op_len);

    SWSS_LOG_NOTICE("Set mac to chip, intf key name: %s, mac: %s", intf_key.c_str(), mac_value.c_str());
    vector<FieldValueTuple> attrs;
    FieldValueTuple mac_attr("mac_addr", mac_value);
    attrs.push_back(mac_attr);
    m_intfTable.set(intf_key, attrs);

    return;
}

void MclagLink::setFdbEntry(char *msg, int msg_len)
{
    struct mclag_fdb_info *fdb_info = NULL;
    struct mclag_fdb fdb;
    string fdb_key;
    char key[64] = { 0 };
    char *cur = NULL;
    short count = 0;
    int index = 0;
    int exist = 0;
    set<mclag_fdb>::iterator it;

    cur = msg;
    count = (short)(msg_len / sizeof(struct mclag_fdb_info));

    for (index = 0; index < count; index++)
    {
        memset(key, 0, 64);

        fdb_info = (struct mclag_fdb_info *)(cur + index * sizeof(struct mclag_fdb_info));

        fdb.mac = fdb_info->mac;
        fdb.port_name = fdb_info->port_name;
        fdb.vid = fdb_info->vid;
        fdb.op_type = MCLAG_FDB_OPER_NONE;
        if (fdb_info->type == MCLAG_FDB_TYPE_STATIC)
            fdb.type = "static";
        else
            fdb.type = "dynamic";

        snprintf(key, 64, "%s%d:%s", "Vlan", fdb_info->vid, fdb_info->mac);
        fdb_key = key;

        if ((it = find(m_pFdbSet->begin(), m_pFdbSet->end(), fdb)) == m_pFdbSet->end())
            exist = 0;
        else
            exist = 1;

        if (fdb_info->op_type == MCLAG_FDB_OPER_ADD)
        {
            vector<FieldValueTuple> attrs;

            /* set port attr */
            FieldValueTuple port_attr("port", fdb.port_name);
            attrs.push_back(port_attr);

            /* set type attr */
            FieldValueTuple type_attr("type", fdb.type);
            attrs.push_back(type_attr);

            if (exist == 0)
            {
                m_pFdbSet->insert(fdb);
                SWSS_LOG_NOTICE("Insert node(portname =%s, mac =%s, vid =%d, type =%s) into fdb_set",
                               fdb.port_name.c_str(), fdb.mac.c_str(), fdb.vid, fdb.type.c_str());
            }
            else
            {
                if (it->port_name == fdb.port_name && it->type == fdb.type)
                {
                    SWSS_LOG_NOTICE("Insert node(portname =%s, mac =%s, vid =%d, type =%s), all the same",
                                   fdb.port_name.c_str(), fdb.mac.c_str(), fdb.vid, fdb.type.c_str());
                    continue;
                }

                SWSS_LOG_NOTICE("Modify node(mac =%s, vid =%d, portname :%s==>%s, type:%s==>%s)",
                               fdb.mac.c_str(), fdb.vid, it->port_name.c_str(), fdb.port_name.c_str(), it->type.c_str(), fdb.type.c_str());
                m_pFdbSet->erase(it);
                m_pFdbSet->insert(fdb);
            }

            m_fdbTable.set(fdb_key, attrs);
            SWSS_LOG_NOTICE("Add fdb entry into ASIC_DB:key =%s, type =%s", fdb_key.c_str(), fdb.type.c_str());
        }
        else if (fdb_info->op_type == MCLAG_FDB_OPER_DEL)
        {
            if (exist)
            {
                m_pFdbSet->erase(it);
                m_fdbTable.del(fdb_key);
                SWSS_LOG_NOTICE("Del fdb entry from ASIC_DB:key =%s", fdb_key.c_str());
            }
        }
    }

    return;
}

void MclagLink::notifyFdbChange()
{
    struct mclag_fdb_info info;
    mclag_msg_hdr_t *msg_head = NULL;
    ssize_t write = 0;
    size_t info_len = 0;
    char *info_start = m_msgSndBuf;

    info_len += sizeof(mclag_msg_hdr_t);

    for (auto it = m_pFdbEvent->begin(); it != m_pFdbEvent->end(); it++)
    {
        if (MCLAG_MAX_SEND_MSG_LEN - info_len < sizeof(struct mclag_fdb_info))
        {
            msg_head = (mclag_msg_hdr_t *)info_start;
            msg_head->version = 1;
            msg_head->msg_len = (unsigned short)info_len;
            msg_head->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

            SWSS_LOG_NOTICE("Mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d", msg_head->msg_len, msg_head->msg_type);

            write =::write(m_connectionSocket, info_start, msg_head->msg_len);
            if (write < 0)
            {
                m_connectionState = false;
                goto OUT;
            }

            info_len = sizeof(mclag_msg_hdr_t);
        }

        SWSS_LOG_NOTICE("Notify fdb msg(mac:%s, vid:%d, portname:%s, type:%s, op_type:%s) to iccpd.",
                       it->mac.c_str(), it->vid, it->port_name.c_str(), it->type.c_str(), (it->op_type == MCLAG_FDB_OPER_ADD) ? "add" : "del");

        memset(&info, 0, sizeof(struct mclag_fdb_info));
        info.op_type = it->op_type;
        memcpy(info.mac, it->mac.c_str(), it->mac.length());
        info.vid = it->vid;
        memcpy(info.port_name, it->port_name.c_str(), it->port_name.length());

        if (memcmp(it->type.c_str(), "dynamic", it->type.length()) == 0)
            info.type = MCLAG_FDB_TYPE_DYNAMIC;
        else
            info.type = MCLAG_FDB_TYPE_STATIC;

        memcpy((char *)(info_start + info_len), (char *)&info, sizeof(struct mclag_fdb_info));
        info_len = info_len + sizeof(struct mclag_fdb_info);
    }

    if (info_len <= sizeof(mclag_msg_hdr_t))    /* no fdb entry need notifying iccpd */
        goto OUT;

    msg_head = (mclag_msg_hdr_t *)info_start;
    msg_head->version = 1;
    msg_head->msg_len = (unsigned short)info_len;
    msg_head->msg_type = MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION;

    SWSS_LOG_NOTICE("Mclagsycnd send msg to iccpd, msg_len =%d, msg_type =%d", msg_head->msg_len, msg_head->msg_type);

    write =::write(m_connectionSocket, info_start, msg_head->msg_len);

    if (write < 0)
    {
        m_connectionState = false;
    }

OUT:
    m_pFdbEvent->clear();

    return;
}

vector<Selectable *>MclagLink::getFdbGatherSelectables()
{
    return m_fdbGather.getSelectables();
}

MclagLink::MclagLink(int fd) :
    m_connectionState(true),
    m_pos(0),
    m_bufSize(MCLAG_MAX_MSG_LEN * MSG_BATCH_SIZE),
    m_applDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_portTable(&m_applDb, APP_PORT_TABLE_NAME),
    m_lagTable(&m_applDb, APP_LAG_TABLE_NAME),
    m_tnlTable(&m_applDb, APP_VXLAN_TUNNEL_TABLE_NAME),
    m_intfTable(&m_applDb, APP_INTF_TABLE_NAME),
    m_fdbTable(&m_applDb, APP_FDB_TABLE_NAME),
    m_aclTable(&m_applDb, APP_ACL_TABLE_TABLE_NAME),
    m_aclRuleTable(&m_applDb, APP_ACL_RULE_TABLE_NAME),
    m_stateFdbTable(&m_stateDb, STATE_FDB_TABLE_NAME),
    m_fdbGatherTables(1, m_stateFdbTable),
    m_fdbGather(&m_stateDb, m_fdbGatherTables)
{
    m_connectionSocket = fd;
    m_pFdbEvent = &m_fdbGather.m_fdbEvent;
    m_pFdbSet = &m_fdbGather.m_fdbSet;
    m_fdbGather.getFdbFromStatedb();
}

MclagLink::~MclagLink()
{
    m_pFdbSet->clear();

    if (m_connectionState)
        close(m_connectionSocket);
}

int MclagLink::getFd()
{
    return m_connectionSocket;
}

uint64_t MclagLink::readData()
{
    mclag_msg_hdr_t *hdr = NULL;
    size_t msg_len = 0;
    size_t start = 0, left = 0;
    ssize_t read = 0;
    char *msg = NULL;

    read =::read(m_connectionSocket, m_msgBuf + m_pos, m_bufSize - m_pos);

    if (read == 0)
    {
        SWSS_LOG_NOTICE("Read data error! connection lost?\n");
        return 0;
    }

    if (read < 0)
        throw system_error(errno, system_category());

    m_pos += (uint32_t)read;

    while (true)
    {
        hdr = (mclag_msg_hdr_t *) (m_msgBuf + start);
        left = m_pos - start;
        if (left < MCLAG_MSG_HDR_LEN)
            break;

        msg_len = mclag_msg_len(hdr);
        if (left < msg_len)
            break;

        if (!mclag_msg_ok(hdr, left))
            throw system_error(make_error_code(errc::bad_message), "Malformed MCLAG message received");

        msg = ((char *)hdr) + MCLAG_MSG_HDR_LEN;

        switch (hdr->msg_type)
        {
            case MCLAG_MSG_TYPE_PORT_ISOLATE:
                setPortIsolate(msg);
                break;
            case MCLAG_MSG_TYPE_PORT_MAC_LEARN_MODE:
                setPortLearnMode(msg);
                break;
            case MCLAG_MSG_TYPE_FLUSH_FDB:
                flushFdb();
                break;
            case MCLAG_MSG_TYPE_FLUSH_FDB_BY_PORT:
                flushFdbByPort(msg);
                break;
            case MCLAG_MSG_TYPE_SET_INTF_MAC:
                setIntfMac(msg);
                break;
            case MCLAG_MSG_TYPE_SET_FDB:
                setFdbEntry(msg, (int)(hdr->msg_len - sizeof(mclag_msg_hdr_t)));
                break;
            default:
                break;
        }

        start += msg_len;
    }

    memmove(m_msgBuf, m_msgBuf + start, m_pos - start);
    m_pos = m_pos - (uint32_t)start;

    return 0;
}

MclagServerLink::MclagServerLink(int port) :
    m_serverUp(false)
{
    struct sockaddr_in addr;
    int true_val = 1;

    m_serverSocket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_serverSocket < 0)
        throw system_error(errno, system_category());

    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &true_val, sizeof(true_val)) < 0)
    {
        close(m_serverSocket);
        throw system_error(errno, system_category());
    }

    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_KEEPALIVE, &true_val, sizeof(true_val)) < 0)
    {
        close(m_serverSocket);
        throw system_error(errno, system_category());
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(MCLAG_DEFAULT_IP);

    if (bind(m_serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(m_serverSocket);
        throw system_error(errno, system_category());
    }

    if (listen(m_serverSocket, 2) != 0)
    {
        close(m_serverSocket);
        throw system_error(errno, system_category());
    }

    m_serverUp = true;
}

MclagServerLink::~MclagServerLink()
{
    vector<MclagLink *>::iterator it;

    for (it = m_linkList.begin(); it != m_linkList.end();)
    {
        MclagLink *link = (*it);
        delete link;
        it++;
    }

    if (m_serverUp)
        close(m_serverSocket);
}

int MclagServerLink::getFd()
{
    return m_serverSocket;
}

void MclagServerLink::accept()
{
    struct sockaddr_in client_addr;
    socklen_t client_len;
    int connect_socket;
    MclagLink *link;

    connect_socket =::accept(m_serverSocket, (struct sockaddr *)&client_addr, &client_len);
    if (connect_socket < 0)
        throw system_error(errno, system_category());

    SWSS_LOG_NOTICE("New connection(fd:%d) accepted from: %s", connect_socket, inet_ntoa(client_addr.sin_addr));

    link = new MclagLink(connect_socket);
    m_linkList.push_back(link);

    m_pSelect->addSelectable(link);
    m_pSelect->addSelectables(link->getFdbGatherSelectables());

    return;
}

uint64_t MclagServerLink::readData()
{
    accept();

    return 0;
}

MclagFdbGather::MclagFdbGather(DBConnector *statDb, const vector<TableConnector> &tables) :
    Orch(tables),
    m_redisClient(statDb)
{
}

void MclagFdbGather::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    SWSS_LOG_NOTICE("Get task from table %s", table.c_str());

    if (table == STATE_FDB_TABLE_NAME)
    {
        storeFdbChange(consumer);
    }
}

void MclagFdbGather::storeFdbChange(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        string mac;
        string vlan_name;
        unsigned int vlan_id;
        string port_name;
        string type;
        short op_type = MCLAG_FDB_OPER_NONE;
        struct mclag_fdb fdb;
        int exist = 0;
        set<mclag_fdb>::iterator fdb_it;

        KeyOpFieldsValuesTuple t = it->second;

        /* format: <VLAN_name>:<MAC_address> */
        vector<string> keys = tokenize(kfvKey(t), ':', 1);
        string op = kfvOp(t);

        vlan_name = keys[0];
        mac = keys[1];

        sscanf(vlan_name.c_str(), "Vlan%d", &vlan_id);

        fdb.mac = mac;
        fdb.vid = vlan_id;
        fdb.op_type = MCLAG_FDB_OPER_NONE;

        if ((fdb_it = find(m_fdbSet.begin(), m_fdbSet.end(), fdb)) == m_fdbSet.end())
            exist = 0;
        else
            exist = 1;

        if (op == SET_COMMAND)
        {
            op_type = MCLAG_FDB_OPER_ADD;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "port")
                {
                    port_name = fvValue(i);
                }

                if (fvField(i) == "type")
                {
                    /* dynamic or static */
                    type = fvValue(i);
                }
            }

            SWSS_LOG_NOTICE("Rcv msg from STATE_FDB_DB to add fdb(mac:%s, vid:%d, port_name:%s, type:%s).",
                           mac.c_str(), vlan_id, port_name.c_str(), type.c_str());

            fdb.port_name = port_name;
            fdb.type = type;

            if (exist == 1)
            {
                if (fdb_it->port_name == port_name && fdb_it->type == type)
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                else
                {
                    m_fdbSet.erase(fdb_it);
                    m_fdbSet.insert(fdb);
                }
            }
            else
            {
                m_fdbSet.insert(fdb);
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Rcv msg from STATE_FDB_DB to del fdb(mac:%s, vid:%d)", mac.c_str(), vlan_id);
            if (exist)
            {
                op_type = MCLAG_FDB_OPER_DEL;
                port_name = "";
                type = "";

                m_fdbSet.erase(fdb_it);
            }
            else
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        m_fdbEvent.push_back(mclag_fdb(mac, vlan_id, port_name, type, op_type));
        it = consumer.m_toSync.erase(it);
    }

    return;
}

void MclagFdbGather::getFdbFromStatedb()
{
    string port_name;
    string mac;
    string type;
    string vlanid;
    int vid;
    size_t pos1 = 0;
    size_t pos2 = 0;
    std::unordered_map<string, string >::iterator type_it;

    auto keys = m_redisClient.keys("FDB_TABLE|*");

    for (auto& key : keys)
    {
        /* get vid */
        pos1 = key.find("Vlan", 0);
        if (pos1 != key.npos)
        {
            pos1 = pos1 + 4;
            pos2 = key.find(":", pos1);
            vlanid = key.substr(pos1, pos2 - pos1);
        }
        else
            continue;

        vid = atoi(vlanid.c_str());

        /* get mac */
        pos1 = key.find_first_of(":") + 1;
        pos2 = key.find_last_of(":") + 3;
        mac = key.substr(pos1, pos2 - pos1);

        /* get port && type */
        auto hash = m_redisClient.hgetall(key);
        type_it = hash.find("port");
        if (type_it == hash.end())
        {
            continue;
        }
        port_name = type_it->second;

        type_it = hash.find("type");
        if (type_it == hash.end())
        {
            continue;
        }
        type = type_it->second;

        SWSS_LOG_NOTICE("Read fdb entry(mac:%s, vid:%d, port_name:%s, type:%s) from STATE_FDB_TABLE and record it locally.",
                       mac.c_str(), vid, port_name.c_str(), type.c_str());

        m_fdbEvent.push_back(mclag_fdb(mac, vid, port_name, type, MCLAG_FDB_OPER_ADD));
        m_fdbSet.insert(mclag_fdb(mac, vid, port_name, type, MCLAG_FDB_OPER_NONE));
    }

    return;
}
