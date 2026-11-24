#include "exec.h"
#include "stpmgr.h"
#include "logger.h"
#include "tokenize.h"
#include "warm_restart.h"
#include <vector>
#include <string>
#include <cstdlib>
#include <cctype>
#include <stdexcept>

#include <iostream>
#include <algorithm>
#include <sstream>
#include <thread>
#include <cerrno>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

using namespace std;
using namespace swss;

static bool parseInteger(const std::string &value, int &result, const char *field)
{
    if (value.empty() ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }))
    {
        SWSS_LOG_ERROR("Invalid integer for %s: '%s'", field, value.c_str());
        return false;
    }

    try
    {
        size_t parsed = 0;
        result = std::stoi(value, &parsed);
        if (parsed != value.size())
        {
            SWSS_LOG_ERROR("Invalid integer for %s: '%s'", field, value.c_str());
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Invalid integer for %s: '%s' (%s)", field, value.c_str(), e.what());
        return false;
    }
}

// Each handler wraps its entry in try/catch and drops the entry on any
// exception, so a bad value is thrown with its field name instead of being
// returned.
static int stpParseInt(const std::string &value, const char *what)
{
    int result;
    if (!parseInteger(value, result, what))
        throw std::invalid_argument(
            std::string(what) + " has non-integer value '" + value + "'");
    return result;
}

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
    m_stateVlanMemberTable(statDb, STATE_VLAN_MEMBER_TABLE_NAME),
    m_cfgMstGlobalTable(confDb, "STP_MST"),
    m_cfgMstInstTable(confDb, "STP_MST_INST"),
    m_cfgMstInstPortTable(confDb, "STP_MST_PORT"),
    m_stateStpVlanTable(statDb, STATE_STP_VLAN_TABLE_NAME),
    m_stateStpMstInstTable(statDb, STATE_STP_MST_INST_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    l2ProtoEnabled = L2_NONE;
    stpd_fd = -1;

    stpGlobalTask = stpVlanTask = stpVlanPortTask = stpPortTask = stpMstInstTask = false;
    m_globalPriority = m_globalForwardDelay = m_globalHelloTime = m_globalMaxAge = -1;
    m_sendFailed = false;

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
    else if (table == "STP_MST")
        doStpMstGlobalTask(consumer);
    else if (table == "STP_MST_INST")
        doStpMstInstTask(consumer);
    else if (table == "STP_MST_PORT")
        doStpMstInstPortTask(consumer);
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
        try
        {
        STP_BRIDGE_CONFIG_MSG msg;
        memset(&msg, 0, sizeof(STP_BRIDGE_CONFIG_MSG));

        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("STP global key %s op %s", key.c_str(), op.c_str());
        if (op == DEL_COMMAND)
        {
            disableProtocol();
            it = consumer.m_toSync.erase(it);
            continue;
        }

        bool mstEnabled = false;
        bool modeChanged = false;
        L2_PROTO_MODE mode = L2_NONE;
        msg.opcode = STP_SET_COMMAND;
        for (auto i : kfvFieldsValues(t))
        {
            SWSS_LOG_DEBUG("Field: %s Val %s", fvField(i).c_str(), fvValue(i).c_str());
            if (fvField(i) == "mode")
            {
                if (fvValue(i) == "pvst")
                    mode = L2_PVSTP;
                else if (fvValue(i) == "mst")
                    mode = L2_MSTP;
                else
                    SWSS_LOG_ERROR("Error: Invalid mode %s", fvValue(i).c_str());
            }
            else if (fvField(i) == "rootguard_timeout")
            {
                msg.rootguard_timeout = stpParseInt(fvValue(i), "STP GLOBAL rootguard_timeout");
            }
        }

        if (mode != L2_NONE)
        {
            // stpd routes a bridge config by the mode in the envelope and
            // keeps the old protocol running under it, so a mode edited in
            // place is taken down first, as a row delete would, before the
            // new mode goes out.
            if (l2ProtoEnabled != L2_NONE && l2ProtoEnabled != mode)
            {
                disableProtocol();
                modeChanged = true;
            }

            if (l2ProtoEnabled == L2_NONE)
            {
                if (mode == L2_PVSTP)
                {
                    const std::string cmd = std::string("") +
                        " ebtables -A FORWARD -d 01:00:0c:cc:cc:cd -j DROP";
                    std::string res;
                    int ret = swss::exec(cmd, res);
                    if (ret != 0)
                        SWSS_LOG_ERROR("ebtables add failed for PVST %d", ret);
                }
                l2ProtoEnabled = mode;
            }
            msg.stp_mode = mode;

            if (mode == L2_MSTP)
            {
                mstEnabled = true;

                // Assign all VLANs to zero instance for MSTP
                fill_n(m_vlanInstMap, MAX_VLANS, 0);
            }
        }

        memcpy(msg.base_mac_addr, macAddress.getMac(), 6);

        // Send the message to the daemon
        sendMsgStpd(STP_BRIDGE_CONFIG, sizeof(msg), (void *)&msg);

        // PVST VLANs without their own timers or priority use the global ones,
        // so a change of one of those is passed on to them. A write that
        // touches none of them (rootguard_timeout, a mode re-write) is not.
        bool inheritedChanged = updateGlobalPvstFields(kfvFieldsValues(t));
        if (inheritedChanged && l2ProtoEnabled == L2_PVSTP && !modeChanged)
            refreshPvstVlans();

        // Enrol VLAN members that pre-existed this MSTP enable. A mode enable
        // does not re-fire STATE_VLAN_MEMBER events, so replay the current
        // membership to stpd now (after the bridge config). Covers first enable
        // and re-enable after an MSTP -> NONE -> MSTP toggle.
        if (mstEnabled)
        {
            enrolMstVlanMembers();
        }

        if (modeChanged)
        {
            replayModeConfig();
        }

        // Move to the next item
        it = consumer.m_toSync.erase(it);
        }
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

// Take the running protocol down in stpd: the bridge delete goes out under
// the mode it is for, then stpmgrd forgets the mode and every instance.
void StpMgr::disableProtocol()
{
    STP_BRIDGE_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(STP_BRIDGE_CONFIG_MSG));
    msg.opcode = STP_DEL_COMMAND;

    // Free Up all instances
    FREE_ALL_INST_ID();
    clearInstanceHolds();

    // Initialize all VLANs to Invalid instance
    fill_n(m_vlanInstMap, MAX_VLANS, INVALID_INSTANCE);

    // Remove ebtables rule based on protocol mode
    if (l2ProtoEnabled == L2_PVSTP)
    {
        const std::string pvst_cmd =
            "ebtables -D FORWARD -d 01:00:0c:cc:cc:cd -j DROP";
        std::string res_pvst;
        int ret_pvst = swss::exec(pvst_cmd, res_pvst);
        if (ret_pvst != 0)
            SWSS_LOG_ERROR("ebtables del failed for PVST %d", ret_pvst);
    }

    // Sent before the mode is cleared: the message is stamped with the
    // current mode, and stpd ignores a bridge config that arrives under L2_NONE.
    sendMsgStpd(STP_BRIDGE_CONFIG, sizeof(msg), (void *)&msg);
    l2ProtoEnabled = L2_NONE;
}

