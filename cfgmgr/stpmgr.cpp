#include "exec.h"
#include "stpmgr.h"
#include "logger.h"
#include "tokenize.h"
#include "warm_restart.h"

#include <iostream>
#include <algorithm>
#include <sstream>
#include <thread>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

using namespace std;
using namespace swss;

StpMgr::StpMgr(DBConnector *confDb, DBConnector *applDb, DBConnector *statDb,
        const vector<TableConnector> &tables) :
    Orch(tables),
    m_cfgStpGlobalTable(confDb, CFG_STP_GLOBAL_TABLE_NAME),
    m_cfgStpVlanTable(confDb, CFG_STP_VLAN_TABLE_NAME),
    m_cfgStpVlanPortTable(confDb, CFG_STP_VLAN_PORT_TABLE_NAME),
    m_cfgStpPortTable(confDb, CFG_STP_PORT_TABLE_NAME),
    m_cfgLagMemberTable(confDb, CFG_LAG_MEMBER_TABLE_NAME),
    m_cfgVlanMemberTable(confDb, CFG_VLAN_MEMBER_TABLE_NAME),
    m_stateVlanTable(statDb, STATE_VLAN_TABLE_NAME),
    m_stateLagTable(statDb, STATE_LAG_TABLE_NAME),
    m_stateStpTable(statDb, STATE_STP_TABLE_NAME),
    m_stateVlanMemberTable(statDb, STATE_VLAN_MEMBER_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    l2ProtoEnabled = L2_NONE;

    stpGlobalTask = stpVlanTask = stpVlanPortTask = stpPortTask = false;

    // Initialize all VLANs to Invalid instance
    fill_n(m_vlanInstMap, MAX_VLANS, INVALID_INSTANCE);

    int ret = system("ebtables -D FORWARD -d 01:00:0c:cc:cc:cd -j DROP");
    SWSS_LOG_DEBUG("ebtables ret %d", ret); 
}

void StpMgr::doTask(Consumer &consumer)
{
    auto table = consumer.getTableName();

    SWSS_LOG_INFO("Get task from table %s", table.c_str());

    if (table == CFG_STP_GLOBAL_TABLE_NAME)
        doStpGlobalTask(consumer);
    else if (table == CFG_STP_VLAN_TABLE_NAME)
        doStpVlanTask(consumer);
    else if (table == CFG_STP_VLAN_PORT_TABLE_NAME)
        doStpVlanPortTask(consumer);
    else if (table == CFG_STP_PORT_TABLE_NAME)
        doStpPortTask(consumer);
    else if (table == CFG_LAG_MEMBER_TABLE_NAME)
        doLagMemUpdateTask(consumer);
    else if (table == STATE_VLAN_MEMBER_TABLE_NAME)
        doVlanMemUpdateTask(consumer);
    else
        SWSS_LOG_ERROR("Invalid table %s", table.c_str());
}

void StpMgr::doStpGlobalTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (stpGlobalTask == false)
        stpGlobalTask = true;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        STP_BRIDGE_CONFIG_MSG msg;
        memset(&msg, 0, sizeof(STP_BRIDGE_CONFIG_MSG));

        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("STP global key %s op %s", key.c_str(), op.c_str());
        if (op == SET_COMMAND)
        {
            msg.opcode = STP_SET_COMMAND;
            for (auto i : kfvFieldsValues(t))
            {
                SWSS_LOG_DEBUG("Field: %s Val %s", fvField(i).c_str(), fvValue(i).c_str());
                if (fvField(i) == "mode")
                {
                    if (fvValue(i) == "pvst")
                    {
                        if (l2ProtoEnabled == L2_NONE)
                        {
                            const std::string cmd = std::string("") + 
                                " ebtables -A FORWARD -d 01:00:0c:cc:cc:cd -j DROP";
                            std::string res;
                            int ret = swss::exec(cmd, res);
                            if (ret != 0)
                                SWSS_LOG_ERROR("ebtables add failed %d", ret);

                            l2ProtoEnabled = L2_PVSTP;
                        }
                        msg.stp_mode = L2_PVSTP;
                    }
                    else
                        SWSS_LOG_ERROR("Error invalid mode %s", fvValue(i).c_str());
                }
                else if (fvField(i) == "rootguard_timeout")
                {
                    msg.rootguard_timeout = stoi(fvValue(i).c_str());
                }
            }

            memcpy(msg.base_mac_addr, macAddress.getMac(), 6);
        }
        else if (op == DEL_COMMAND)
        {
            msg.opcode = STP_DEL_COMMAND;
            l2ProtoEnabled = L2_NONE;

            //Free Up all instances
            FREE_ALL_INST_ID();
    
            // Initialize all VLANs to Invalid instance
            fill_n(m_vlanInstMap, MAX_VLANS, INVALID_INSTANCE);

            const std::string cmd = std::string("") + 
                    " ebtables -D FORWARD -d 01:00:0c:cc:cc:cd -j DROP";
            std::string res;
            int ret = swss::exec(cmd, res);
            if (ret != 0)
                SWSS_LOG_ERROR("ebtables del failed %d", ret);
        }

        sendMsgStpd(STP_BRIDGE_CONFIG, sizeof(msg), (void *)&msg);

        it = consumer.m_toSync.erase(it);
    }
}

