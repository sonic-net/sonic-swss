#include <fstream>
#include <iostream>
#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "timer.h"
#include "buffermgrdyn.h"
#include "bufferorch.h"
#include "exec.h"
#include "shellcmd.h"
#include "schema.h"
#include "warm_restart.h"

/*
 * Some Tips
 * 1. All keys in this file are in format of APPL_DB key.
 *    Key population:
 *        On receiving item update from CONFIG_DB: key has been transformed into the format of APPL_DB
 *        In intermal maps: table name removed from the index
 * 2. Maintain maps for pools, profiles and PGs in CONFIG_DB and APPL_DB
 * 3. Keys of maps in this file don't contain the TABLE_NAME
 * 3. 
 */
using namespace std;
using namespace swss;

BufferMgrDynamic::BufferMgrDynamic(DBConnector *cfgDb, DBConnector *stateDb, DBConnector *applDb, const vector<TableConnector> &tables, shared_ptr<vector<KeyOpFieldsValuesTuple>> gearboxInfo = nullptr) :
        Orch(tables),
        m_applDb(applDb),
        m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME),
        m_cfgCableLenTable(cfgDb, CFG_PORT_CABLE_LEN_TABLE_NAME),
        m_cfgBufferProfileTable(cfgDb, CFG_BUFFER_PROFILE_TABLE_NAME),
        m_cfgBufferPgTable(cfgDb, CFG_BUFFER_PG_TABLE_NAME),
        m_cfgLosslessPgPoolTable(cfgDb, CFG_BUFFER_POOL_TABLE_NAME),
        m_cfgDefaultLosslessBufferParam(cfgDb, CFG_DEFAULT_LOSSLESS_BUFFER_PARAMETER),
        m_applBufferPoolTable(applDb, APP_BUFFER_POOL_TABLE_NAME),
        m_applBufferProfileTable(applDb, APP_BUFFER_PROFILE_TABLE_NAME),
        m_applBufferPgTable(applDb, APP_BUFFER_PG_TABLE_NAME),
        m_applBufferQueueTable(applDb, APP_BUFFER_QUEUE_TABLE_NAME),
        m_applBufferIngressProfileListTable(applDb, APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME),
        m_applBufferEgressProfileListTable(applDb, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME),
        m_stateBufferMaximumTable(stateDb, STATE_BUFFER_MAXIMUM_VALUE_TABLE),
        m_stateBufferPoolTable(stateDb, STATE_BUFFER_POOL_TABLE_NAME),
        m_stateBufferProfileTable(stateDb, STATE_BUFFER_PROFILE_TABLE_NAME),
        m_applPortTable(applDb, APP_PORT_TABLE_NAME),
        m_portInitDone(false),
        m_firstTimeCalculateBufferPool(true),
        m_mmuSizeNumber(0)
{
    SWSS_LOG_ENTER();

    // Initialize the handler map
    initTableHandlerMap();
    parseGearboxInfo(gearboxInfo);

    string platform = getenv("ASIC_VENDOR") ? getenv("ASIC_VENDOR") : "";
    if (platform == "")
    {
        SWSS_LOG_ERROR("Platform environment variable is not defined, buffermgrd won't start");
        return;
    }

    string headroomSha, bufferpoolSha;
    string headroomPluginName = "buffer_headroom_" + platform + ".lua";
    string bufferpoolPluginName = "buffer_pool_" + platform + ".lua";
    string checkHeadroomPluginName = "buffer_check_headroom_" + platform + ".lua";

    try
    {
        string headroomLuaScript = swss::loadLuaScript(headroomPluginName);
        m_headroomSha = swss::loadRedisScript(applDb, headroomLuaScript);

        string bufferpoolLuaScript = swss::loadLuaScript(bufferpoolPluginName);
        m_bufferpoolSha = swss::loadRedisScript(applDb, bufferpoolLuaScript);

        string checkHeadroomLuaScript = swss::loadLuaScript(checkHeadroomPluginName);
        m_checkHeadroomSha = swss::loadRedisScript(applDb, checkHeadroomLuaScript);
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Lua scripts for buffer calculation were not loaded successfully, buffermgrd won't start");
        return;
    }

    // Init timer
    auto interv = timespec { .tv_sec = BUFFERMGR_TIMER_PERIOD, .tv_nsec = 0 };
    m_buffermgrPeriodtimer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(m_buffermgrPeriodtimer, this, "PORT_INIT_DONE_POLL_TIMER");
    Orch::addExecutor(executor);
    m_buffermgrPeriodtimer->start();

    // Try fetch mmu size from STATE_DB
    // - warm-reboot, the mmuSize should be in the STATE_DB,
    //   which is done by not removing it from STATE_DB before warm reboot
    // - warm-reboot for the first time or cold-reboot, the mmuSize is
    //   fetched from SAI and then pushed into STATE_DB by orchagent
    // This is to accelerate the process of inserting all the buffer pools
    // into APPL_DB when the system starts
    // In case that the mmuSize isn't available yet at time buffermgrd starts,
    // the buffer_pool_<vendor>.lua should try to fetch that from BUFFER_POOL
    m_stateBufferMaximumTable.hget("global", "mmu_size", m_mmuSize);
    if (!m_mmuSize.empty())
    {
        m_mmuSizeNumber = atol(m_mmuSize.c_str());
    }

    // Try fetch default dynamic_th from CONFIG_DB
    vector<string> keys;
    m_cfgDefaultLosslessBufferParam.getKeys(keys);
    if (!keys.empty())
    {
        m_cfgDefaultLosslessBufferParam.hget(keys[0], "default_dynamic_th", m_defaultThreshold);
    }
}

void BufferMgrDynamic::parseGearboxInfo(shared_ptr<vector<KeyOpFieldsValuesTuple>> gearboxInfo)
{
    if (nullptr == gearboxInfo)
    {
        m_supportGearbox = false;
    }
    else
    {
        string gearboxModel;
        for (auto &kfv : *gearboxInfo)
        {
            auto table = parseObjectNameFromKey(kfvKey(kfv), 0);
            auto key = parseObjectNameFromKey(kfvKey(kfv), 1);

            if (table.empty() || key.empty())
            {
                SWSS_LOG_ERROR("Invalid format of key %s for gearbox info, won't initialize it",
                               kfvKey(kfv).c_str());
                return;
            }

            if (table == STATE_PERIPHERAL_TABLE)
            {
                for (auto &fv: kfvFieldsValues(kfv))
                {
                    auto &field = fvField(fv);
                    auto &value = fvValue(fv);
                    SWSS_LOG_DEBUG("Processing table %s field:%s, value:%s", table.c_str(), field.c_str(), value.c_str());
                    if (field == "gearbox_delay")
                        m_gearboxDelay[key] = value;
                }
            }

            if (table == STATE_PORT_PERIPHERAL_TABLE)
            {
                if (key != "global")
                {
                    SWSS_LOG_ERROR("Port peripheral table: only global gearbox model is supported but got %s", key.c_str());
                    continue;
                }

                for (auto &fv: kfvFieldsValues(kfv))
                {
                    auto &field = fvField(fv);
                    auto &value = fvValue(fv);
                    SWSS_LOG_DEBUG("Processing table %s field:%s, value:%s", table.c_str(), field.c_str(), value.c_str());
                    if (fvField(fv) == "gearbox_model")
                        gearboxModel = fvValue(fv);
                }
            }
        }

        m_identifyGearboxDelay = m_gearboxDelay[gearboxModel];
        m_supportGearbox = false;
    }
}