// After a mode change the rows of the new mode's tables are either still
// held or already went out under the old mode and were dropped by stpd.
// Feed them again from CONFIG_DB in the order a start-up takes them.
void StpMgr::replayModeConfig()
{
    vector<Table *> tables = {&m_cfgStpPortTable};
    if (l2ProtoEnabled == L2_PVSTP)
    {
        tables.push_back(&m_cfgStpVlanTable);
        tables.push_back(&m_cfgStpVlanPortTable);
    }
    else if (l2ProtoEnabled == L2_MSTP)
    {
        tables.push_back(&m_cfgMstGlobalTable);
        tables.push_back(&m_cfgMstInstTable);
        tables.push_back(&m_cfgMstInstPortTable);
    }

    for (auto *table : tables)
    {
        addExistingData(table);
        auto *executor = getExecutor(table->getTableName());
        if (executor)
            executor->drain();
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
        try
        {
        STP_VLAN_CONFIG_MSG *msg = NULL;
        uint32_t len = 0;
        bool stpEnable = false;
        uint8_t newInstance = 0;
        int instId, forwardDelay, helloTime, maxAge, priority, portCnt = 0;
        instId = forwardDelay = helloTime = maxAge = priority = portCnt = 0;

        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op  = kfvOp(t);

        string vlanKey = key.substr(4); // Remove Vlan prefix
    int vlan_id = stpParseInt(vlanKey, "STP_VLAN key");

        SWSS_LOG_INFO("STP vlan key %s op %s", key.c_str(), op.c_str());
        if (op == SET_COMMAND)
        {
            if (l2ProtoEnabled == L2_NONE || !isVlanStateOk(key))
            {
                // Wait till STP is configured
                it++;
                continue;
            }

            readStpVlanFields(kfvFieldsValues(t), stpEnable, forwardDelay, helloTime, maxAge, priority);
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
                        SWSS_LOG_ERROR("No free STP instance for VLAN %d: the hardware holds %u, "
                                       "spanning tree stays off for it until one is freed",
                                       vlan_id, max_stp_instances);
                        holdForInstance(m_stateStpVlanTable, key, m_vlansWaitingForInstance, vlan_id);
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                    releaseHold(m_stateStpVlanTable, key, m_vlansWaitingForInstance, vlan_id);

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
                // Already deallocated, or never allocated because no
                // instance was free. Nothing to tell stpd either way.
                releaseHold(m_stateStpVlanTable, key, m_vlansWaitingForInstance, vlan_id);
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
            retryWaitingVlan();
        }

        sendMsgStpd(STP_VLAN_CONFIG, len, (void *)msg);
        if (msg)
            free(msg);

        it = consumer.m_toSync.erase(it);
        }
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

// Read an STP_VLAN row. A timer or priority the row leaves out stays -1 and is
// filled from STP|GLOBAL, then from the protocol default.
void StpMgr::readStpVlanFields(const vector<FieldValueTuple> &fvs, bool &enabled,
                               int &forwardDelay, int &helloTime, int &maxAge, int &priority)
{
    enabled = false;
    forwardDelay = helloTime = maxAge = priority = -1;
    for (const auto &fv : fvs)
    {
        SWSS_LOG_DEBUG("Field: %s Val: %s", fvField(fv).c_str(), fvValue(fv).c_str());
        if (fvField(fv) == "enabled")
            enabled = (fvValue(fv) == "true");
        else if (fvField(fv) == "forward_delay")
            forwardDelay = stpParseInt(fvValue(fv), "STP_VLAN forward_delay");
        else if (fvField(fv) == "hello_time")
            helloTime = stpParseInt(fvValue(fv), "STP_VLAN hello_time");
        else if (fvField(fv) == "max_age")
            maxAge = stpParseInt(fvValue(fv), "STP_VLAN max_age");
        else if (fvField(fv) == "priority")
            priority = stpParseInt(fvValue(fv), "STP_VLAN priority");
    }

    vector<FieldValueTuple> global;
    m_cfgStpGlobalTable.get("GLOBAL", global);
    auto fill = [this, &global](int &val, const string &field, int dflt) {
        if (val >= 0)
            return;
        val = dflt;
        for (const auto &fv : global)
        {
            if (fvField(fv) == field)
            {
                val = parseGlobalInt(field, fvValue(fv), dflt);
                break;
            }
        }
    };
    fill(forwardDelay, "forward_delay", STP_DFLT_FORWARD_DELAY);
    fill(helloTime, "hello_time", STP_DFLT_HELLO_TIME);
    fill(maxAge, "max_age", STP_DFLT_MAX_AGE);
    fill(priority, "priority", STP_DFLT_BRIDGE_PRIORITY);
}

// An integer field of STP|GLOBAL. A value that is not a number falls back to
// the protocol default and is reported once per value, so one bad global
// field cannot stop the VLAN rows from being programmed.
int StpMgr::parseGlobalInt(const string &field, const string &value, int dflt)
{
    try
    {
        int parsed = stpParseInt(value, ("STP GLOBAL " + field).c_str());
        m_badGlobalFields.erase(field);
        return parsed;
    }
    catch (const std::exception &e)
    {
        auto seen = m_badGlobalFields.find(field);
        if (seen == m_badGlobalFields.end() || seen->second != value)
        {
            SWSS_LOG_ERROR("stpmgr: %s, using %d", e.what(), dflt);
            m_badGlobalFields[field] = value;
        }
        return dflt;
    }
}

// Remember the STP|GLOBAL priority and timers the PVST VLANs inherit. True
// when this row changed one of them; a field the row leaves out counts as
// the protocol default.
bool StpMgr::updateGlobalPvstFields(const vector<FieldValueTuple> &fvs)
{
    int priority = STP_DFLT_BRIDGE_PRIORITY;
    int forwardDelay = STP_DFLT_FORWARD_DELAY;
    int helloTime = STP_DFLT_HELLO_TIME;
    int maxAge = STP_DFLT_MAX_AGE;

    for (const auto &fv : fvs)
    {
        if (fvField(fv) == "priority")
            priority = parseGlobalInt("priority", fvValue(fv), STP_DFLT_BRIDGE_PRIORITY);
        else if (fvField(fv) == "forward_delay")
            forwardDelay = parseGlobalInt("forward_delay", fvValue(fv), STP_DFLT_FORWARD_DELAY);
        else if (fvField(fv) == "hello_time")
            helloTime = parseGlobalInt("hello_time", fvValue(fv), STP_DFLT_HELLO_TIME);
        else if (fvField(fv) == "max_age")
            maxAge = parseGlobalInt("max_age", fvValue(fv), STP_DFLT_MAX_AGE);
    }

    bool changed = priority != m_globalPriority || forwardDelay != m_globalForwardDelay ||
                   helloTime != m_globalHelloTime || maxAge != m_globalMaxAge;
    m_globalPriority = priority;
    m_globalForwardDelay = forwardDelay;
    m_globalHelloTime = helloTime;
    m_globalMaxAge = maxAge;
    return changed;
}

// Re-send the timers and priority of every PVST VLAN that already has an
// instance, after STP|GLOBAL changed.
void StpMgr::refreshPvstVlans()
{
    vector<string> keys;
    m_cfgStpVlanTable.getKeys(keys);
    for (const auto &key : keys)
    {
        if (key.compare(0, 4, "Vlan") != 0)
            continue;
        try
        {
            int vlan_id = stpParseInt(key.substr(4), "STP_VLAN key");
            if (vlan_id <= 0 || vlan_id >= MAX_VLANS || m_vlanInstMap[vlan_id] == INVALID_INSTANCE)
                continue;

            vector<FieldValueTuple> fvs;
            if (!m_cfgStpVlanTable.get(key, fvs))
                continue;

            STP_VLAN_CONFIG_MSG msg;
            memset(&msg, 0, sizeof(msg));
            bool enabled;
            readStpVlanFields(fvs, enabled, msg.forward_delay, msg.hello_time, msg.max_age, msg.priority);
            if (!enabled)
                continue;

            msg.opcode  = STP_SET_COMMAND;
            msg.vlan_id = vlan_id;
            msg.inst_id = m_vlanInstMap[vlan_id];
            sendMsgStpd(STP_VLAN_CONFIG, sizeof(msg), (void *)&msg);
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("stpmgr: skipping STP_VLAN '%s' on global refresh: %s", key.c_str(), e.what());
        }
    }
}

void StpMgr::doStpMstGlobalTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (stpGlobalTask == false)
        return;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        try
        {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        // Region config sent before the mode has gone out lands on a bridge
        // that is re-initialised by the mode message and loses it; wait.
        if (op == SET_COMMAND && l2ProtoEnabled != L2_MSTP)
        {
            it++;
            continue;
        }

        SWSS_LOG_INFO("STP MST global key %s op %s", key.c_str(), op.c_str());

        STP_MST_GLOBAL_CONFIG_MSG msg;
        memset(&msg, 0, sizeof(msg)); // Initialize message structure to zero

        if (op == SET_COMMAND)
        {
            msg.opcode = STP_SET_COMMAND;

            for (auto i : kfvFieldsValues(t))
            {
                SWSS_LOG_DEBUG("Field: %s Val: %s", fvField(i).c_str(), fvValue(i).c_str());

                if (fvField(i) == "name")
                {
                    strncpy(msg.name, fvValue(i).c_str(), sizeof(msg.name) - 1);
                }
                else if (fvField(i) == "revision")
                {
                    msg.revision_number = static_cast<uint32_t>(stpParseInt(fvValue(i), "STP_MST GLOBAL revision"));
                }
                else if (fvField(i) == "forward_delay")
                {
                    msg.forward_delay = static_cast<uint8_t>(stpParseInt(fvValue(i), "STP_MST GLOBAL forward_delay"));
                }
                else if (fvField(i) == "hello_time")
                {
                    msg.hello_time = static_cast<uint8_t>(stpParseInt(fvValue(i), "STP_MST GLOBAL hello_time"));
                }
                else if (fvField(i) == "max_age")
                {
                    msg.max_age = static_cast<uint8_t>(stpParseInt(fvValue(i), "STP_MST GLOBAL max_age"));
                }
                else if (fvField(i) == "max_hops")
                {
                    msg.max_hops = static_cast<uint8_t>(stpParseInt(fvValue(i), "STP_MST GLOBAL max_hops"));
                }
                else
                {
                    SWSS_LOG_ERROR("Invalid field: %s", fvField(i).c_str());
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            msg.opcode = STP_DEL_COMMAND;
        }

        sendMsgStpd(STP_MST_GLOBAL_CONFIG, sizeof(msg), (void *)&msg);

        it = consumer.m_toSync.erase(it);
        }
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
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
                msg.path_cost = stpParseInt(fvValue(i), "STP_VLAN_PORT path_cost");
            }
            else if (fvField(i) == "priority")
            {
                msg.priority = stpParseInt(fvValue(i), "STP_VLAN_PORT priority");
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
        try
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
            vlan_id = stpParseInt(vlanKey.substr(0, found), "STP_VLAN_PORT key");
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
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void StpMgr::processStpPortAttr(const string op,
                                vector<FieldValueTuple> &tupEntry,
                                const string intfName)
{
    STP_PORT_CONFIG_MSG *msg = nullptr;
    uint32_t len = 0;
    int vlanCnt = 0;
    vector<VLAN_ATTR> vlan_list;

    // If we're setting this port's attributes, retrieve the list of VLANs for it.
    if (op == SET_COMMAND)
    {
        vlanCnt = getAllPortVlan(intfName, vlan_list);
    }

    // Allocate enough space for STP_PORT_CONFIG_MSG + all VLAN_ATTR entries.
    len = static_cast<uint32_t>(
        sizeof(STP_PORT_CONFIG_MSG) + (vlanCnt * sizeof(VLAN_ATTR))
    );
    msg = static_cast<STP_PORT_CONFIG_MSG *>(calloc(1, len));
    if (!msg)
    {
        SWSS_LOG_ERROR("calloc failed for interface %s", intfName.c_str());
        return;
    }
    // Copy interface name and VLAN count into the message.
    strncpy(msg->intf_name, intfName.c_str(), IFNAMSIZ - 1);
    msg->count = vlanCnt;
    SWSS_LOG_INFO("VLAN count for %s is %d", intfName.c_str(), vlanCnt);
    SWSS_LOG_INFO("VLAN count for %s is %d", intfName.c_str(), vlanCnt);

    // If there are VLANs, copy them into the message structure.
    if (msg->count > 0)
    // If there are VLANs, copy them into the message structure.
    if (msg->count > 0)
    {
        for (int i = 0; i < msg->count; i++)
        {
            msg->vlan_list[i].inst_id = vlan_list[i].inst_id;
            msg->vlan_list[i].mode    = vlan_list[i].mode;
            msg->vlan_list[i].vlan_id = vlan_list[i].vlan_id;
            SWSS_LOG_DEBUG("Inst:%d Mode:%d",
                           vlan_list[i].inst_id,
                           vlan_list[i].mode);
        }
    }

    // Populate message fields based on the operation (SET or DEL).
    if (op == SET_COMMAND)
    {
        msg->opcode   = STP_SET_COMMAND;
        msg->priority = -1; // Default priority unless specified

        for (auto &fvt : tupEntry)
        {
            const auto &field = fvField(fvt);
            const auto &value = fvValue(fvt);

            SWSS_LOG_DEBUG("Field: %s, Value: %s", field.c_str(), value.c_str());

            if (field == "enabled")
            {
                msg->enabled = (value == "true") ? 1 : 0;
            }
            else if (field == "root_guard")
            {
                msg->root_guard = (value == "true") ? 1 : 0;
            }
            else if (field == "loop_guard")
            {
                msg->loop_guard = (value == "true") ? 1 : 0;
            }
            else if (field == "bpdu_guard")
            {
                msg->bpdu_guard = (value == "true") ? 1 : 0;
            }
            else if (field == "bpdu_guard_do_disable")
            {
                msg->bpdu_guard_do_disable = (value == "true") ? 1 : 0;
            }
            else if (field == "path_cost")
            {
                msg->path_cost = stpParseInt(value, "STP_PORT path_cost");
            }
            else if (field == "priority")
            {
                msg->priority = stpParseInt(value, "STP_PORT priority");
            }
            else if (field == "portfast" && l2ProtoEnabled == L2_PVSTP)
            {
                msg->portfast = (value == "true") ? 1 : 0;
            }
            else if (field == "uplink_fast" && l2ProtoEnabled ==L2_PVSTP)
            {
                msg->uplink_fast = (value == "true") ? 1 : 0;
            }
            else if (field == "edge_port" && l2ProtoEnabled ==L2_MSTP)
            {
                msg->edge_port = (value == "true") ? 1 : 0;
            }
            else if (field == "link_type" && l2ProtoEnabled == L2_MSTP)
            {
                // Map the link_type value string to the LinkType enum.
                // (Previously this called stoi(field) which parsed the field
                //  name "link_type" and threw std::invalid_argument.)
                if (value == "point-to-point" || value == "p2p")
                    msg->link_type = POINT_TO_POINT;
                else if (value == "shared")
                    msg->link_type = SHARED;
                else
                    msg->link_type = AUTO;
            }
        }
    }
    else if (op == DEL_COMMAND)
    {
        msg->opcode  = STP_DEL_COMMAND;
        msg->enabled = 0;
    }

    // Send the fully prepared message to the STP daemon.
    sendMsgStpd(STP_PORT_CONFIG, len, reinterpret_cast<void *>(msg));

    // Clean up.
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
        try
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
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

// Send stpd the MSTP per-VLAN member port list (STP_MST_VLAN_PORT_LIST_CONFIG).
// stpd resolves the owning instance (CIST for VLANs not mapped to an MSTI) and
// enrols/removes the ports. Shared by the STATE_VLAN_MEMBER event path and the
// enable-time replay (enrolMstVlanMembers).
void StpMgr::sendMstVlanPortList(int vlan_id, const vector<PORT_ATTR> &memPorts, bool add)
{
    uint32_t cnt = static_cast<uint32_t>(memPorts.size());
    uint32_t mlen = sizeof(STP_MST_VLAN_PORT_MAP) + static_cast<uint32_t>(cnt * sizeof(PORT_LIST));
    STP_MST_VLAN_PORT_MAP *vpmsg = (STP_MST_VLAN_PORT_MAP *)calloc(1, mlen);
    if (!vpmsg)
    {
        SWSS_LOG_ERROR("Memory allocation failed for STP_MST_VLAN_PORT_MAP");
        return;
    }
    vpmsg->vlan_id    = static_cast<uint16_t>(vlan_id);
    vpmsg->port_count = static_cast<uint16_t>(cnt);
    vpmsg->stp_mode   = L2_MSTP;
    vpmsg->add        = add ? 1 : 0;
    for (uint32_t i = 0; i < cnt; i++)
    {
        strncpy(vpmsg->port_list[i].intf_name, memPorts[i].intf_name, IFNAMSIZ-1);
        vpmsg->port_list[i].tagging_mode = memPorts[i].mode;
    }
    SWSS_LOG_INFO("MST vlan-port-list vlan:%d add:%d ports:%u", vlan_id, vpmsg->add, cnt);
    sendMsgStpd(STP_MST_VLAN_PORT_LIST_CONFIG, mlen, (void *)vpmsg);
    free(vpmsg);
}

// Replay the current VLAN->member mapping to stpd. Called when MSTP is enabled
// so that VLAN members which pre-existed the enable are enrolled. A member's
// STATE_VLAN_MEMBER event is delivered only once (at stpmgrd start / member
// add); enabling MSTP later does NOT re-fire those events, so without this the
// pre-existing members never reach stpd and their ports never enrol in the CIST
// (previously masked by a manual VLAN-membership flap). This reads STATE_DB
// directly, so it recovers members regardless of whether their event was
// consumed while STP was disabled.
void StpMgr::enrolMstVlanMembers()
{
    if (l2ProtoEnabled != L2_MSTP)
        return;

    vector<string> vmKeys;
    m_stateVlanMemberTable.getKeys(vmKeys);

    set<int> vlans;
    for (const auto &key : vmKeys)
    {
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR);
        if (found == string::npos)
            continue;
        string vlanName = key.substr(0, found);   // "Vlan<id>"
        if (vlanName.compare(0, 4, "Vlan") != 0)
            continue;
        try
        {
            vlans.insert(stoi(vlanName.substr(4)));
        }
        catch (const exception &e)
        {
            SWSS_LOG_ERROR("enrolMstVlanMembers: bad VLAN key %s: %s", key.c_str(), e.what());
        }
    }

    for (int vlan_id : vlans)
    {
        vector<PORT_ATTR> memPorts;
        if (getAllVlanMem("Vlan" + to_string(vlan_id), memPorts) > 0)
            sendMstVlanPortList(vlan_id, memPorts, /*add=*/true);
    }
}

void StpMgr::doVlanMemUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        try
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
            vlan_id = stpParseInt(vlanKey.substr(0, found), "VLAN_MEMBER key");
            intfName = vlanKey.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format. No member port is presented: %s", kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        // MSTP: stpd programs MST port state from STP_MST_VLAN_PORT_LIST_CONFIG
        // (a per-VLAN member port list); it resolves the owning instance itself
        // (CIST for VLANs not mapped to an MSTI). stpd ignores the PVST
        // STP_VLAN_MEM_CONFIG path below when running MSTP.
        if (l2ProtoEnabled == L2_MSTP)
        {
            vector<PORT_ATTR> memPorts;
            if (op == SET_COMMAND)
            {
                getAllVlanMem("Vlan" + to_string(vlan_id), memPorts);
            }
            else
            {
                PORT_ATTR pa;
                memset(&pa, 0, sizeof(pa));
                strncpy(pa.intf_name, intfName.c_str(), IFNAMSIZ-1);
                pa.mode = getVlanMemMode(key);
                memPorts.push_back(pa);
            }

            sendMstVlanPortList(vlan_id, memPorts, /*add=*/(op == SET_COMMAND));
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
                            msg.priority = stpParseInt(entry.second, "STP_VLAN_PORT priority");
                        else if (entry.first == "path_cost")
                            msg.path_cost = stpParseInt(entry.second, "STP_VLAN_PORT path_cost");
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
        else
        {
            // Dropping this is legitimate, but silence here is the single
            // most common reason a port looks like it never enrolled.
            SWSS_LOG_WARN("STP not notified of vlan member change %s op:%s: %s",
                          key.c_str(), op.c_str(),
                          (m_vlanInstMap[vlan_id] == INVALID_INSTANCE)
                              ? "STP is not enabled on this VLAN"
                              : "port channel has no members");
        }

        it = consumer.m_toSync.erase(it);
        }
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

// PVST: tell stpd that intfName is a member of vlan_id, the same message
// doVlanMemUpdateTask() sends when the membership first appears.
void StpMgr::sendVlanMemConfig(int vlan_id, const string &intfName, const string &vlanMemKey, bool add)
{
    if (m_vlanInstMap[vlan_id] == INVALID_INSTANCE)
    {
        // Dropping this is legitimate, but silence here is the single
        // most common reason a port looks like it never enrolled.
        SWSS_LOG_WARN("STP not notified of vlan member change %s op:%s: STP is not enabled on this VLAN",
                      vlanMemKey.c_str(), add ? "add" : "del");
        return;
    }

    // No mode means the CONFIG_DB row is gone; its delete reaches stpd on
    // its own, so there is nothing to send.
    int8_t tagging_mode = getVlanMemMode(vlanMemKey);
    if (tagging_mode == INVALID_MODE)
    {
        return;
    }

    STP_VLAN_MEM_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(STP_VLAN_MEM_CONFIG_MSG));
    msg.opcode    = add ? STP_SET_COMMAND : STP_DEL_COMMAND;
    msg.vlan_id   = vlan_id;
    msg.inst_id   = m_vlanInstMap[vlan_id];
    msg.mode      = tagging_mode;
    msg.enabled   = isStpEnabled(intfName);
    msg.priority  = -1;
    msg.path_cost = 0;
    strncpy(msg.intf_name, intfName.c_str(), IFNAMSIZ-1);

    sendMsgStpd(STP_VLAN_MEM_CONFIG, sizeof(msg), (void *)&msg);
}

// Replay every VLAN membership of a PortChannel to stpd, with the STP_VLAN_PORT
// row for each. Used when the LAG gets its first member back: while it had
// none, isLagEmpty() made the membership and per-VLAN tasks skip it.
void StpMgr::reenrolLagVlans(const string &po_name)
{
    vector<string> vmKeys;
    m_stateVlanMemberTable.getKeys(vmKeys);

    for (const auto &key : vmKeys)
    {
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR);
        if (found == string::npos || key.compare(0, 4, "Vlan") != 0)
        {
            continue;
        }

        string vlanName = key.substr(0, found);
        if (key.substr(found + 1) != po_name)
        {
            continue;
        }

        int vlan_id;
        try
        {
            vlan_id = stpParseInt(vlanName.substr(4), "VLAN_MEMBER key");
        }
        catch (const exception &e)
        {
            SWSS_LOG_ERROR("reenrolLagVlans: bad VLAN key %s: %s", key.c_str(), e.what());
            continue;
        }

        if (l2ProtoEnabled == L2_MSTP)
        {
            PORT_ATTR pa;
            memset(&pa, 0, sizeof(pa));
            strncpy(pa.intf_name, po_name.c_str(), IFNAMSIZ-1);
            pa.mode = getVlanMemMode(key);
            if (pa.mode == INVALID_MODE)
            {
                continue;
            }
            vector<PORT_ATTR> memPorts = { pa };
            sendMstVlanPortList(vlan_id, memPorts, /*add=*/true);
        }
        else
        {
            sendVlanMemConfig(vlan_id, po_name, key, /*add=*/true);
        }

        vector<FieldValueTuple> vlanPortTup;
        if (m_cfgStpVlanPortTable.get(key, vlanPortTup))
        {
            processStpVlanPortAttr(SET_COMMAND, vlan_id, po_name, vlanPortTup);
        }
    }
}

void StpMgr::doLagMemUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        try
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
            vector<FieldValueTuple> tupEntry;

            // stpd dropped the PortChannel node with its last member and
            // re-creates it on STP_PORT_CONFIG, or later from netlink once the
            // member is enslaved. The port's own config goes first, with
            // defaults when it has no row, so the membership that follows on
            // the same socket finds the port.
            m_cfgStpPortTable.get(po_name, tupEntry);
            processStpPortAttr(op, tupEntry, po_name);

            // The LAG left spanning tree with its last member, so its VLAN
            // membership and per-VLAN settings have to reach stpd again.
            // STP_PORT alone carries only the port's own settings, and a
            // PortChannel with no STP_PORT row is still a VLAN member.
            reenrolLagVlans(po_name);
        }

        SWSS_LOG_DEBUG("LagMap");
        for (auto itr = m_lagMap.begin(); itr != m_lagMap.end(); ++itr) {
            SWSS_LOG_DEBUG("PO: %s Cnt:%d", itr->first.c_str(), itr->second);
        }

        it = consumer.m_toSync.erase(it);
        }
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void StpMgr::ipcInitStpd()
{
    int ret;
	struct sockaddr_un addr;

    unlink(STPMGRD_SOCK_NAME);
    // create socket
    stpd_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (stpd_fd < 0) {
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
        stpd_fd = -1;
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

bool StpMgr::mstInstanceLimitReached()
{
    size_t count = m_mstInstances.size() + (m_mstInstances.count(0) ? 0 : 1);
    return count >= max_stp_instances;
}

void StpMgr::holdForInstance(Table &stateTable, const string &key, set<int> &waiting, int id)
{
    waiting.insert(id);
    stateTable.set(key, {{STP_INSTANCE_STATE_FIELD, STP_INSTANCE_UNAVAILABLE},
                         {"reason", "hardware limit of " + to_string(max_stp_instances) +
                                    " spanning tree instances reached"}});
}

void StpMgr::releaseHold(Table &stateTable, const string &key, set<int> &waiting, int id)
{
    if (waiting.erase(id))
        stateTable.del(key);
}

// An instance was freed: queue the lowest waiting VLAN's row again, as if it
// had just been written.
void StpMgr::retryWaitingVlan()
{
    while (!m_vlansWaitingForInstance.empty())
    {
        int vlan_id = *m_vlansWaitingForInstance.begin();
        string key = "Vlan" + to_string(vlan_id);
        vector<FieldValueTuple> fvs;

        if (!m_cfgStpVlanTable.get(key, fvs))
        {
            releaseHold(m_stateStpVlanTable, key, m_vlansWaitingForInstance, vlan_id);
            continue;
        }

        Consumer *consumer = dynamic_cast<Consumer *>(getExecutor(CFG_STP_VLAN_TABLE_NAME));
        if (consumer)
        {
            SWSS_LOG_NOTICE("STP instance freed, retrying VLAN %d", vlan_id);
            consumer->addToSync(KeyOpFieldsValuesTuple(key, SET_COMMAND, fvs));
        }
        return;
    }
}

void StpMgr::retryWaitingMstInstance()
{
    while (!m_mstInstancesWaiting.empty())
    {
        int instance_id = *m_mstInstancesWaiting.begin();
        string key = to_string(instance_id);
        vector<FieldValueTuple> fvs;

        if (!m_cfgMstInstTable.get(key, fvs))
        {
            releaseHold(m_stateStpMstInstTable, key, m_mstInstancesWaiting, instance_id);
            continue;
        }

        Consumer *consumer = dynamic_cast<Consumer *>(getExecutor("STP_MST_INST"));
        if (consumer)
        {
            SWSS_LOG_NOTICE("STP instance freed, retrying MST instance %d", instance_id);
            consumer->addToSync(KeyOpFieldsValuesTuple(key, SET_COMMAND, fvs));
        }
        return;
    }
}

void StpMgr::clearInstanceHolds()
{
    for (int vlan_id : m_vlansWaitingForInstance)
        m_stateStpVlanTable.del("Vlan" + to_string(vlan_id));
    for (int instance_id : m_mstInstancesWaiting)
        m_stateStpMstInstTable.del(to_string(instance_id));
    m_vlansWaitingForInstance.clear();
    m_mstInstancesWaiting.clear();
    m_mstInstances.clear();
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
            vlan_id = stpParseInt(vlanKey.substr(0, found), "VLAN_MEMBER key");
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

void StpMgr::doStpMstInstTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (stpGlobalTask == false || (stpPortTask == false && !isStpPortEmpty()))
        return;

    if (stpMstInstTask == false)
        stpMstInstTask = true;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        try
        {
        STP_MST_INST_CONFIG_MSG *msg = NULL;
        uint32_t len = 0;

        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        string instance = key; // Key is "<id>" (YANG STP_MST_INST_LIST key "instance")
        uint16_t instance_id = static_cast<uint16_t>(stpParseInt(instance, "STP_MST_INST id"));

        uint16_t priority = 32768; // Default bridge priority
        string vlan_list_str;
        vector<uint16_t> vlan_ids;
        // The YANG model defines the mapping as leaf-list "vlan", stored as
        // "vlan@" and read back as "vlan". Rows written by older images
        // carry "vlan_list" instead.
        // Both are collected here and chosen between after the loop, so the
        // result does not depend on the order the fields arrive in.
        string legacyVlanStr;
        bool haveYangVlans = false;
        bool haveLegacyVlans = false;

        SWSS_LOG_INFO("STP_MST instance key %s op %s", key.c_str(), op.c_str());
        if (op == SET_COMMAND && l2ProtoEnabled != L2_MSTP)
        {
            // An instance sent before the mode has gone out is wiped by it; wait.
            it++;
            continue;
        }
        if (op == SET_COMMAND)
        {
            if (instance_id != 0 && !m_mstInstances.count(instance_id) && mstInstanceLimitReached())
            {
                SWSS_LOG_ERROR("No free STP instance for MST instance %u: the hardware holds %u "
                               "including the CIST, it stays down until one is deleted",
                               instance_id, max_stp_instances);
                holdForInstance(m_stateStpMstInstTable, key, m_mstInstancesWaiting, instance_id);
                it = consumer.m_toSync.erase(it);
                continue;
            }
            m_mstInstances.insert(instance_id);
            releaseHold(m_stateStpMstInstTable, key, m_mstInstancesWaiting, instance_id);

            for (auto i : kfvFieldsValues(t))
            {
                SWSS_LOG_DEBUG("Field: %s Val: %s", fvField(i).c_str(), fvValue(i).c_str());

                if (fvField(i) == "bridge_priority")
                {
                    priority = static_cast<uint16_t>(stpParseInt(fvValue(i), "STP_MST_INST bridge_priority"));
                }
                else if (fvField(i) == STP_MST_INST_VLAN_FIELD)
                {
                    vlan_list_str = fvValue(i);
                    haveYangVlans = true;
                }
                else if (fvField(i) == STP_MST_INST_VLAN_FIELD_LEGACY)
                {
                    legacyVlanStr = fvValue(i);
                    haveLegacyVlans = true;
                }
            }

            if (!haveYangVlans && haveLegacyVlans)
            {
                // Accept the old spelling so a mapping written by an older
                // image is not dropped, and say so rather than acting on it
                // in silence.
                vlan_list_str = legacyVlanStr;
                SWSS_LOG_WARN("STP_MST_INST|%s uses the pre-YANG field '%s'; "
                              "rewrite the mapping to '%s'",
                              key.c_str(), STP_MST_INST_VLAN_FIELD_LEGACY,
                              STP_MST_INST_VLAN_FIELD);
            }
            else if (haveYangVlans && haveLegacyVlans)
            {
                // A row can carry both when an old image wrote it and a new
                // one touched it since. That is data, not a bug in here, so
                // it is said and the current spelling wins.
                SWSS_LOG_WARN("STP_MST_INST|%s carries both '%s' and the pre-YANG '%s'; "
                              "using '%s', delete the other",
                              key.c_str(), STP_MST_INST_VLAN_FIELD,
                              STP_MST_INST_VLAN_FIELD_LEGACY, STP_MST_INST_VLAN_FIELD);
            }

            if (!parseVlanList(vlan_list_str, vlan_ids))
                throw std::invalid_argument("STP_MST_INST " + key + " has an invalid VLAN list '" + vlan_list_str + "'");
            updateVlanInstanceMap(instance_id, vlan_ids, true);

            uint32_t vlan_count = static_cast<uint32_t>(vlan_ids.size());
            len = sizeof(STP_MST_INST_CONFIG_MSG) + static_cast<uint32_t>(vlan_count * sizeof(VLAN_LIST));

            msg = (STP_MST_INST_CONFIG_MSG *)calloc(1, len);
            if (!msg)
            {
                SWSS_LOG_ERROR("Memory allocation failed for STP_MST_INST_CONFIG_MSG");
                return;
            }

            msg->opcode = STP_SET_COMMAND;
            msg->mst_id = instance_id;
            msg->priority = priority;
            msg->vlan_count = static_cast<uint16_t>(vlan_ids.size());
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Waddress-of-packed-member"
            VLAN_LIST *vlan_attr = (VLAN_LIST *)&msg->vlan_list;
            #pragma GCC diagnostic pop
            for (size_t i = 0; i < vlan_ids.size(); i++)
            {
                vlan_attr[i].vlan_id = vlan_ids[i];
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_mstInstancesWaiting.count(instance_id))
            {
                // stpd never had it
                releaseHold(m_stateStpMstInstTable, key, m_mstInstancesWaiting, instance_id);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            len = sizeof(STP_MST_INST_CONFIG_MSG);
            msg = (STP_MST_INST_CONFIG_MSG *)calloc(1, len);
            if (!msg)
            {
                SWSS_LOG_ERROR("Memory allocation failed for MST_INST_CONFIG_MSG");
                return;
            }

            msg->opcode = STP_DEL_COMMAND;
            msg->mst_id = instance_id;
            updateVlanInstanceMap(instance_id, vlan_ids, false);
            m_mstInstances.erase(instance_id);
        }

        sendMsgStpd(STP_MST_INST_CONFIG, len, (void *)msg);
        if (msg)
            free(msg);

        it = consumer.m_toSync.erase(it);
        if (op == DEL_COMMAND)
            retryWaitingMstInstance();
        }
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void StpMgr::processStpMstInstPortAttr(const string op, uint16_t mst_id, const string intfName,
                                       vector<FieldValueTuple>& tupEntry)
{
    STP_MST_INST_PORT_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(STP_MST_INST_PORT_CONFIG_MSG));

    // Populate the message fields
    msg.mst_id = mst_id;
    strncpy(msg.intf_name, intfName.c_str(), IFNAMSIZ - 1);

    // Set opcode and process the fields from the tuple
    if (op == SET_COMMAND)
    {
        msg.opcode = STP_SET_COMMAND;
        // Both attributes are optional on the row and the memset leaves them
        // at 0, which stpd reads as an explicit value in MST mode. A row
        // carrying only one of them would otherwise pin the other to zero.
        msg.priority = STP_MST_PORT_ATTR_UNSET;
        msg.path_cost = STP_MST_PORT_ATTR_UNSET;

        for (auto i : tupEntry)
        {
            SWSS_LOG_DEBUG("Field: %s Val: %s", fvField(i).c_str(), fvValue(i).c_str());

            if (fvField(i) == "path_cost")
            {
                msg.path_cost = stpParseInt(fvValue(i), "STP_MST_INST_PORT path_cost");
            }
            else if (fvField(i) == "priority")
            {
                msg.priority = stpParseInt(fvValue(i), "STP_MST_INST_PORT priority");
            }
        }
    }
    else if (op == DEL_COMMAND)
    {
        msg.opcode = STP_DEL_COMMAND;
    }

    // Send the message to the daemon
    sendMsgStpd(STP_MST_INST_PORT_CONFIG, sizeof(msg), (void *)&msg);
}


void StpMgr::doStpMstInstPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (stpGlobalTask == false || stpMstInstTask == false || stpPortTask == false)
        return;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        try
        {
        STP_MST_INST_PORT_CONFIG_MSG msg;
        memset(&msg, 0, sizeof(STP_MST_INST_PORT_CONFIG_MSG));

        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND && l2ProtoEnabled != L2_MSTP)
        {
            // Sent before the mode has gone out it is wiped by the mode message; wait.
            it++;
            continue;
        }

        string mstKey = key; // Key is "<id>|<intf>" (YANG STP_MST_PORT_LIST key "inst_id ifname")
        size_t found = mstKey.find(CONFIGDB_KEY_SEPARATOR);

        uint16_t mst_id;
        string intfName;
        if (found != string::npos)
        {
            mst_id = static_cast<uint16_t>(stpParseInt(mstKey.substr(0, found), "STP_MST_INST_PORT key"));
            intfName = mstKey.substr(found + 1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format %s", kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        SWSS_LOG_INFO("STP MST intf key:%s op:%s", key.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            if ((l2ProtoEnabled == L2_NONE))
            {
                // Wait till STP/MST instance is configured
                it++;
                continue;
            }
        }
        else
        {
            if (l2ProtoEnabled == L2_NONE || !(isInstanceMapped(mst_id)))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }

        processStpMstInstPortAttr(op, mst_id, intfName, kfvFieldsValues(t));

        it = consumer.m_toSync.erase(it);
        }
        catch (const std::exception &e)
        {
            // A malformed field/key value threw during processing;
            // log the offending entry and drop it instead of exiting stpmgrd.
            SWSS_LOG_ERROR("stpmgr: dropping unparseable CONFIG_DB entry '%s': %s",
                           kfvKey(it->second).c_str(), e.what());
            it = consumer.m_toSync.erase(it);
        }
    }
}

// Send Message to STPd
int StpMgr::sendMsgStpd(STP_MSG_TYPE msgType, uint32_t msgLen, void *data)
{
    if (stpd_fd < 0)
    {
        SWSS_LOG_ERROR("STPd IPC socket not initialized, dropping msg type %d", msgType);
        return -1;
    }

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
    // stpd gates several handlers on proto_mode (e.g. STP_VLAN_MEM_CONFIG for
    // PVST, STP_MST_VLAN_PORT_LIST_CONFIG for MST). Populate it with the active
    // protocol mode; previously it was left 0 (L2_NONE) so those handlers were
    // silently skipped and port state was never programmed.
    tx_msg->proto_mode = l2ProtoEnabled;
    memcpy(tx_msg->data, data, msgLen);

    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, STPD_SOCK_NAME, sizeof(addr.sun_path)-1);

    // A send that finds no listener (stpd not bound yet, or its queue full)
    // is retried for a short while instead of dropping the message. The retry
    // budget is spent once per backoff window: while stpd stays away every
    // further message costs one sendto, so a queue of pending updates cannot
    // hold the event loop for minutes.
    bool mayRetry = !m_sendFailed ||
        std::chrono::steady_clock::now() - m_lastSendFailure >= std::chrono::seconds(STPD_SEND_BACKOFF_S);
    for (int attempt = 0; ; attempt++)
    {
        rc = (int)sendto(stpd_fd, (void*)tx_msg, len, 0, (struct sockaddr *)&addr, sizeof(addr));
        if (rc != -1 || !mayRetry || attempt >= STPD_SEND_RETRIES ||
            (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN && errno != ENOBUFS))
            break;
        usleep(STPD_SEND_RETRY_US);
    }
    if (rc == -1)
    {
        m_sendFailed = true;
        m_lastSendFailure = std::chrono::steady_clock::now();
        SWSS_LOG_ERROR("tx_msg send error: %s", strerror(errno));
    }
    else
    {
        m_sendFailed = false;
        SWSS_LOG_INFO("tx_msg sent %d", rc);
    }

    free(tx_msg);
    return rc;
}