void StpMgr::doStpVlanTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (stpGlobalTask == false || (stpPortTask == false && !isStpPortEmpty()))
        return;

    if (stpVlanTask == false)
        stpVlanTask = true;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        STP_VLAN_CONFIG_MSG *msg = NULL;
        uint32_t len = 0;
        bool stpEnable = false;
        uint8_t newInstance = 0;
        int instId, forwardDelay, helloTime, maxAge, priority, portCnt = 0;
        instId = forwardDelay = helloTime = maxAge = priority = portCnt = 0;

        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        
        string vlanKey = key.substr(4); // Remove Vlan prefix
        int vlan_id = stoi(vlanKey.c_str());

        SWSS_LOG_INFO("STP vlan key %s op %s", key.c_str(), op.c_str());
        if (op == SET_COMMAND)
        {
            if (l2ProtoEnabled == L2_NONE || !isVlanStateOk(key))
            {
                // Wait till STP is configured
                it++;
                continue;
            }

            for (auto i : kfvFieldsValues(t))
            {
                SWSS_LOG_DEBUG("Field: %s Val: %s", fvField(i).c_str(), fvValue(i).c_str());

                if (fvField(i) == "enabled")
                {
                    stpEnable = (fvValue(i) == "true") ? true : false;
                }
                else if (fvField(i) == "forward_delay")
                {
                    forwardDelay = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "hello_time")
                {
                    helloTime = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "max_age")
                {
                    maxAge = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "priority")
                {
                    priority = stoi(fvValue(i).c_str());
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            stpEnable = false;
            if (l2ProtoEnabled == L2_NONE)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }

        len = sizeof(STP_VLAN_CONFIG_MSG); 
        if (stpEnable == true)
        {
            vector<PORT_ATTR> port_list;
            if (m_vlanInstMap[vlan_id] == INVALID_INSTANCE)
            {
                /* VLAN is being added to the instance. Get all members for VLAN Mapping*/
                if (l2ProtoEnabled == L2_PVSTP)
                {
                    newInstance = 1;
                    instId  = allocL2Instance(vlan_id);
                    if (instId == -1)
                    {
                        SWSS_LOG_ERROR("Couldnt allocate instance to VLAN %d", vlan_id);
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    portCnt = getAllVlanMem(key, port_list);
                    SWSS_LOG_DEBUG("Port count %d", portCnt);
                }
            
                len += (uint32_t)(portCnt * sizeof(PORT_ATTR));
            }

            msg = (STP_VLAN_CONFIG_MSG *)calloc(1, len); 
            if (!msg)
            {
                SWSS_LOG_ERROR("mem failed for vlan %d", vlan_id);
                return;
            }
            
            msg->opcode      = STP_SET_COMMAND;
            msg->vlan_id     = vlan_id;
            msg->newInstance = newInstance;
            msg->inst_id     = m_vlanInstMap[vlan_id];
            msg->forward_delay = forwardDelay;
            msg->hello_time  = helloTime;
            msg->max_age     = maxAge;
            msg->priority    = priority;
            msg->count       = portCnt;

            if(msg->count)
            {
                int i = 0;
                PORT_ATTR *attr = msg->port_list;
                for (auto p = port_list.begin(); p != port_list.end(); p++)
                {
                    attr[i].mode    = p->mode;
                    attr[i].enabled = p->enabled;
                    strncpy(attr[i].intf_name, p->intf_name, IFNAMSIZ);
                    SWSS_LOG_DEBUG("MemIntf: %s", p->intf_name);
                    i++;
                }
            }
        }
        else
        {
            if (m_vlanInstMap[vlan_id] == INVALID_INSTANCE)
            {
                // Already deallocated. NoOp. This can happen when STP 
                // is disabled on a VLAN more than once
                it = consumer.m_toSync.erase(it);
                continue;
            }

            msg = (STP_VLAN_CONFIG_MSG *)calloc(1, len); 
            if (!msg)
            {
                SWSS_LOG_ERROR("mem failed for vlan %d", vlan_id);
                return;
            }

            msg->opcode = STP_DEL_COMMAND;
            msg->inst_id = m_vlanInstMap[vlan_id];

            deallocL2Instance(vlan_id);
        }
        
        sendMsgStpd(STP_VLAN_CONFIG, len, (void *)msg);
        if (msg)
            free(msg);

        it = consumer.m_toSync.erase(it);
    }
}

void StpMgr::processStpVlanPortAttr(const string op, uint32_t vlan_id, const string intfName,
        vector<FieldValueTuple>&tupEntry)
{
    STP_VLAN_PORT_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(STP_VLAN_PORT_CONFIG_MSG));

    msg.vlan_id = vlan_id;
    msg.inst_id = m_vlanInstMap[vlan_id];
    strncpy(msg.intf_name, intfName.c_str(), IFNAMSIZ-1);

    if (op == SET_COMMAND)
    {
        msg.opcode = STP_SET_COMMAND;
        msg.priority = -1;

        for (auto i : tupEntry)
        {
            SWSS_LOG_DEBUG("Field: %s Val: %s", fvField(i).c_str(), fvValue(i).c_str());
            if (fvField(i) == "path_cost")
            {
                msg.path_cost = stoi(fvValue(i).c_str());
            }
            else if (fvField(i) == "priority")
            {
                msg.priority = stoi(fvValue(i).c_str());
            }
        }
    }
    else if (op == DEL_COMMAND)
    {
        msg.opcode = STP_DEL_COMMAND;
    }

    sendMsgStpd(STP_VLAN_PORT_CONFIG, sizeof(msg), (void *)&msg);
}