void BufferMgrDynamic::initTableHandlerMap()
{
    m_bufferTableHandlerMap.insert(buffer_handler_pair(STATE_BUFFER_MAXIMUM_VALUE_TABLE, &BufferMgrDynamic::handleBufferMaxParam));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_DEFAULT_LOSSLESS_BUFFER_PARAMETER, &BufferMgrDynamic::handleDefaultLossLessBufferParam));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_POOL_TABLE_NAME, &BufferMgrDynamic::handleBufferPoolTable));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PROFILE_TABLE_NAME, &BufferMgrDynamic::handleBufferProfileTable));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_QUEUE_TABLE_NAME, &BufferMgrDynamic::handleBufferQueueTable));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PG_TABLE_NAME, &BufferMgrDynamic::handleBufferPgTable));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, &BufferMgrDynamic::handleBufferPortIngressProfileListTable));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, &BufferMgrDynamic::handleBufferPortEgressProfileListTable));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_PORT_TABLE_NAME, &BufferMgrDynamic::handlePortTable));
    m_bufferTableHandlerMap.insert(buffer_handler_pair(CFG_PORT_CABLE_LEN_TABLE_NAME, &BufferMgrDynamic::handleCableLenTable));
}

// APIs to handle variant kinds of keys

// Transform key from CONFIG_DB format to APPL_DB format
void BufferMgrDynamic::transformSeperator(string &name)
{
    size_t pos;
    while ((pos = name.find("|")) != string::npos)
        name.replace(pos, 1, ":");
}

void BufferMgrDynamic::transformReference(string &name)
{
    auto references = tokenize(name, list_item_delimiter);
    int ref_index = 0;

    name = "";

    for (auto &reference : references)
    {
        if (ref_index != 0)
            name += list_item_delimiter;
        ref_index ++;

        auto keys = tokenize(reference, config_db_key_delimiter);
        int key_index = 0;
        for (auto &key : keys)
        {
            if (key_index == 0)
                name += key + "_TABLE";
            else
                name += delimiter + key;
            key_index ++;
        }
    }
}

// For string "TABLE_NAME|objectname", returns "objectname"
string BufferMgrDynamic::parseObjectNameFromKey(const string &key, size_t pos = 0)
{
    auto keys = tokenize(key, delimiter);
    if (pos >= keys.size())
    {
        SWSS_LOG_ERROR("Failed to fetch %zu-th sector of key %s", pos, key.c_str());
        return string();
    }
    return keys[pos];
}

// For string "[foo]", returns "foo"
string BufferMgrDynamic::parseObjectNameFromReference(const string &reference)
{
    auto objName = reference.substr(1, reference.size() - 2);
    return parseObjectNameFromKey(objName, 1);
}

string BufferMgrDynamic::getDynamicProfileName(const string &speed, const string &cable, const string &mtu, const string &threshold, const string &gearbox_model)
{
    string buffer_profile_key;

    if (mtu == DEFAULT_MTU_STR)
    {
        buffer_profile_key = "pg_lossless_" + speed + "_" + cable;
    }
    else
    {
        buffer_profile_key = "pg_lossless_" + speed + "_" + cable + "_mtu" + mtu;
    }

    if (threshold != m_defaultThreshold)
    {
        buffer_profile_key = buffer_profile_key + "_th" + threshold;
    }

    if (!gearbox_model.empty())
    {
        buffer_profile_key = buffer_profile_key + "_" + gearbox_model;
    }

    return buffer_profile_key + "_profile";
}

string BufferMgrDynamic::getPgPoolMode()
{
    return m_bufferPoolLookup[INGRESS_LOSSLESS_PG_POOL_NAME].mode;
}

// Meta flows which are called by main flows
void BufferMgrDynamic::calculateHeadroomSize(const string &speed, const string &cable, const string &port_mtu, const string &gearbox_model, buffer_profile_t &headroom)
{
    // Call vendor-specific lua plugin to calculate the xon, xoff, xon_offset, size and threshold
    vector<string> keys = {};
    vector<string> argv = {};

    keys.emplace_back(headroom.name);
    argv.emplace_back(speed);
    argv.emplace_back(cable);
    argv.emplace_back(port_mtu);
    argv.emplace_back(m_identifyGearboxDelay);

    try
    {
        auto ret = swss::runRedisScript(*m_applDb, m_headroomSha, keys, argv);

        // The format of the result:
        // a list of strings containing key, value pairs with colon as separator
        // each is a field of the profile
        // "xon:18432"
        // "xoff:18432"
        // "size:36864"

        for ( auto i : ret)
        {
            auto pairs = tokenize(i, ':');
            if (pairs[0] == "xon")
                headroom.xon = pairs[1];
            if (pairs[0] == "xoff")
                headroom.xoff = pairs[1];
            if (pairs[0] == "size")
                headroom.size = pairs[1];
            if (pairs[0] == "xon_offset")
                headroom.xon_offset = pairs[1];
        }
    }
    catch (...)
    {
        SWSS_LOG_WARN("Lua scripts for headroom calculation were not executed successfully");
    }
}

void BufferMgrDynamic::recalculateSharedBufferPool()
{
    try
    {
        vector<string> keys = {};
        vector<string> argv = {};

        auto ret = runRedisScript(*m_applDb, m_bufferpoolSha, keys, argv);

        // The format of the result:
        // a list of strings containing key, value pairs with colon as separator
        // each is the size of a buffer pool

        for ( auto i : ret)
        {
            auto pairs = tokenize(i, ':');
            auto poolName = pairs[0];

            if ("debug" != poolName)
            {
                auto &pool = m_bufferPoolLookup[pairs[0]];

                if (pool.total_size == pairs[1])
                    continue;

                auto &poolSizeStr = pairs[1];
                unsigned long poolSizeNum = atol(poolSizeStr.c_str());
                if (m_mmuSizeNumber > 0 && m_mmuSizeNumber < poolSizeNum)
                {
                    SWSS_LOG_ERROR("Buffer pool %s: Invalid size %s, exceeding the mmu size %s",
                                   poolName.c_str(), poolSizeStr.c_str(), m_mmuSize.c_str());
                    continue;
                }

                pool.total_size = poolSizeStr;
                updateBufferPoolToDb(poolName, pool);

                SWSS_LOG_NOTICE("Buffer pool %s had been updated with new size [%s]", poolName.c_str(), pool.total_size.c_str());
            }
            else
            {
                SWSS_LOG_INFO("Buffer pool debug info %s", i.c_str());
            }
        }
    }
    catch (...)
    {
        SWSS_LOG_WARN("Lua scripts for buffer calculation were not executed successfully");
    }
}