// Wait until stpd has bound its socket, so the first messages are not lost.
// A datagram connect() succeeds only once something is bound to the path; a
// stale socket file from an earlier run is refused.
bool StpMgr::waitStpdReady(int timeout_ms)
{
    struct sockaddr_un addr;
    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, STPD_SOCK_NAME, sizeof(addr.sun_path)-1);

    for (int waited = 0; waited <= timeout_ms; waited += STPD_READY_POLL_MS)
    {
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            SWSS_LOG_ERROR("probe socket error %s", strerror(errno));
            return false;
        }
        int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        close(fd);
        if (rc == 0)
        {
            SWSS_LOG_NOTICE("stpd ready after %d ms", waited);
            return true;
        }
        usleep(STPD_READY_POLL_MS * 1000);
    }
    SWSS_LOG_ERROR("stpd not ready after %d ms", timeout_ms);
    return false;
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
        // vlanmgrd adds a member with no tagging_mode as untagged.
        mode = UNTAGGED_MODE;
        for (auto entry : vmEntry)
        {
            // vlanmgrd programs priority_tagged like untagged: the port keeps
            // the PVID and sends untagged, so spanning tree treats it the same.
            if (entry.first == "tagging_mode")
                mode = (entry.second == "untagged" || entry.second == "priority_tagged")
                       ? UNTAGGED_MODE : TAGGED_MODE;
            SWSS_LOG_INFO("mode %d for %s", mode, key.c_str());
        }
    }
    else
        SWSS_LOG_WARN("no CONFIG_DB VLAN_MEMBER row for %s, the member is being removed", key.c_str());

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
                    try
                    {
                        max_stp_instances = (uint16_t)std::stoi(entry.second);
                    }
                    catch (const std::exception &e)
                    {
                        SWSS_LOG_ERROR("stpmgr: invalid max_stp_inst '%s': %s; keeping default",
                                       entry.second.c_str(), e.what());
                    }
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