void StpMgr::doStpVlanPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (stpGlobalTask == false || stpVlanTask == false || stpPortTask == false)
        return;

    if (stpVlanPortTask == false)
        stpVlanPortTask = true;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        STP_VLAN_PORT_CONFIG_MSG msg;
        memset(&msg, 0, sizeof(STP_VLAN_PORT_CONFIG_MSG));

        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        string vlanKey = key.substr(4); // Remove VLAN keyword
        size_t found = vlanKey.find(CONFIGDB_KEY_SEPARATOR);

        int vlan_id;
        string intfName;
        if (found != string::npos)
        {
            vlan_id = stoi(vlanKey.substr(0, found));
            intfName = vlanKey.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format %s", kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        SWSS_LOG_INFO("STP vlan intf key:%s op:%s", key.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            if ((l2ProtoEnabled == L2_NONE) || (m_vlanInstMap[vlan_id] == INVALID_INSTANCE))
            {
                // Wait till STP/VLAN is configured
                it++;
                continue;
            }
        }
        else
        {
            if (l2ProtoEnabled == L2_NONE || (m_vlanInstMap[vlan_id] == INVALID_INSTANCE))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }

        if (isLagEmpty(intfName))
        {
            // Lag has no member. Process when first member is added/deleted
            it = consumer.m_toSync.erase(it);
            continue;
        }

        processStpVlanPortAttr(op, vlan_id, intfName, kfvFieldsValues(t));

        it = consumer.m_toSync.erase(it);
    }
}