void BufferMgrDynamic::checkSharedBufferPoolSize()
{
    // PortInitDone indicates all steps of port initialization has been done
    // Only after that does the buffer pool size update starts
    if (!m_portInitDone)
    {
        vector<FieldValueTuple> values;
        if (m_applPortTable.get("PortInitDone", values))
        {
            SWSS_LOG_NOTICE("Buffer pools start to be updated");
            m_portInitDone = true;
        }
        else
        {
            if (m_firstTimeCalculateBufferPool)
            {
                // It's something like a placeholder especially for warm reboot flow
                // without all buffer pools created, buffer profiles are unable to be cureated,
                // which in turn causes buffer pgs and buffer queues unable to be created,
                // which prevents the port from being ready and eventually fails the warm reboot
                // After the buffer pools are created for the first time, we won't touch it
                // until portInitDone
                // Eventually, the correct values will pushed to APPL_DB and then ASIC_DB
                recalculateSharedBufferPool();
                m_firstTimeCalculateBufferPool = false;
                SWSS_LOG_NOTICE("Buffer pool update defered because port is still under initialization, start polling timer");
            }

            return;
        }
    }

    if (!m_mmuSize.empty())
        recalculateSharedBufferPool();
}

// For buffer pool, only size can be updated on-the-fly
void BufferMgrDynamic::updateBufferPoolToDb(const string &name, const buffer_pool_t &pool)
{
    vector<FieldValueTuple> fvVector;

    if (pool.ingress)
        fvVector.emplace_back(make_pair("type", "ingress"));
    else
        fvVector.emplace_back(make_pair("type", "egress"));

    fvVector.emplace_back(make_pair("mode", pool.mode));

    SWSS_LOG_INFO("Buffer pool %s is initialized", name.c_str());

    fvVector.emplace_back(make_pair("size", pool.total_size));

    m_applBufferPoolTable.set(name, fvVector);

    m_stateBufferPoolTable.set(name, fvVector);
}

void BufferMgrDynamic::updateBufferProfileToDb(const string &name, const buffer_profile_t &profile)
{
    vector<FieldValueTuple> fvVector;
    string mode = getPgPoolMode();

    // profile threshold field name
    mode += "_th";
    string pg_pool_reference = string(APP_BUFFER_POOL_TABLE_NAME) +
        m_applBufferProfileTable.getTableNameSeparator() +
        INGRESS_LOSSLESS_PG_POOL_NAME;

    fvVector.emplace_back(make_pair("xon", profile.xon));
    if (!profile.xon_offset.empty()) {
        fvVector.emplace_back(make_pair("xon_offset", profile.xon_offset));
    }
    fvVector.emplace_back(make_pair("xoff", profile.xoff));
    fvVector.emplace_back(make_pair("size", profile.size));
    fvVector.emplace_back(make_pair("pool", "[" + pg_pool_reference + "]"));
    fvVector.emplace_back(make_pair(mode, profile.threshold));

    m_applBufferProfileTable.set(name, fvVector);
    m_stateBufferProfileTable.set(name, fvVector);
}

// Database operation
// Set/remove BUFFER_PG table entry
void BufferMgrDynamic::updateBufferPgToDb(const string &key, const string &profile, bool add)
{
    if (add)
    {
        vector<FieldValueTuple> fvVector;

        fvVector.clear();

        string profile_ref = string("[") +
            APP_BUFFER_PROFILE_TABLE_NAME +
            m_applBufferPgTable.getTableNameSeparator() +
            profile +
            "]";

        fvVector.clear();
 
        fvVector.push_back(make_pair("profile", profile_ref));
        m_applBufferPgTable.set(key, fvVector);
    }
    else
    {
        m_applBufferPgTable.del(key);
    }
}

// We have to check the headroom ahead of applying them
task_process_status BufferMgrDynamic::allocateProfile(const string &speed, const string &cable, const string &mtu, const string &threshold, const string &gearbox_model, string &profile_name)
{
    // Create record in BUFFER_PROFILE table

    SWSS_LOG_INFO("Allocating new BUFFER_PROFILE %s", profile_name.c_str());

    // check if profile already exists - if yes - skip creation
    auto profileRef = m_bufferProfileLookup.find(profile_name);
    if (profileRef == m_bufferProfileLookup.end())
    {
        auto &profile = m_bufferProfileLookup[profile_name];
        SWSS_LOG_NOTICE("Creating new profile '%s'", profile_name.c_str());

        string mode = getPgPoolMode();
        if (mode.empty())
        {
            SWSS_LOG_NOTICE("BUFFER_PROFILE %s cannot be created because the buffer pool isn't ready", profile_name.c_str());
            return task_process_status::task_need_retry;
        }

        // Call vendor-specific lua plugin to calculate the xon, xoff, xon_offset, size
        // Pay attention, the threshold can contain valid value
        calculateHeadroomSize(speed, cable, mtu, gearbox_model, profile);

        profile.threshold = threshold;
        profile.dynamic_calculated = true;
        profile.static_configured = false;
        profile.lossless = true;
        profile.name = profile_name;
        profile.state = PROFILE_NORMAL;

        updateBufferProfileToDb(profile_name, profile);

        SWSS_LOG_NOTICE("BUFFER_PROFILE %s has been created successfully", profile_name.c_str());
        SWSS_LOG_DEBUG("New profile created %s according to (%s %s %s): xon %s xoff %s size %s",
                       profile_name.c_str(),
                       speed.c_str(), cable.c_str(), gearbox_model.c_str(),
                       profile.xon.c_str(), profile.xoff.c_str(), profile.size.c_str());
    }
    else
    {
        SWSS_LOG_NOTICE("Reusing existing profile '%s'", profile_name.c_str());
    }

    return task_process_status::task_success;
}

void BufferMgrDynamic::releaseProfile(const string &profile_name)
{
    // Crete record in BUFFER_PROFILE table
    // key format is pg_lossless_<speed>_<cable>_profile
    vector<FieldValueTuple> fvVector;
    auto &profile = m_bufferProfileLookup[profile_name];

    if (profile.static_configured)
    {
        // Check whether the profile is statically configured.
        // This means:
        // 1. It's a statically configured profile, headroom override
        // 2. It's dynamically calculated headroom with static threshold (alpha)
        // In this case we won't remove the entry from the local cache even if it's dynamically calculated
        // because the speed, cable length and cable model are fixed the headroom info will always be valid once calculated.
        SWSS_LOG_NOTICE("Unable to release profile %s because it's a static configured profile", profile_name.c_str());
        return;
    }

    // Check whether it's referenced anymore by other PGs.
    if (!profile.port_pgs.empty())
    {
        for (auto &pg : profile.port_pgs)
        {
            SWSS_LOG_INFO("Unable to release profile %s because it's still referenced by %s (and others)",
                          profile_name.c_str(), pg.c_str());
            return;
        }
    }

    profile.port_pgs.clear();

    m_applBufferProfileTable.del(profile_name);

    m_stateBufferProfileTable.del(profile_name);

    m_bufferProfileLookup.erase(profile_name);

    SWSS_LOG_NOTICE("BUFFER_PROFILE %s has been released successfully", profile_name.c_str());
}