std::vector<std::string> StpMgr::getVlanAliasesForInstance(uint16_t instance) {
    std::vector<std::string> vlanAliases;

    for (uint16_t vlanId = 0; vlanId < MAX_VLANS; ++vlanId) {
        if (m_vlanInstMap[vlanId] == instance) {
            vlanAliases.push_back("VLAN" + std::to_string(vlanId));
        }
    }

    return vlanAliases;
}

//Function to parse the VLAN list and handle ranges
bool StpMgr::parseVlanList(const std::string &vlanStr, std::vector<uint16_t> &vlanList) {
    vlanList.clear();
    std::stringstream ss(vlanStr);
    std::string segment;

    // Split the string by commas
    while (std::getline(ss, segment, ',')) {
        size_t dashPos = segment.find('-');
        if (dashPos != std::string::npos) {
            // If a dash is found, it's a range like "22-25"
            int start;
            int end;
            if (!parseInteger(segment.substr(0, dashPos), start, "vlan_list range start") ||
                !parseInteger(segment.substr(dashPos + 1), end, "vlan_list range end"))
            {
                vlanList.clear();
                return false;
            }

            // Add all VLANs in the range to the list
            for (int i = start; i <= end; ++i) {
                vlanList.push_back(static_cast<uint16_t>(i));
            }
        } else {
            // Single VLAN, add it to the list
            int vlan;
            if (!parseInteger(segment, vlan, "vlan_list entry"))
            {
                vlanList.clear();
                return false;
            }
            vlanList.push_back(static_cast<uint16_t>(vlan));
        }
    }
    return true;
}