void StpMgr::processStpPortAttr(const string op, vector<FieldValueTuple>&tupEntry, const string intfName)
{
    STP_PORT_CONFIG_MSG *msg;
    uint32_t len = 0;
    int vlanCnt = 0;
    vector<VLAN_ATTR> vlan_list;

    if (op == SET_COMMAND)
        vlanCnt = getAllPortVlan(intfName, vlan_list);

    len = (uint32_t)(sizeof(STP_PORT_CONFIG_MSG) + (vlanCnt * sizeof(VLAN_ATTR)));
    msg = (STP_PORT_CONFIG_MSG *)calloc(1, len);
    if (!msg)
    {
        SWSS_LOG_ERROR("mem failed for %s", intfName.c_str());
        return;
    }

    strncpy(msg->intf_name, intfName.c_str(), IFNAMSIZ-1);
    msg->count = vlanCnt; 
    SWSS_LOG_INFO("Vlan count %d", vlanCnt);

    if(msg->count)
    {
        int i = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
        VLAN_ATTR *attr = msg->vlan_list;
#pragma GCC diagnostic pop
        for (auto p = vlan_list.begin(); p != vlan_list.end(); p++)
        {
            attr[i].inst_id = p->inst_id;
            attr[i].mode    = p->mode;
            SWSS_LOG_DEBUG("Inst:%d Mode:%d", p->inst_id, p->mode);
            i++;
        }
    }

    if (op == SET_COMMAND)
    {
        msg->opcode = STP_SET_COMMAND;
        msg->priority = -1;

        for (auto i : tupEntry)
        {
            SWSS_LOG_DEBUG("Field: %s Val: %s", fvField(i).c_str(), fvValue(i).c_str());
            if (fvField(i) == "enabled")
            {
                msg->enabled = (fvValue(i) == "true") ? 1 : 0;
            }
            else if (fvField(i) == "root_guard")
            {
                msg->root_guard = (fvValue(i) == "true") ? 1 : 0;
            }
            else if (fvField(i) == "bpdu_guard")
            {
                msg->bpdu_guard = (fvValue(i) == "true") ? 1 : 0;
            }
            else if (fvField(i) == "bpdu_guard_do_disable")
            {
                msg->bpdu_guard_do_disable = (fvValue(i) == "true") ? 1 : 0;
            }
            else if (fvField(i) == "path_cost")
            {
                msg->path_cost = stoi(fvValue(i).c_str());
            }
            else if (fvField(i) == "priority")
            {
                msg->priority = stoi(fvValue(i).c_str());
            }
            else if (fvField(i) == "portfast")
            {
                msg->portfast = (fvValue(i) == "true") ? 1 : 0;
            }
            else if (fvField(i) == "uplink_fast")
            {
                msg->uplink_fast = (fvValue(i) == "true") ? 1 : 0;
            }
        }
    }
    else if (op == DEL_COMMAND)
    {
        msg->opcode = STP_DEL_COMMAND;
        msg->enabled = 0;
    }

    sendMsgStpd(STP_PORT_CONFIG, len, (void *)msg);
    if (msg)
        free(msg);
}

void StpMgr::doStpPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (stpGlobalTask == false)
        return;

    if (stpPortTask == false)
        stpPortTask = true;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (isLagEmpty(key))
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            if (l2ProtoEnabled == L2_NONE)
            {
                // Wait till STP is configured
                it++;
                continue;
            }
        }
        else
        {
            if (l2ProtoEnabled == L2_NONE)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }

        SWSS_LOG_INFO("STP port key:%s op:%s", key.c_str(), op.c_str());
        processStpPortAttr(op, kfvFieldsValues(t), key);

        it = consumer.m_toSync.erase(it);
    }
}