bool BufferMgrDynamic::isHeadroomResourceValid(const string &port, const buffer_profile_t &profile, const string &new_pg = "")
{
    //port: used to fetch the maximum headroom size
    //profile: the profile referenced by the new_pg (if provided) or all PGs
    //new_pg: which pg is newly added?

    if (!profile.lossless)
    {
        SWSS_LOG_INFO("No need to check headroom for lossy PG port %s profile %s size %s pg %s",
                  port.c_str(), profile.name.c_str(), profile.size.c_str(), new_pg.c_str());
        return true;
    }

    bool result = true;

    vector<string> keys = {port};
    vector<string> argv = {};

    argv.emplace_back(profile.name);
    argv.emplace_back(profile.size);

    if (!new_pg.empty())
    {
        argv.emplace_back(new_pg);
    }

    SWSS_LOG_INFO("Checking headroom for port %s with profile %s size %s pg %s",
                  port.c_str(), profile.name.c_str(), profile.size.c_str(), new_pg.c_str());

    try
    {
        auto ret = runRedisScript(*m_applDb, m_checkHeadroomSha, keys, argv);

        // The format of the result:
        // a list of strings containing key, value pairs with colon as separator
        // each is the size of a buffer pool

        for ( auto i : ret)
        {
            auto pairs = tokenize(i, ':');
            if ("result" == pairs[0])
            {
                if ("true" != pairs[1])
                {
                    SWSS_LOG_ERROR("Unable to update profile for port %s. Accumulative headroom size exceeds limit", port.c_str());
                    result = false;
                }
                else
                {
                    result = true;
                }
            }
            else if ("debug" == pairs[0])
            {
                SWSS_LOG_INFO("Buffer headroom checking debug info %s", i.c_str());
            }
        }
    }
    catch (...)
    {
        SWSS_LOG_WARN("Lua scripts for buffer calculation were not executed successfully");
    }

    return result;
}

//Called when speed/cable length updated from CONFIG_DB
// Update buffer profile of a certern PG of a port or all PGs of the port according to its speed, cable_length and mtu
// Called when
//    - port's speed, cable_length or mtu updated
//    - one buffer pg of port's is set to dynamic calculation
// Args
//    port - port name
//    speed, cable_length, mtu - port info
//    exactly_matched_key - representing a port,pg. when provided, only profile of this key is updated
// Flow
// 1. For each PGs in the port
//    a. skip non-dynamically-calculated or non-exactly-matched PGs
//    b. allocate/reuse profile according to speed/cable length/mtu
//    c. check accumulative headroom size, fail if exceeding per-port limit
//    d. profile reference: remove reference to old profile and add reference to new profile
//    e. put the old profile to to-be-released profile set
//    f. update BUFFER_PG database
// 2. Update port's info: speed, cable length and mtu
// 3. If any of the PGs is updated, recalculate pool size
// 4. try release each profile in to-be-released profile set
task_process_status BufferMgrDynamic::refreshPriorityGroupsForPort(const string &port, const string &speed, const string &cable_length, const string &mtu, const string &exactly_matched_key = "")
{
    port_info_t &portInfo = m_portInfoLookup[port];
    string &gearbox_model = portInfo.gearbox_model;
    bool isHeadroomUpdated = false;
    buffer_pg_lookup_t &portPgs = m_portPgLookup[port];
    set<string> profilesToBeReleased;

    // Iterate all the lossless PGs configured on this port
    for (auto it = portPgs.begin(); it != portPgs.end(); ++it)
    {
        auto &key = it->first;
        if (!exactly_matched_key.empty() && key != exactly_matched_key)
        {
            SWSS_LOG_DEBUG("Update buffer PGs: key %s doesn't match %s, skipped ", key.c_str(), exactly_matched_key.c_str());
            continue;
        }
        auto &portPg = it->second;
        string newProfile, oldProfile;

        oldProfile = portPg.running_profile_name;
        if (!oldProfile.empty())
        {
            // Clear old profile
            portPg.running_profile_name = "";
        }

        if (portPg.dynamic_calculated)
        {
            string threshold;
            //Calculate new headroom size
            if (portPg.static_configured)
            {
                // static_configured but dynamic_calculated means non-default threshold value
                auto &profile = m_bufferProfileLookup[portPg.configured_profile_name];
                threshold = profile.threshold;
            }
            else
            {
                threshold = m_defaultThreshold;
            }
            newProfile = getDynamicProfileName(speed, cable_length, mtu, threshold, gearbox_model);
            auto rc = allocateProfile(speed, cable_length, mtu, threshold, gearbox_model, newProfile);
            if (task_process_status::task_success != rc)
                return rc;

            SWSS_LOG_DEBUG("Handling PG %s port %s, for dynamically calculated profile %s", key.c_str(), port.c_str(), newProfile.c_str());
        }
        else
        {
            newProfile = portPg.configured_profile_name;

            SWSS_LOG_DEBUG("Handling PG %s port %s, for static profile %s", key.c_str(), port.c_str(), newProfile.c_str());
        }

        //Calculate whether accumulative headroom size exceeds the maximum value
        //Abort if it does
        if (!isHeadroomResourceValid(port, m_bufferProfileLookup[newProfile], exactly_matched_key))
        {
            SWSS_LOG_ERROR("Update speed (%s) and cable length (%s) for port %s failed, accumulative headroom size exceeds the limit",
                           speed.c_str(), cable_length.c_str(), port.c_str());

            releaseProfile(newProfile);

            return task_process_status::task_failed;
        }

        if (newProfile != oldProfile)
        {
            // Need to remove the reference to the old profile
            // and create the reference to the new one
            m_bufferProfileLookup[oldProfile].port_pgs.erase(key);
            m_bufferProfileLookup[newProfile].port_pgs.insert(key);
            SWSS_LOG_INFO("Move profile reference for %s from [%s] to [%s]", key.c_str(), oldProfile.c_str(), newProfile.c_str());

            // buffer pg needs to be updated as well
            portPg.running_profile_name = newProfile;

            // Add the old profile to "to be removed" set
            if (!oldProfile.empty())
                profilesToBeReleased.insert(oldProfile);
        }

        //appl_db Database operation: set item BUFFER_PG|<port>|<pg>
        updateBufferPgToDb(key, newProfile, true);
        isHeadroomUpdated = true;
    }

    portInfo.speed = speed;
    portInfo.cable_length = cable_length;
    portInfo.gearbox_model = gearbox_model;

    if (isHeadroomUpdated)
    {
        checkSharedBufferPoolSize();
    }
    else
    {
        SWSS_LOG_DEBUG("Nothing to do for port %s since no PG configured on it", port.c_str());
    }

    portInfo.state = PORT_READY;

    //Remove the old profile which is probably not referenced anymore.
    //TODO release all profiles in to-be-removed map
    if (!profilesToBeReleased.empty())
    {
        for (auto &oldProfile : profilesToBeReleased)
        {
            releaseProfile(oldProfile);
        }
    }

    return task_process_status::task_success;
}