void StpMgr::updateVlanInstanceMap(int instance, const std::vector<uint16_t>& newVlanList, bool operation) {
    if (!operation) {
        // Delete instance: Reset all VLANs mapped to this instance
        for (int vlan = 0; vlan < MAX_VLANS; ++vlan) {
            if (m_vlanInstMap[vlan] == instance) {
                m_vlanInstMap[vlan] = 0; // Reset to default instance
            }
        }
    }
    else {
        // Add/Update instance: Handle additions and deletions
        // Use an unordered_set for efficient lookup of new VLAN list
        std::unordered_set<int> newVlanSet(newVlanList.begin(), newVlanList.end());

        // Iterate over the current mapping to handle deletions
        for (int vlan = 0; vlan < MAX_VLANS; ++vlan) {
            if (m_vlanInstMap[vlan] == instance) {
                // If a VLAN is mapped to this instance but not in the new list, reset it to 0
                if (newVlanSet.find(vlan) == newVlanSet.end()) {
                    m_vlanInstMap[vlan] = 0;
                }
            }
        }

        // Handle additions
        for (int vlan : newVlanList) {
            if (vlan >= 0 && vlan < MAX_VLANS) {
                m_vlanInstMap[vlan] = instance;
            }
        }
    }
}

bool StpMgr::isInstanceMapped(uint16_t instance) { 
    for (int i = 0; i < MAX_VLANS; ++i) {
        if (m_vlanInstMap[i] == static_cast<int>(instance)) {
            return true; // Instance found
        }
    }
    return false; // Instance not, found
}