void StpMgr::doVlanMemUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        STP_VLAN_MEM_CONFIG_MSG msg;
        memset(&msg, 0, sizeof(STP_VLAN_MEM_CONFIG_MSG));

        KeyOpFieldsValuesTuple t = it->second;

        auto key = kfvKey(t);
        auto op = kfvOp(t);

        string vlanKey = key.substr(4); // Remove Vlan prefix
        size_t found = vlanKey.find(CONFIGDB_KEY_SEPARATOR);

        int vlan_id;
        string intfName;
        if (found != string::npos)
        {
            vlan_id = stoi(vlanKey.substr(0, found));
            intfName = vlanKey.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format. No member port is presented: %s", kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
            
        SWSS_LOG_INFO("STP vlan mem key:%s op:%s inst:%d", key.c_str(), op.c_str(), m_vlanInstMap[vlan_id]);
        // If STP is running on this VLAN, notify STPd
        if (m_vlanInstMap[vlan_id] != INVALID_INSTANCE && !isLagEmpty(intfName))
        {
            int8_t tagging_mode = TAGGED_MODE;

            if (op == SET_COMMAND)
            {
                tagging_mode = getVlanMemMode(key);
                if (tagging_mode == INVALID_MODE)
                {
                    SWSS_LOG_ERROR("invalid mode %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                SWSS_LOG_DEBUG("mode %d key %s", tagging_mode, key.c_str());

                msg.enabled = isStpEnabled(intfName);

                vector<FieldValueTuple> stpVlanPortEntry;
                if (m_cfgStpVlanPortTable.get(key, stpVlanPortEntry))
                {
                    for (auto entry : stpVlanPortEntry)
                    {
                        if (entry.first == "priority")
                            msg.priority = stoi(entry.second);
                        else if (entry.first == "path_cost")
                            msg.path_cost = stoi(entry.second);
                    }
                }
            }

            msg.opcode  = (op == SET_COMMAND) ? STP_SET_COMMAND : STP_DEL_COMMAND;
            msg.vlan_id = vlan_id;
            msg.inst_id = m_vlanInstMap[vlan_id];
            msg.mode    = tagging_mode;
            msg.priority  = -1;
            msg.path_cost = 0;

            strncpy(msg.intf_name, intfName.c_str(), IFNAMSIZ-1);

            sendMsgStpd(STP_VLAN_MEM_CONFIG, sizeof(msg), (void *)&msg);
        }

        it = consumer.m_toSync.erase(it);
    }
}

void StpMgr::doLagMemUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        bool notifyStpd = false;

        auto key = kfvKey(t);
        auto op = kfvOp(t);

        string po_name;
        string po_mem;
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR);

        if (found != string::npos)
        {
            po_name = key.substr(0, found);
            po_mem  = key.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            if (!isLagStateOk(po_name))
            {
                it++;
                continue;
            }

            auto elm = m_lagMap.find(po_name);
            if (elm == m_lagMap.end())
            {
                // First Member added to the LAG 
                m_lagMap[po_name] = 1;
                notifyStpd = true;
            }
            else
            {
                elm->second++;
            }
        }
        else if (op == DEL_COMMAND)
        {
            auto elm = m_lagMap.find(po_name);
            if (elm != m_lagMap.end())
            {
                elm->second--;

                if (elm->second == 0)
                {
                    // Last Member deleted from the LAG 
                    m_lagMap.erase(po_name);
                    //notifyStpd = true;
                }
            }
            else
                SWSS_LOG_ERROR("PO not found %s", po_name.c_str());
        }

        if (notifyStpd && l2ProtoEnabled != L2_NONE)
        {
            vector<VLAN_ATTR> vlan_list;
            vector<FieldValueTuple> tupEntry;

            if (m_cfgStpPortTable.get(po_name, tupEntry))
            {
                //Push STP_PORT configs for this port
                processStpPortAttr(op, tupEntry, po_name);
                            
                getAllPortVlan(po_name, vlan_list);
                //Push STP_VLAN_PORT configs for this port
                for (auto p = vlan_list.begin(); p != vlan_list.end(); p++)
                {
                    vector<FieldValueTuple> vlanPortTup;

                    string vlanPortKey = "Vlan" + to_string(p->vlan_id) + "|" + po_name;
                    if (m_cfgStpVlanPortTable.get(vlanPortKey, vlanPortTup))
                        processStpVlanPortAttr(op, p->vlan_id, po_name, vlanPortTup);
                }
            }
        }

        SWSS_LOG_DEBUG("LagMap");
        for (auto itr = m_lagMap.begin(); itr != m_lagMap.end(); ++itr) { 
            SWSS_LOG_DEBUG("PO: %s Cnt:%d", itr->first.c_str(), itr->second);
        } 

        it = consumer.m_toSync.erase(it);
    }
}

void StpMgr::ipcInitStpd()
{
    int ret;
	struct sockaddr_un addr;

    unlink(STPMGRD_SOCK_NAME);
    // create socket
    stpd_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (!stpd_fd) {
		SWSS_LOG_ERROR("socket error %s", strerror(errno));
		return;
    }

    // setup socket address structure
    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, STPMGRD_SOCK_NAME, sizeof(addr.sun_path)-1);

    ret = (int)bind(stpd_fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un));
    if (ret == -1)
    {
		SWSS_LOG_ERROR("ipc bind error %s", strerror(errno));
        close(stpd_fd);
        return;
    }
}

int StpMgr::allocL2Instance(uint32_t vlan_id)
{
    int idx = 0;

    if (!IS_INST_ID_AVAILABLE())
    {
        SWSS_LOG_ERROR("No instance available");
        return -1;
    }

    if (l2ProtoEnabled == L2_PVSTP)
    {
        GET_FIRST_FREE_INST_ID(idx);
    }
    else
    {
        SWSS_LOG_ERROR("invalid proto %d for vlan %d", l2ProtoEnabled, vlan_id);
        return -1;
    }

    //Set VLAN to Instance mapping
    m_vlanInstMap[vlan_id] = idx;
    SWSS_LOG_INFO("Allocated Id: %d Vlan %d", m_vlanInstMap[vlan_id], vlan_id);

    return idx;
}