// Main flows

// Update lossless pg on a port after an PG has been installed on the port
// Called when pg updated from CONFIG_DB
// Key format: BUFFER_PG:<port>:<pg>
task_process_status BufferMgrDynamic::doUpdatePgTask(const string &pg_key, const string &port)
{
    string value;
    port_info_t &portInfo = m_portInfoLookup[port];
    auto &bufferPg = m_portPgLookup[port][pg_key];
    task_process_status task_status = task_process_status::task_success;

    switch (portInfo.state)
    {
    case PORT_READY:
        // Not having profile_name but both speed and cable length have been configured for that port
        // This is because the first PG on that port is configured after speed, cable length configured
        // Just regenerate the profile
        task_status = refreshPriorityGroupsForPort(port, portInfo.speed, portInfo.cable_length, portInfo.mtu, pg_key);
        if (task_status != task_process_status::task_success)
            return task_status;

        break;

    case PORT_INITIALIZING:
        if (bufferPg.dynamic_calculated)
        {
            SWSS_LOG_NOTICE("Skip setting BUFFER_PG for %s because port's info isn't ready for dynamic calculation", pg_key.c_str());
        }
        else
        {
            task_status = refreshPriorityGroupsForPort(port, portInfo.speed, portInfo.cable_length, portInfo.mtu, pg_key);
            if (task_status != task_process_status::task_success)
                return task_status;
        }
        break;

    default:
        // speed and cable length hasn't been configured
        // In that case, we just skip the this update and return success.
        // It will be handled after speed and cable length configured.
        SWSS_LOG_NOTICE("Skip setting BUFFER_PG for %s because port's info isn't ready for dynamic calculation", pg_key.c_str());
        return task_process_status::task_success;
    }

    if (bufferPg.static_configured && bufferPg.dynamic_calculated)
    {
        auto &profile = m_bufferProfileLookup[bufferPg.configured_profile_name];
        profile.port_pgs.insert(pg_key);
    }

    return task_process_status::task_success;
}

//Remove the currently configured lossless pg
task_process_status BufferMgrDynamic::doRemovePgTask(const string &pg_key, const string &port)
{
    auto &bufferPgs = m_portPgLookup[port];
    buffer_pg_t &bufferPg = bufferPgs[pg_key];
    port_info_t &portInfo = m_portInfoLookup[port];

    // Remove the PG from APPL_DB
    string null_str("");
    updateBufferPgToDb(pg_key, null_str, false);

    SWSS_LOG_NOTICE("Remove BUFFER_PG %s (profile %s, %s)", pg_key.c_str(), bufferPg.running_profile_name.c_str(), bufferPg.configured_profile_name.c_str());

    // recalculate pool size
    checkSharedBufferPoolSize();

    if (!portInfo.speed.empty() && !portInfo.cable_length.empty())
        portInfo.state = PORT_READY;
    else
        portInfo.state = PORT_INITIALIZING;
    SWSS_LOG_NOTICE("try removing the original profile %s", bufferPg.running_profile_name.c_str());
    releaseProfile(bufferPg.running_profile_name);

    if (bufferPg.static_configured && bufferPg.dynamic_calculated)
    {
        auto &profile = m_bufferProfileLookup[bufferPg.configured_profile_name];
        profile.port_pgs.erase(pg_key);
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::doUpdateStaticProfileTask(buffer_profile_t &profile)
{
    const string &profileName = profile.name;
    auto &profileToMap = profile.port_pgs;
    set<string> portsChecked;

    if (profile.dynamic_calculated)
    {
        for (auto &key : profileToMap)
        {
            auto portName = parseObjectNameFromKey(key);
            auto &port = m_portInfoLookup[portName];
            task_process_status rc;

            if (portsChecked.find(portName) != portsChecked.end())
                continue;

            SWSS_LOG_DEBUG("Checking PG %s for dynamic profile %s", key.c_str(), profileName.c_str());
            portsChecked.insert(portName);

            rc = refreshPriorityGroupsForPort(portName, port.speed, port.cable_length, port.mtu);
            if (task_process_status::task_success != rc)
            {
                SWSS_LOG_ERROR("Update the profile on %s failed", key.c_str());
                return rc;
            }
        }
    }
    else
    {
        for (auto &key : profileToMap)
        {
            auto port = parseObjectNameFromKey(key);

            if (portsChecked.find(port) != portsChecked.end())
                continue;

            SWSS_LOG_DEBUG("Checking PG %s for profile %s", key.c_str(), profileName.c_str());

            if (!isHeadroomResourceValid(port, profile))
            {
                // to do: get the value from application database
                SWSS_LOG_ERROR("BUFFER_PROFILE %s cannot be updated because %s referencing it violates the resource limitation",
                               profileName.c_str(), key.c_str());
                return task_process_status::task_failed;
            }

            portsChecked.insert(port);
        }

        updateBufferProfileToDb(profileName, profile);
    }

    checkSharedBufferPoolSize();

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleBufferMaxParam(KeyOpFieldsValuesTuple &tuple)
{
    string op = kfvOp(tuple);

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "mmu_size")
            {
                m_mmuSize = fvValue(i);
                if (!m_mmuSize.empty())
                {
                    m_mmuSizeNumber = atol(m_mmuSize.c_str());
                }
                if (0 == m_mmuSizeNumber)
                {
                    SWSS_LOG_ERROR("BUFFER_MAX_PARAM: Got invalid mmu size %s", m_mmuSize.c_str());
                    return task_process_status::task_failed;
                }
                SWSS_LOG_DEBUG("Handling Default Lossless Buffer Param table field mmu_size %s", m_mmuSize.c_str());
            }
        }
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleDefaultLossLessBufferParam(KeyOpFieldsValuesTuple &tuple)
{
    string op = kfvOp(tuple);

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "default_dynamic_th")
            {
                m_defaultThreshold = fvValue(i);
                SWSS_LOG_DEBUG("Handling Buffer Maximum value table field default_dynamic_th value %s", m_defaultThreshold.c_str());
            }
        }
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleCableLenTable(KeyOpFieldsValuesTuple &tuple)
{
    string op = kfvOp(tuple);

    task_process_status task_status = task_process_status::task_success;
    int failed_item_count = 0;
    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            // receive and cache cable length table
            auto &port = fvField(i);
            auto &cable_length = fvValue(i);
            port_info_t &portInfo = m_portInfoLookup[port];
            string &speed = portInfo.speed;
            string &mtu = portInfo.mtu;

            SWSS_LOG_DEBUG("Handling CABLE_LENGTH table field %s length %s", port.c_str(), cable_length.c_str());
            SWSS_LOG_DEBUG("Port Info for %s before handling %s %s %s",
                           port.c_str(),
                           portInfo.speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());

            if (portInfo.cable_length == cable_length)
            {
                continue;
            }

            portInfo.cable_length = cable_length;
            if (speed.empty())
            {
                SWSS_LOG_WARN("Speed for %s hasn't been configured yet, unable to calculate headroom", port.c_str());
                // We don't retry here because it doesn't make sense until the speed is configured.
                continue;
            }

            if (mtu.empty())
            {
                // During initialization, the flow can be
                // 1. mtu, cable_length, speed
                // 2. cable_length, speed, mtu, or speed, cable_length, mtu
                // 3. cable_length, speed, but no mtu specified, or speed, cable_length but no mtu
                // it's ok for the 1st case
                // but for the 2nd case, we can't wait mtu for calculating the headrooms
                // because we can't distinguish case 2 from case 3 which is also legal.
                // So once we have speed updated, let's try to calculate the headroom with default mtu.
                // The cost is that if the mtu is provided in the following iteration
                // the current calculation is of no value and will be replaced.
                mtu = DEFAULT_MTU_STR;
            }

            SWSS_LOG_INFO("Updating BUFFER_PG for port %s due to cable length updated", port.c_str());

            //Try updating the buffer information
            switch (portInfo.state)
            {
            case PORT_INITIALIZING:
                portInfo.state = PORT_READY;
                task_status = refreshPriorityGroupsForPort(port, speed, cable_length, mtu);
                break;

            case PORT_READY:
                task_status = refreshPriorityGroupsForPort(port, speed, cable_length, mtu);
                break;
            }

            switch (task_status)
            {
            case task_process_status::task_need_retry:
                return task_status;
            case task_process_status::task_failed:
                // We shouldn't return directly here. Because doing so will cause the following items lost
                failed_item_count++;
                break;
            default:
                break;
            }

            SWSS_LOG_DEBUG("Port Info for %s after handling speed %s cable %s gb %s",
                           port.c_str(),
                           portInfo.speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());
        }
    }

    if (failed_item_count > 0)
    {
        return task_process_status::task_failed;
    }

    return task_process_status::task_success;
}

// A tiny state machine is required for handling the events
// flags:
//      speed_updated
//      mtu_updated
//      admin_status_updated
// flow:
// 1. handle all events in order, record new value if necessary
// 2. if cable length or speed hasn't been configured, return success
// 3. if mtu isn't configured, take the default value
// 4. if speed_updated or mtu_updated, update headroom size
//    elif admin_status_updated, update buffer pool size
task_process_status BufferMgrDynamic::handlePortTable(KeyOpFieldsValuesTuple &tuple)
{
    auto &port = kfvKey(tuple);
    string op = kfvOp(tuple);
    bool speed_updated = false, mtu_updated = false, admin_status_updated = false;

    SWSS_LOG_DEBUG("processing command:%s PORT table key %s", op.c_str(), port.c_str());

    port_info_t &portInfo = m_portInfoLookup[port];

    SWSS_LOG_DEBUG("Port Info for %s before handling %s %s %s",
                   port.c_str(),
                   portInfo.speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());

    task_process_status task_status = task_process_status::task_success;

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "speed")
            {
                speed_updated = true;
                portInfo.speed = fvValue(i);
            }
            else if (fvField(i) == "mtu")
            {
                mtu_updated = true;
                portInfo.mtu = fvValue(i);
            }
            else if (fvField(i) == "admin_status")
            {
                admin_status_updated = true;
            }
        }

        string &cable_length = portInfo.cable_length;
        string &mtu = portInfo.mtu;
        string &speed = portInfo.speed;

        if (cable_length.empty() || speed.empty())
        {
            SWSS_LOG_WARN("Cable length for %s hasn't been configured yet, unable to calculate headroom", port.c_str());
            // We don't retry here because it doesn't make sense until the cable length is configured.
            return task_process_status::task_success;
        }

        if (speed_updated || mtu_updated)
        {
            SWSS_LOG_INFO("Updating BUFFER_PG for port %s due to speed or port updated", port.c_str());

            //Try updating the buffer information
            switch (portInfo.state)
            {
            case PORT_INITIALIZING:
                portInfo.state = PORT_READY;
                if (mtu.empty())
                {
                    // It's the same case as that in handleCableLenTable
                    mtu = DEFAULT_MTU_STR;
                }
                task_status = refreshPriorityGroupsForPort(port, speed, cable_length, mtu);
                break;

            case PORT_READY:
                task_status = refreshPriorityGroupsForPort(port, speed, cable_length, mtu);
                break;
            }

            SWSS_LOG_DEBUG("Port Info for %s after handling speed %s cable %s gb %s",
                           port.c_str(),
                           portInfo.speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());
        }
        else if (admin_status_updated)
        {
            SWSS_LOG_INFO("Recalculate shared buffer pool size due to port %s's admin_status updated", port.c_str());
            checkSharedBufferPoolSize();
        }
    }

    return task_status;
}