void StpMgr::deallocL2Instance(uint32_t vlan_id)
{
    int idx = 0;

    if (l2ProtoEnabled == L2_PVSTP)
    {
        idx = m_vlanInstMap[vlan_id];
        FREE_INST_ID(idx);
    }
    else
    {
        SWSS_LOG_ERROR("invalid proto %d for vlan %d", l2ProtoEnabled, vlan_id);
    }

    m_vlanInstMap[vlan_id] = INVALID_INSTANCE;
    SWSS_LOG_INFO("Deallocated Id: %d Vlan %d", m_vlanInstMap[vlan_id], vlan_id);
}


int StpMgr::getAllVlanMem(const string &vlanKey, vector<PORT_ATTR>&port_list)
{
    PORT_ATTR port_id;
    vector<FieldValueTuple> vmEntry;

    vector<string> vmKeys;
    m_stateVlanMemberTable.getKeys(vmKeys);

    SWSS_LOG_INFO("VLAN Key: %s", vlanKey.c_str());
    for (auto key : vmKeys)
    {
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR); //split VLAN and interface

        string vlanName;
        string intfName;
        if (found != string::npos)
        {
            vlanName = key.substr(0, found);
            intfName = key.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid Key: %s", key.c_str());
            continue;
        }

        if (vlanKey == vlanName && !isLagEmpty(intfName))
        {
            port_id.mode = getVlanMemMode(key);
            if (port_id.mode == INVALID_MODE)
            {
                SWSS_LOG_ERROR("invalid mode %s", key.c_str());
                continue;
            }

            port_id.enabled = isStpEnabled(intfName);
            strncpy(port_id.intf_name, intfName.c_str(), IFNAMSIZ-1);
            port_list.push_back(port_id);
            SWSS_LOG_DEBUG("MemIntf: %s", intfName.c_str());
        }
    }

    return (int)port_list.size();
}

int StpMgr::getAllPortVlan(const string &intfKey, vector<VLAN_ATTR>&vlan_list)
{
    VLAN_ATTR vlan;
    vector<FieldValueTuple> vmEntry;

    vector<string> vmKeys;
    m_stateVlanMemberTable.getKeys(vmKeys);

    SWSS_LOG_INFO("Intf Key: %s", intfKey.c_str());
    for (auto key : vmKeys)
    {
        string vlanKey = key.substr(4); // Remove Vlan prefix
        size_t found = vlanKey.find(CONFIGDB_KEY_SEPARATOR); //split VLAN and interface
        SWSS_LOG_DEBUG("Vlan mem Key: %s", key.c_str());

        int vlan_id;
        string intfName;
        if (found != string::npos)
        {
            vlan_id = stoi(vlanKey.substr(0, found));
            intfName = vlanKey.substr(found+1);

            if (intfName == intfKey)
            {
                if (m_vlanInstMap[vlan_id] != INVALID_INSTANCE)
                {
                    vlan.mode = getVlanMemMode(key);
                    if (vlan.mode == INVALID_MODE)
                    {
                        SWSS_LOG_ERROR("invalid mode %s", key.c_str());
                        continue;
                    }
            
                    vlan.vlan_id = vlan_id;
                    vlan.inst_id = m_vlanInstMap[vlan_id];
                    vlan_list.push_back(vlan);
                    SWSS_LOG_DEBUG("Matched vlan key: %s intf key %s", intfName.c_str(), intfKey.c_str());
                }
            }
        }
    }

    return (int)vlan_list.size();
}

// Send Message to STPd
int StpMgr::sendMsgStpd(STP_MSG_TYPE msgType, uint32_t msgLen, void *data)
{
    STP_IPC_MSG *tx_msg;
    size_t len = 0;
    struct sockaddr_un addr;
    int rc;

    len = msgLen + (offsetof(struct STP_IPC_MSG, data));
    SWSS_LOG_INFO("tx_msg len %d msglen %d", (int)len, msgLen);

    tx_msg = (STP_IPC_MSG *)calloc(1, len);
    if (tx_msg == NULL)
    {
		SWSS_LOG_ERROR("tx_msg mem alloc error\n");
        return -1;
    }

    tx_msg->msg_type = msgType;
    tx_msg->msg_len  = msgLen;
    memcpy(tx_msg->data, data, msgLen);

    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, STPD_SOCK_NAME, sizeof(addr.sun_path)-1);

    rc = (int)sendto(stpd_fd, (void*)tx_msg, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == -1)
    {
		SWSS_LOG_ERROR("tx_msg send error\n");
    }   
    else
    {
        SWSS_LOG_INFO("tx_msg sent %d", rc);
    }

    free(tx_msg);
    return rc;
}

bool StpMgr::isPortInitDone(DBConnector *app_db)
{
    bool portInit = 0;
    long cnt = 0;

    while(!portInit) {
        Table portTable(app_db, APP_PORT_TABLE_NAME);
        std::vector<FieldValueTuple> tuples;
        portInit = portTable.get("PortInitDone", tuples);

        if(portInit)
            break;
        sleep(1);
        cnt++;
    }
    SWSS_LOG_NOTICE("PORT_INIT_DONE : %d %ld", portInit, cnt);
    return portInit;
}

bool StpMgr::isVlanStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("%s is ready", alias.c_str());
            return true;
        }
    }
    SWSS_LOG_DEBUG("%s is not ready", alias.c_str());
    return false;
}

bool StpMgr::isLagStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (m_stateLagTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("%s is ready", alias.c_str());
        return true;
    }

    SWSS_LOG_DEBUG("%s is not ready", alias.c_str());
    return false;
}

bool StpMgr::isLagEmpty(const string &key)
{
    size_t po_find = key.find("PortChannel"); 
    if (po_find != string::npos)
    {
        // If Lag, check if members present
        auto elm = m_lagMap.find(key);
        if (elm == m_lagMap.end())
        {
            // Lag has no member
            SWSS_LOG_DEBUG("%s empty", key.c_str());
            return true;
        }
        SWSS_LOG_DEBUG("%s not empty", key.c_str());
    }
    // Else: Interface not PO

    return false;
}

bool StpMgr::isStpPortEmpty()
{
    vector<string> portKeys;
    m_cfgStpPortTable.getKeys(portKeys);

    if (portKeys.empty())
    {
        SWSS_LOG_NOTICE("stp port empty");
        return true;
    }

    SWSS_LOG_NOTICE("stp port not empty");
    return false;
}

bool StpMgr::isStpEnabled(const string &intf_name)
{
    vector<FieldValueTuple> temp;

    if (m_cfgStpPortTable.get(intf_name, temp))
    {
        for (auto entry : temp)
        {
            if (entry.first == "enabled" && entry.second == "true")
            {
                SWSS_LOG_NOTICE("STP enabled on %s", intf_name.c_str());
                return true;
            }
        }
    }
    
    SWSS_LOG_NOTICE("STP NOT enabled on %s", intf_name.c_str());
    return false;
}

int8_t StpMgr::getVlanMemMode(const string &key)
{
    int8_t mode = -1;
    vector<FieldValueTuple> vmEntry;

    if (m_cfgVlanMemberTable.get(key, vmEntry))
    {
        for (auto entry : vmEntry)
        {
            if (entry.first == "tagging_mode")
                mode = (entry.second == "untagged") ? UNTAGGED_MODE : TAGGED_MODE;
            SWSS_LOG_INFO("mode %d for %s", mode, key.c_str());
        }
    }
    else
        SWSS_LOG_ERROR("config vlan_member table fetch failed %s", key.c_str());

    return mode;
}

uint16_t StpMgr::getStpMaxInstances(void)
{
    vector<FieldValueTuple> vmEntry;
    uint16_t max_delay = 60;
    string key;

    key = "GLOBAL";

    while(max_delay)
    {
        if (m_stateStpTable.get(key, vmEntry))
        {
            for (auto entry : vmEntry)
            {
                if (entry.first == "max_stp_inst")
                {
                    max_stp_instances = (uint16_t)stoi(entry.second.c_str());
                    SWSS_LOG_NOTICE("max stp instance %d count %d", max_stp_instances, (60-max_delay));
                }
            }
            break;
        }
        sleep(1);
        max_delay--;
    }

    if(max_stp_instances == 0)
    {
        max_stp_instances = STP_DEFAULT_MAX_INSTANCES;
        SWSS_LOG_NOTICE("set default max stp instance %d", max_stp_instances);
    }

    return max_stp_instances;
}