task_process_status BufferMgrDynamic::handleBufferPoolTable(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    string &pool = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<FieldValueTuple> fvVector;

    SWSS_LOG_DEBUG("processing command:%s table BUFFER_POOL key %s", op.c_str(), pool.c_str());
    if (op == SET_COMMAND)
    {
        // For set command:
        // 1. Create the corresponding table entries in APPL_DB
        // 2. Record the table in the internal cache m_bufferPoolLookup
        buffer_pool_t &bufferPool = m_bufferPoolLookup[pool];

        bufferPool.dynamic_size = true;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            string &field = fvField(*i);
            string &value = fvValue(*i);

            SWSS_LOG_DEBUG("field:%s, value:%s", field.c_str(), value.c_str());
            if (field == buffer_size_field_name)
            {
                bufferPool.dynamic_size = false;
            }
            if (field == buffer_pool_xoff_field_name)
            {
                bufferPool.xoff = value;
            }
            if (field == buffer_pool_mode_field_name)
            {
                bufferPool.mode = value;
            }
            if (field == buffer_pool_type_field_name)
            {
                bufferPool.ingress = (value == buffer_value_ingress);
            }
            fvVector.emplace_back(FieldValueTuple(field, value));
            SWSS_LOG_INFO("Inserting BUFFER_POOL table field %s value %s", field.c_str(), value.c_str());
        }
        if (!bufferPool.dynamic_size)
        {
            m_applBufferPoolTable.set(pool, fvVector);
            m_stateBufferPoolTable.set(pool, fvVector);
        }
    }
    else if (op == DEL_COMMAND)
    {
        // How do we handle dependency?
        m_applBufferPoolTable.del(pool);
        m_stateBufferPoolTable.del(pool);
        m_bufferPoolLookup.erase(pool);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleBufferProfileTable(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    string profileName = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<FieldValueTuple> fvVector;

    SWSS_LOG_DEBUG("processing command:%s BUFFER_PROFILE table key %s", op.c_str(), profileName.c_str());
    if (op == SET_COMMAND)
    {
        //For set command:
        //1. Create the corresponding table entries in APPL_DB
        //2. Record the table in the internal cache m_bufferProfileLookup
        buffer_profile_t &profileApp = m_bufferProfileLookup[profileName];

        profileApp.static_configured = true;
        if (PROFILE_INITIALIZING == profileApp.state)
        {
            profileApp.dynamic_calculated = false;
            profileApp.lossless = false;
            profileApp.name = profileName;
        }
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            string &field = fvField(*i);
            string &value = fvValue(*i);

            SWSS_LOG_DEBUG("field:%s, value:%s", field.c_str(), value.c_str());
            if (field == buffer_pool_field_name)
            {
                if (!value.empty())
                {
                    transformReference(value);
                    auto poolName = parseObjectNameFromReference(value);
                    if (poolName.empty())
                    {
                        SWSS_LOG_ERROR("BUFFER_PROFILE: Invalid format of reference to pool: %s", value.c_str());
                        return task_process_status::task_invalid_entry;
                    }

                    auto poolRef = m_bufferPoolLookup.find(poolName);
                    if (poolRef == m_bufferPoolLookup.end())
                    {
                        SWSS_LOG_WARN("Pool %s hasn't been configured yet, skip", poolName.c_str());
                        return task_process_status::task_need_retry;
                    }
                    profileApp.pool_name = poolName;
                    profileApp.ingress = poolRef->second.ingress;
                }
                else
                {
                    SWSS_LOG_ERROR("Pool for BUFFER_PROFILE %s hasn't been specified", field.c_str());
                    return task_process_status::task_failed;
                }
            }
            if (field == buffer_xon_field_name)
            {
                profileApp.xon = value;
            }
            if (field == buffer_xoff_field_name)
            {
                profileApp.xoff = value;
                profileApp.lossless = true;
            }
            if (field == buffer_xon_offset_field_name)
            {
                profileApp.xon_offset = value;
            }
            if (field == buffer_size_field_name)
            {
                profileApp.size = value;
            }
            if (field == buffer_dynamic_th_field_name)
            {
                profileApp.threshold = value;
            }
            if (field == buffer_static_th_field_name)
            {
                profileApp.threshold = value;
            }
            if (field == buffer_headroom_type_field_name)
            {
                profileApp.dynamic_calculated = (value == "dynamic");
                if (profileApp.dynamic_calculated)
                {
                    // For dynamic calculated headroom, user can provide this field only
                    // We need to supply lossless and ingress
                    profileApp.lossless = true;
                    profileApp.ingress = true;
                }
            }
            fvVector.emplace_back(FieldValueTuple(field, value));
            SWSS_LOG_INFO("Inserting BUFFER_PROFILE table field %s value %s", field.c_str(), value.c_str());
        }
        // don't insert dynamically calculated profiles into APPL_DB
        if (profileApp.lossless && profileApp.ingress)
        {
            if (profileApp.dynamic_calculated)
            {
                profileApp.state = PROFILE_NORMAL;
                SWSS_LOG_NOTICE("BUFFER_PROFILE %s is dynamic calculation so it won't be deployed to APPL_DB until referenced by a port",
                                profileName.c_str());
                doUpdateStaticProfileTask(profileApp);
            }
            else
            {
                profileApp.state = PROFILE_NORMAL;
                doUpdateStaticProfileTask(profileApp);
                SWSS_LOG_NOTICE("BUFFER_PROFILE %s has been inserted into APPL_DB", profileName.c_str());
                SWSS_LOG_DEBUG("BUFFER_PROFILE %s for headroom override has been stored internally: [pool %s xon %s xoff %s size %s]",
                               profileName.c_str(),
                               profileApp.pool_name.c_str(), profileApp.xon.c_str(), profileApp.xoff.c_str(), profileApp.size.c_str());
            }
        }
        else
        {
            m_applBufferProfileTable.set(profileName, fvVector);
            SWSS_LOG_NOTICE("BUFFER_PROFILE %s has been inserted into APPL_DB directly", profileName.c_str());

            m_stateBufferProfileTable.set(profileName, fvVector);
            m_bufferProfileIgnored.insert(profileName);
        }
    }
    else if (op == DEL_COMMAND)
    {
        // For del command:
        // Check whether it is referenced by port. If yes, return "need retry" and exit
        // Typically, the referencing occurs when headroom override configured
        // Remove it from APPL_DB and internal cache

        auto profileRef = m_bufferProfileLookup.find(profileName);
        if (profileRef != m_bufferProfileLookup.end())
        {
            auto &profile = profileRef->second;
            if (!profile.port_pgs.empty())
            {
                // still being referenced
                if (profile.static_configured)
                {
                    // For headroom override, we just wait until all reference removed
                    SWSS_LOG_WARN("BUFFER_PROFILE %s for headroom override is referenced and cannot be removed for now", profileName.c_str());
                    return task_process_status::task_need_retry;
                }
                else
                {
                    SWSS_LOG_ERROR("Try to remove non-static-configured profile %s", profileName.c_str());
                    return task_process_status::task_invalid_entry;
                }
            }
        }

        m_applBufferProfileTable.del(profileName);
        m_stateBufferProfileTable.del(profileName);

        m_bufferProfileLookup.erase(profileName);
        m_bufferProfileIgnored.erase(profileName);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleOneBufferPgEntry(const string &key, const string &port, const string &op, const KeyOpFieldsValuesTuple &tuple)
{
    vector<FieldValueTuple> fvVector;
    buffer_pg_t &bufferPg = m_portPgLookup[port][key];

    SWSS_LOG_DEBUG("processing command:%s table BUFFER_PG key %s", op.c_str(), key.c_str());
    if (op == SET_COMMAND)
    {
        bool ignored = false;
        bool pureDynamic = true;
        // For set command:
        // 1. Create the corresponding table entries in APPL_DB
        // 2. Record the table in the internal cache m_portPgLookup
        // 3. Check whether the profile is ingress or egress
        // 4. Initialize "profile_name" of buffer_pg_t

        bufferPg.dynamic_calculated = true;
        bufferPg.static_configured = false;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            const string &field = fvField(*i);
            string value = fvValue(*i);

            SWSS_LOG_DEBUG("field:%s, value:%s", field.c_str(), value.c_str());
            if (field == buffer_profile_field_name && value != "NULL")
            {
                // Headroom override
                pureDynamic = false;
                transformReference(value);
                string profileName = parseObjectNameFromReference(value);
                if (profileName.empty())
                {
                    SWSS_LOG_ERROR("BUFFER_PG: Invalid format of reference to profile: %s", value.c_str());
                    return task_process_status::task_invalid_entry;
                }

                auto searchRef = m_bufferProfileLookup.find(profileName);
                if (searchRef == m_bufferProfileLookup.end())
                {
                    if (m_bufferProfileIgnored.find(profileName) != m_bufferProfileIgnored.end())
                    {
                        // Referencing an ignored profile, the PG should be ignored as well
                        ignored = true;
                        bufferPg.dynamic_calculated = false;
                        bufferPg.lossless = false;
                        bufferPg.configured_profile_name = profileName;
                    }
                    else
                    {
                        // In this case, we shouldn't set the dynamc calculated flat to true
                        // It will be updated when its profile configured.
                        bufferPg.dynamic_calculated = false;
                        SWSS_LOG_WARN("Profile %s hasn't been configured yet, skip", profileName.c_str());
                        return task_process_status::task_need_retry;
                    }
                }
                else
                {
                    buffer_profile_t &profileRef = searchRef->second;
                    bufferPg.dynamic_calculated = profileRef.dynamic_calculated;
                    bufferPg.configured_profile_name = profileName;
                    bufferPg.lossless = profileRef.lossless;
                }
                bufferPg.static_configured = true;
                bufferPg.configured_profile_name = profileName;
            }
            fvVector.emplace_back(FieldValueTuple(field, value));
            SWSS_LOG_INFO("Inserting BUFFER_PG table field %s value %s", field.c_str(), value.c_str());
        }

        if (pureDynamic)
        {
            // Generic dynamically calculated headroom
            bufferPg.dynamic_calculated = true;
            bufferPg.lossless = true;
        }

        if (!ignored && bufferPg.lossless)
        {
            doUpdatePgTask(key, port);
        }
        else
        {
            SWSS_LOG_NOTICE("Inserting BUFFER_PG table entry %s into APPL_DB directly", key.c_str());
            m_applBufferPgTable.set(key, fvVector);
        }
    }
    else if (op == DEL_COMMAND)
    {
        // For del command:
        // 1. Removing it from APPL_DB
        // 2. Update internal caches
        string &profileName = bufferPg.running_profile_name;

        m_bufferProfileLookup[profileName].port_pgs.erase(key);

        if (bufferPg.lossless)
        {
            doRemovePgTask(key, port);
        }
        else
        {
            SWSS_LOG_NOTICE("Removing BUFFER_PG table entry %s from APPL_DB directly", key.c_str());
            m_applBufferPgTable.del(key);
        }

        m_portPgLookup[port].erase(key);
        SWSS_LOG_DEBUG("Profile %s has been removed from port %s PG %s", profileName.c_str(), port.c_str(), key.c_str());
        if (m_portPgLookup[port].empty())
        {
            m_portPgLookup.erase(port);
            SWSS_LOG_DEBUG("Profile %s has been removed from port %s on all lossless PG", profileName.c_str(), port.c_str());
        }
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleBufferPgTable(KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);

    transformSeperator(key);
    string ports = parseObjectNameFromKey(key);
    string pgs = parseObjectNameFromKey(key, 1);
    if (ports.empty() || pgs.empty())
    {
        SWSS_LOG_ERROR("Invalid key format %s for BUFFER_PG table", key.c_str());
        return task_process_status::task_invalid_entry;
    }

    auto portsList = tokenize(ports, ',');

    task_process_status rc = task_process_status::task_success;

    if (portsList.size() == 1)
    {
        rc = handleOneBufferPgEntry(key, ports, op, tuple);
    }
    else
    {
        for (auto port : portsList)
        {
            string singleKey = port + ':' + pgs;
            rc = handleOneBufferPgEntry(singleKey, port, op, tuple);
            if (rc == task_process_status::task_need_retry)
                return rc;
        }
    }

    return rc;
}

task_process_status BufferMgrDynamic::handleBufferQueueTable(KeyOpFieldsValuesTuple &tuple)
{
    return doBufferTableTask(tuple, m_applBufferQueueTable);
}

task_process_status BufferMgrDynamic::handleBufferPortIngressProfileListTable(KeyOpFieldsValuesTuple &tuple)
{
    return doBufferTableTask(tuple, m_applBufferIngressProfileListTable);
}

task_process_status BufferMgrDynamic::handleBufferPortEgressProfileListTable(KeyOpFieldsValuesTuple &tuple)
{
    return doBufferTableTask(tuple, m_applBufferEgressProfileListTable);
}

/*
 * This function copies the data from tables in CONFIG_DB to APPL_DB.
 * With dynamically buffer calculation supported, the following tables
 * will be moved to APPL_DB from CONFIG_DB because the CONFIG_DB contains
 * confgured entries only while APPL_DB contains dynamically generated entries
 *  - BUFFER_POOL
 *  - BUFFER_PROFILE
 *  - BUFFER_PG
 * The following tables have to be moved to APPL_DB because they reference
 * some entries that have been moved to APPL_DB
 *  - BUFFER_QUEUE
 *  - BUFFER_PORT_INGRESS_PROFILE_LIST
 *  - BUFFER_PORT_EGRESS_PROFILE_LIST   
 * One thing we need to handle is to transform the separator from | to :
 * The following items contain separator:
 *  - keys of each item
 *  - pool in BUFFER_POOL
 *  - profile in BUFFER_PG
 */
task_process_status BufferMgrDynamic::doBufferTableTask(KeyOpFieldsValuesTuple &tuple, ProducerStateTable &applTable)
{
    SWSS_LOG_ENTER();

    string key = kfvKey(tuple);
    const string &name = applTable.getTableName();

    //transform the separator in key from "|" to ":"
    transformSeperator(key);

    string op = kfvOp(tuple);
    if (op == SET_COMMAND)
    {
        vector<FieldValueTuple> fvVector;

        SWSS_LOG_INFO("Inserting entry %s|%s from CONFIG_DB to APPL_DB", name.c_str(), key.c_str());

        for (auto i : kfvFieldsValues(tuple))
        {
            //transform the separator in values from "|" to ":"
            if (fvField(i) == "pool")
                transformReference(fvValue(i));
            if (fvField(i) == "profile")
                transformReference(fvValue(i));
            if (fvField(i) == "profile_list")
                transformReference(fvValue(i));
            fvVector.emplace_back(FieldValueTuple(fvField(i), fvValue(i)));
            SWSS_LOG_INFO("Inserting field %s value %s", fvField(i).c_str(), fvValue(i).c_str());
        }
        applTable.set(key, fvVector);
    }
    else if (op == DEL_COMMAND)
    {
        SWSS_LOG_INFO("Removing entry %s from APPL_DB", key.c_str());
        applTable.del(key);
    }

    return task_process_status::task_success;
}

void BufferMgrDynamic::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    string table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();

    if (m_bufferTableHandlerMap.find(table_name) == m_bufferTableHandlerMap.end())
    {
        SWSS_LOG_ERROR("No handler for key:%s found.", table_name.c_str());
        while (it != consumer.m_toSync.end())
            it = consumer.m_toSync.erase(it);
        return;
    }

    while (it != consumer.m_toSync.end())
    {
        auto task_status = (this->*(m_bufferTableHandlerMap[table_name]))(it->second);
        switch (task_status)
        {
            case task_process_status::task_failed:
                SWSS_LOG_ERROR("Failed to process table update");
                return;
            case task_process_status::task_need_retry:
                SWSS_LOG_INFO("Unable to process table update. Will retry...");
                it++;
                break;
            case task_process_status::task_invalid_entry:
                SWSS_LOG_ERROR("Failed to process invalid entry, drop it");
                it = consumer.m_toSync.erase(it);
                break;
            default:
                it = consumer.m_toSync.erase(it);
                break;
        }
    }
}

void BufferMgrDynamic::doTask(SelectableTimer &timer)
{
    checkSharedBufferPoolSize();
}
