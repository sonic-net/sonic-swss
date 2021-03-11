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
 *        In internal maps: table name removed from the index
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
void BufferMgrDynamic::calculateHeadroomSize(buffer_profile_t &headroom)
{
    // Call vendor-specific lua plugin to calculate the xon, xoff, xon_offset, size and threshold
    vector<string> keys = {};
    vector<string> argv = {};

    keys.emplace_back(headroom.name);
    argv.emplace_back(headroom.speed);
    argv.emplace_back(headroom.cable_length);
    argv.emplace_back(headroom.port_mtu);
    argv.emplace_back(m_identifyGearboxDelay);

    try
    {
        auto ret = swss::runRedisScript(*m_applDb, m_headroomSha, keys, argv);

        if (ret.empty())
        {
            SWSS_LOG_ERROR("Failed to calculate headroom for %s", headroom.name.c_str());
            return;
        }

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

// This function is designed to fetch the sizes of shared buffer pool and shared headroom pool
// and programe them to APPL_DB if they differ from the current value.
// The function is called periodically:
// 1. Fetch the sizes by calling lug plugin
//    - For each of the pools, it checks the size of shared buffer pool.
//    - For ingress_lossless_pool, it checks the size of the shared headroom pool (field xoff of the pool) as well.
// 2. Compare the fetched value and the previous value
// 3. Program to APPL_DB.BUFFER_POOL_TABLE only if its sizes differ from the stored value
void BufferMgrDynamic::recalculateSharedBufferPool()
{
    try
    {
        vector<string> keys = {};
        vector<string> argv = {};

        auto ret = runRedisScript(*m_applDb, m_bufferpoolSha, keys, argv);

        // The format of the result:
        // a list of lines containing key, value pairs with colon as separator
        // each is the size of a buffer pool
        // possible format of each line:
        // 1. shared buffer pool only:
        //    <pool name>:<pool size>
        //    eg: "egress_lossless_pool:12800000"
        // 2. shared buffer pool and shared headroom pool, for ingress_lossless_pool only:
        //    ingress_lossless_pool:<pool size>:<shared headroom pool size>,
        //    eg: "ingress_lossless_pool:3200000:1024000"
        // 3. debug information:
        //    debug:<debug info>

        if (ret.empty())
        {
            SWSS_LOG_WARN("Failed to recalculate shared buffer pool size");
            return;
        }

        for ( auto i : ret)
        {
            auto pairs = tokenize(i, ':');
            auto poolName = pairs[0];

            if ("debug" != poolName)
            {
                // We will handle the sizes of buffer pool update here.
                // For the ingress_lossless_pool, there are some dedicated steps for shared headroom pool
                //  - The sizes of both the shared headroom pool and the shared buffer pool should be taken into consideration
                //  - In case the shared headroom pool size is statically configured, as it is programmed to APPL_DB during buffer pool handling,
                //     - any change from lua plugin will be ignored.
                //     - will handle ingress_lossless_pool in the way all other pools are handled in this case
                auto &pool = m_bufferPoolLookup[poolName];
                auto &poolSizeStr = pairs[1];
                auto old_xoff = pool.xoff;
                bool xoff_updated = false;

                if (poolName == INGRESS_LOSSLESS_PG_POOL_NAME && !isNonZero(m_configuredSharedHeadroomPoolSize))
                {
                    // Shared headroom pool size is treated as "updated" if either of the following conditions satisfied:
                    //  - It is legal and differs from the stored value.
                    //  - The lua plugin doesn't return the shared headroom pool size but there is a non-zero value stored
                    //    This indicates the shared headroom pool was enabled by over subscribe ratio and is disabled.
                    //    In this case a "0" will programmed to APPL_DB, indicating the shared headroom pool is disabled.
                    SWSS_LOG_DEBUG("Buffer pool ingress_lossless_pool xoff: %s, size %s", pool.xoff.c_str(), pool.total_size.c_str());

                    if (pairs.size() > 2)
                    {
                        auto &xoffStr = pairs[2];
                        if (pool.xoff != xoffStr)
                        {
                            unsigned long xoffNum = atol(xoffStr.c_str());
                            if (m_mmuSizeNumber > 0 && m_mmuSizeNumber < xoffNum)
                            {
                                SWSS_LOG_ERROR("Buffer pool %s: Invalid xoff %s, exceeding the mmu size %s, ignored xoff but the pool size will be updated",
                                               poolName.c_str(), xoffStr.c_str(), m_mmuSize.c_str());
                            }
                            else
                            {
                                pool.xoff = xoffStr;
                                xoff_updated = true;
                            }
                        }
                    }
                    else
                    {
                        if (isNonZero(pool.xoff))
                        {
                            xoff_updated = true;
                        }
                        pool.xoff = "0";
                    }
                }

                // In general, the APPL_DB should be updated in case any of the following conditions satisfied
                // 1. Shared headroom pool size has been updated
                //    This indicates the shared headroom pool is enabled by configuring over subscribe ratio,
                //    which means the shared headroom pool size has updated by lua plugin
                // 2. The size of the shared buffer pool isn't configured and has been updated by lua plugin
                if ((pool.total_size == poolSizeStr || !pool.dynamic_size) && !xoff_updated)
                    continue;

                unsigned long poolSizeNum = atol(poolSizeStr.c_str());
                if (m_mmuSizeNumber > 0 && m_mmuSizeNumber < poolSizeNum)
                {
                    SWSS_LOG_ERROR("Buffer pool %s: Invalid size %s, exceeding the mmu size %s",
                                   poolName.c_str(), poolSizeStr.c_str(), m_mmuSize.c_str());
                    continue;
                }

                auto old_size = pool.total_size;
                pool.total_size = poolSizeStr;
                updateBufferPoolToDb(poolName, pool);

                if (!pool.xoff.empty())
                {
                    SWSS_LOG_NOTICE("Buffer pool %s has been updated: size from [%s] to [%s], xoff from [%s] to [%s]",
                                    poolName.c_str(), old_size.c_str(), pool.total_size.c_str(), old_xoff.c_str(), pool.xoff.c_str());
                }
                else
                {
                    SWSS_LOG_NOTICE("Buffer pool %s has been updated: size from [%s] to [%s]", poolName.c_str(), old_size.c_str(), pool.total_size.c_str());
                }
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
                // without all buffer pools created, buffer profiles are unable to be created,
                // which in turn causes buffer pgs and buffer queues unable to be created,
                // which prevents the port from being ready and eventually fails the warm reboot
                // After the buffer pools are created for the first time, we won't touch it
                // until portInitDone
                // Eventually, the correct values will pushed to APPL_DB and then ASIC_DB
                recalculateSharedBufferPool();
                m_firstTimeCalculateBufferPool = false;
                SWSS_LOG_NOTICE("Buffer pool update deferred because port is still under initialization, start polling timer");
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
        fvVector.emplace_back("type", "ingress");
    else
        fvVector.emplace_back("type", "egress");

    if (!pool.xoff.empty())
        fvVector.emplace_back("xoff", pool.xoff);

    fvVector.emplace_back("mode", pool.mode);

    fvVector.emplace_back("size", pool.total_size);

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

    fvVector.emplace_back("xon", profile.xon);
    if (!profile.xon_offset.empty()) {
        fvVector.emplace_back("xon_offset", profile.xon_offset);
    }
    fvVector.emplace_back("xoff", profile.xoff);
    fvVector.emplace_back("size", profile.size);
    fvVector.emplace_back("pool", "[" + pg_pool_reference + "]");
    fvVector.emplace_back(mode, profile.threshold);

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
task_process_status BufferMgrDynamic::allocateProfile(const string &speed, const string &cable_len, const string &mtu, const string &threshold, const string &gearbox_model, string &profile_name)
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

        profile.speed = speed;
        profile.cable_length = cable_len;
        profile.port_mtu = mtu;
        profile.gearbox_model = gearbox_model;

        // Call vendor-specific lua plugin to calculate the xon, xoff, xon_offset, size
        // Pay attention, the threshold can contain valid value
        calculateHeadroomSize(profile);

        profile.threshold = threshold;
        profile.static_configured = false;
        profile.lossless = true;
        profile.name = profile_name;
        profile.state = PROFILE_NORMAL;

        updateBufferProfileToDb(profile_name, profile);

        SWSS_LOG_NOTICE("BUFFER_PROFILE %s has been created successfully", profile_name.c_str());
        SWSS_LOG_DEBUG("New profile created %s according to (%s %s %s): xon %s xoff %s size %s",
                       profile_name.c_str(),
                       speed.c_str(), cable_len.c_str(), gearbox_model.c_str(),
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
    // port: used to fetch the maximum headroom size
    // profile: the profile referenced by the new_pg (if provided) or all PGs
    // new_pg: which pg is newly added?

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

        if (ret.empty())
        {
            SWSS_LOG_WARN("Failed to check headroom for %s", profile.name.c_str());
            return result;
        }

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

task_process_status BufferMgrDynamic::removeAllPgsFromPort(const string &port)
{
    buffer_pg_lookup_t &portPgs = m_portPgLookup[port];
    set<string> profilesToBeReleased;

    SWSS_LOG_INFO("Removing all PGs from port %s", port.c_str());

    for (auto it = portPgs.begin(); it != portPgs.end(); ++it)
    {
        auto &key = it->first;
        auto &portPg = it->second;

        SWSS_LOG_INFO("Removing PG %s from port %s", key.c_str(), port.c_str());

        if (portPg.running_profile_name.empty())
            continue;

        m_bufferProfileLookup[portPg.running_profile_name].port_pgs.erase(key);
        updateBufferPgToDb(key, portPg.running_profile_name, false);
        profilesToBeReleased.insert(portPg.running_profile_name);
        portPg.running_profile_name.clear();
    }

    checkSharedBufferPoolSize();

    // Remove the old profile which is probably not referenced anymore.
    if (!profilesToBeReleased.empty())
    {
        for (auto &oldProfile : profilesToBeReleased)
        {
            releaseProfile(oldProfile);
        }
    }

    return task_process_status::task_success;
}

// Called when speed/cable length updated from CONFIG_DB
// Update buffer profile of a certain PG of a port or all PGs of the port according to its speed, cable_length and mtu
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
task_process_status BufferMgrDynamic::refreshPgsForPort(const string &port, const string &speed, const string &cable_length, const string &mtu, const string &exactly_matched_key = "")
{
    port_info_t &portInfo = m_portInfoLookup[port];
    string &gearbox_model = portInfo.gearbox_model;
    bool isHeadroomUpdated = false;
    buffer_pg_lookup_t &portPgs = m_portPgLookup[port];
    set<string> profilesToBeReleased;

    if (portInfo.state == PORT_ADMIN_DOWN)
    {
        SWSS_LOG_INFO("Nothing to be done since the port %s is administratively down", port.c_str());
        return task_process_status::task_success;
    }

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

        if (portPg.dynamic_calculated)
        {
            string threshold;
            // Calculate new headroom size
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

        // Calculate whether accumulative headroom size exceeds the maximum value
        // Abort if it does
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
            m_bufferProfileLookup[newProfile].port_pgs.insert(key);
            SWSS_LOG_INFO("Move profile reference for %s from [%s] to [%s]", key.c_str(), oldProfile.c_str(), newProfile.c_str());

            // Add the old profile to "to be removed" set
            if (!oldProfile.empty())
            {
                profilesToBeReleased.insert(oldProfile);
                m_bufferProfileLookup[oldProfile].port_pgs.erase(key);
            }

            // buffer pg needs to be updated as well
            portPg.running_profile_name = newProfile;
        }

        // appl_db Database operation: set item BUFFER_PG|<port>|<pg>
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

    // Remove the old profile which is probably not referenced anymore.
    if (!profilesToBeReleased.empty())
    {
        for (auto &oldProfile : profilesToBeReleased)
        {
            releaseProfile(oldProfile);
        }
    }

    return task_process_status::task_success;
}

void BufferMgrDynamic::refreshSharedHeadroomPool(bool enable_state_updated_by_ratio, bool enable_state_updated_by_size)
{
    // The lossless profiles need to be refreshed only if system is switched between SHP and non-SHP
    bool need_refresh_profiles = false;
    bool shp_enabled_by_size = isNonZero(m_configuredSharedHeadroomPoolSize);
    bool shp_enabled_by_ratio = isNonZero(m_overSubscribeRatio);

    /*
     * STATE               EVENT           ACTION
     * enabled by size  -> config ratio:   no action
     * enabled by size  -> remove ratio:   no action
     * enabled by size  -> remove size:    shared headroom pool disabled
     *                                     SHP size will be set here (zero) and programmed to APPL_DB in handleBufferPoolTable
     * enabled by ratio -> config size:    SHP size will be set here (non zero) and programmed to APPL_DB in handleBufferPoolTable
     * enabled by ratio -> remove size:    SHP size will be set and programmed to APPL_DB during buffer pool size update
     * enabled by ratio -> remove ratio:   shared headroom pool disabled
     *                                     dynamic size: SHP size will be handled and programmed to APPL_DB during buffer pool size update
     *                                     static size: SHP size will be set (zero) and programmed to APPL_DB here
     * disabled         -> config ratio:   shared headroom pool enabled. all lossless profiles refreshed
     *                                     SHP size will be handled during buffer pool size update
     * disabled         -> config size:    shared headroom pool enabled. all lossless profiles refreshed
     *                                     SHP size will be handled here and programmed to APPL_DB during buffer pool table handling
     */

    auto &ingressLosslessPool = m_bufferPoolLookup[INGRESS_LOSSLESS_PG_POOL_NAME];
    if (enable_state_updated_by_ratio)
    {
        if (shp_enabled_by_size)
        {
            // enabled by size -> config or remove ratio, no action
            SWSS_LOG_INFO("SHP: No need to refresh lossless profiles even if enable state updated by over subscribe ratio, already enabled by SHP size");
        }
        else
        {
            // enabled by ratio -> remove ratio
            // disabled -> config ratio
            need_refresh_profiles = true;
        }
    }

    if (enable_state_updated_by_size)
    {
        if (shp_enabled_by_ratio)
        {
            // enabled by ratio -> config size, size will be updated (later in this function)
            // enabled by ratio -> remove size, no action here, will be handled during buffer pool size update
            SWSS_LOG_INFO("SHP: No need to refresh lossless profiles even if enable state updated by SHP size, already enabled by over subscribe ratio");
        }
        else
        {
            // disabled -> config size
            // enabled by size -> remove size
            need_refresh_profiles = true;
        }
    }

    if (need_refresh_profiles)
    {
        SWSS_LOG_NOTICE("Updating dynamic buffer profiles due to shared headroom pool state updated");

        for (auto it = m_bufferProfileLookup.begin(); it != m_bufferProfileLookup.end(); ++it)
        {
            auto &name = it->first;
            auto &profile = it->second;
            if (profile.static_configured)
            {
                SWSS_LOG_INFO("Non dynamic profile %s skipped", name.c_str());
                continue;
            }
            SWSS_LOG_INFO("Updating profile %s with speed %s cable length %s mtu %s gearbox model %s",
                          name.c_str(),
                          profile.speed.c_str(), profile.cable_length.c_str(), profile.port_mtu.c_str(), profile.gearbox_model.c_str());
            // recalculate the headroom size
            calculateHeadroomSize(profile);
            if (task_process_status::task_success != doUpdateBufferProfileForSize(profile, false))
            {
                SWSS_LOG_ERROR("Failed to update buffer profile %s when toggle shared headroom pool. See previous message for detail. Please adjust the configuration manually", name.c_str());
            }
        }
        SWSS_LOG_NOTICE("Updating dynamic buffer profiles finished");
    }

    if (shp_enabled_by_size)
    {
        ingressLosslessPool.xoff = m_configuredSharedHeadroomPoolSize;
        if (isNonZero(ingressLosslessPool.total_size))
            updateBufferPoolToDb(INGRESS_LOSSLESS_PG_POOL_NAME, ingressLosslessPool);
    }
    else if (!shp_enabled_by_ratio && enable_state_updated_by_ratio)
    {
        // shared headroom pool is enabled by ratio and will be disabled
        // need to program APPL_DB because nobody else will take care of it
        ingressLosslessPool.xoff = "0";
        if (isNonZero(ingressLosslessPool.total_size))
            updateBufferPoolToDb(INGRESS_LOSSLESS_PG_POOL_NAME, ingressLosslessPool);
    }

    checkSharedBufferPoolSize();
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
        task_status = refreshPgsForPort(port, portInfo.speed, portInfo.cable_length, portInfo.mtu, pg_key);
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
            task_status = refreshPgsForPort(port, portInfo.speed, portInfo.cable_length, portInfo.mtu, pg_key);
            if (task_status != task_process_status::task_success)
                return task_status;
        }
        break;

    case PORT_ADMIN_DOWN:
        SWSS_LOG_NOTICE("Skip setting BUFFER_PG for %s because the port is administratively down", port.c_str());
        break;

    default:
        // speed and cable length hasn't been configured
        // In that case, we just skip the this update and return success.
        // It will be handled after speed and cable length configured.
        SWSS_LOG_NOTICE("Skip setting BUFFER_PG for %s because port's info isn't ready for dynamic calculation", pg_key.c_str());
        return task_process_status::task_success;
    }

    return task_process_status::task_success;
}

// Remove the currently configured lossless pg
task_process_status BufferMgrDynamic::doRemovePgTask(const string &pg_key, const string &port)
{
    auto &bufferPgs = m_portPgLookup[port];
    buffer_pg_t &bufferPg = bufferPgs[pg_key];
    port_info_t &portInfo = m_portInfoLookup[port];

    // Remove the PG from APPL_DB
    string null_str("");
    updateBufferPgToDb(pg_key, null_str, false);

    SWSS_LOG_NOTICE("Remove BUFFER_PG %s (profile %s, %s)", pg_key.c_str(), bufferPg.running_profile_name.c_str(), bufferPg.configured_profile_name.c_str());

    // Recalculate pool size
    checkSharedBufferPoolSize();

    if (portInfo.state != PORT_ADMIN_DOWN)
    {
        if (!portInfo.speed.empty() && !portInfo.cable_length.empty())
            portInfo.state = PORT_READY;
        else
            portInfo.state = PORT_INITIALIZING;
    }

    // The bufferPg.running_profile_name can be empty if the port is admin down.
    // In that case, releaseProfile should not be called
    if (!bufferPg.running_profile_name.empty())
    {
        SWSS_LOG_NOTICE("Try removing the original profile %s", bufferPg.running_profile_name.c_str());
        releaseProfile(bufferPg.running_profile_name);
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::doUpdateBufferProfileForDynamicTh(buffer_profile_t &profile)
{
    const string &profileName = profile.name;
    auto &profileToMap = profile.port_pgs;
    set<string> portsChecked;

    if (profile.static_configured && profile.dynamic_calculated)
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

            rc = refreshPgsForPort(portName, port.speed, port.cable_length, port.mtu);
            if (task_process_status::task_success != rc)
            {
                SWSS_LOG_ERROR("Update the profile on %s failed", key.c_str());
                return rc;
            }
        }
    }

    checkSharedBufferPoolSize();

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::doUpdateBufferProfileForSize(buffer_profile_t &profile, bool update_pool_size=true)
{
    const string &profileName = profile.name;
    auto &profileToMap = profile.port_pgs;
    set<string> portsChecked;

    if (!profile.static_configured || !profile.dynamic_calculated)
    {
        for (auto &key : profileToMap)
        {
            auto port = parseObjectNameFromKey(key);

            if (portsChecked.find(port) != portsChecked.end())
                continue;

            SWSS_LOG_DEBUG("Checking PG %s for profile %s", key.c_str(), profileName.c_str());

            if (!isHeadroomResourceValid(port, profile))
            {
                SWSS_LOG_ERROR("BUFFER_PROFILE %s cannot be updated because %s referencing it violates the resource limitation",
                               profileName.c_str(), key.c_str());
                return task_process_status::task_failed;
            }

            portsChecked.insert(port);
        }

        updateBufferProfileToDb(profileName, profile);
    }

    if (update_pool_size)
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
    string newRatio = "0";

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "default_dynamic_th")
            {
                m_defaultThreshold = fvValue(i);
                SWSS_LOG_DEBUG("Handling Buffer parameter table field default_dynamic_th value %s", m_defaultThreshold.c_str());
            }
            else if (fvField(i) == "over_subscribe_ratio")
            {
                newRatio = fvValue(i);
                SWSS_LOG_DEBUG("Handling Buffer parameter table field over_subscribe_ratio value %s", fvValue(i).c_str());
            }
        }
    }
    else
    {
        SWSS_LOG_ERROR("Unsupported command %s received for DEFAULT_LOSSLESS_BUFFER_PARAMETER table", op.c_str());
        return task_process_status::task_failed;
    }

    if (newRatio != m_overSubscribeRatio)
    {
        bool isSHPEnabled = isNonZero(m_overSubscribeRatio);
        bool willSHPBeEnabled = isNonZero(newRatio);
        SWSS_LOG_INFO("Recalculate shared buffer pool size due to over subscribe ratio has been updated from %s to %s",
                      m_overSubscribeRatio.c_str(), newRatio.c_str());
        m_overSubscribeRatio = newRatio;
        refreshSharedHeadroomPool(isSHPEnabled != willSHPBeEnabled, false);
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

            // Try updating the buffer information
            switch (portInfo.state)
            {
            case PORT_INITIALIZING:
                portInfo.state = PORT_READY;
                task_status = refreshPgsForPort(port, speed, cable_length, mtu);
                break;

            case PORT_READY:
                task_status = refreshPgsForPort(port, speed, cable_length, mtu);
                break;

            case PORT_ADMIN_DOWN:
                // Nothing to be done here
                SWSS_LOG_INFO("Nothing to be done when port %s's cable length updated", port.c_str());
                task_status = task_process_status::task_success;
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
    bool speed_updated = false, mtu_updated = false, admin_status_updated = false, admin_up;

    SWSS_LOG_DEBUG("Processing command:%s PORT table key %s", op.c_str(), port.c_str());

    port_info_t &portInfo = m_portInfoLookup[port];

    SWSS_LOG_DEBUG("Port Info for %s before handling %s %s %s",
                   port.c_str(),
                   portInfo.speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());

    task_process_status task_status = task_process_status::task_success;

    if (op == SET_COMMAND)
    {
        string old_speed;
        string old_mtu;

        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "speed" && fvValue(i) != portInfo.speed)
            {
                speed_updated = true;
                old_speed = move(portInfo.speed);
                portInfo.speed = fvValue(i);
            }

            if (fvField(i) == "mtu" && fvValue(i) != portInfo.mtu)
            {
                mtu_updated = true;
                old_mtu = move(portInfo.mtu);
                portInfo.mtu = fvValue(i);
            }

            if (fvField(i) == "admin_status")
            {
                admin_up = (fvValue(i) == "up");
                auto old_admin_up = (portInfo.state != PORT_ADMIN_DOWN);
                admin_status_updated = (admin_up != old_admin_up);
            }
        }

        string &cable_length = portInfo.cable_length;
        string &mtu = portInfo.mtu;
        string &speed = portInfo.speed;

        bool need_refresh_all_pgs = false, need_remove_all_pgs = false;

        if (speed_updated || mtu_updated)
        {
            if (!cable_length.empty() && !speed.empty())
            {
                if (speed_updated)
                {
                    if (mtu_updated)
                    {
                        SWSS_LOG_INFO("Updating BUFFER_PG for port %s due to speed updated from %s to %s and MTU updated from %s to %s",
                                      port.c_str(), old_speed.c_str(), portInfo.speed.c_str(), old_mtu.c_str(), portInfo.mtu.c_str());
                    }
                    else
                    {
                        SWSS_LOG_INFO("Updating BUFFER_PG for port %s due to speed updated from %s to %s",
                                      port.c_str(), old_speed.c_str(), portInfo.speed.c_str());
                    }
                }
                else
                {
                    SWSS_LOG_INFO("Updating BUFFER_PG for port %s due to MTU updated from %s to %s",
                                  port.c_str(), old_mtu.c_str(), portInfo.mtu.c_str());
                }

                // Try updating the buffer information
                switch (portInfo.state)
                {
                case PORT_INITIALIZING:
                    portInfo.state = PORT_READY;
                    if (mtu.empty())
                    {
                        // It's the same case as that in handleCableLenTable
                        mtu = DEFAULT_MTU_STR;
                    }
                    need_refresh_all_pgs = true;
                    break;

                case PORT_READY:
                    need_refresh_all_pgs = true;
                    break;

                case PORT_ADMIN_DOWN:
                    SWSS_LOG_INFO("Nothing to be done when port %s's speed or cable length updated since the port is administratively down", port.c_str());
                    break;

                default:
                    SWSS_LOG_ERROR("Port %s: invalid port state %d when handling port update", port.c_str(), portInfo.state);
                    break;
                }

                SWSS_LOG_DEBUG("Port Info for %s after handling speed %s cable %s gb %s",
                               port.c_str(),
                               portInfo.speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());
            }
            else
            {
                SWSS_LOG_WARN("Cable length or speed for %s hasn't been configured yet, unable to calculate headroom", port.c_str());
                // We don't retry here because it doesn't make sense until both cable length and speed are configured.
            }
        }

        if (admin_status_updated)
        {
            if (admin_up)
            {
                if (!portInfo.speed.empty() && !portInfo.cable_length.empty())
                    portInfo.state = PORT_READY;
                else
                    portInfo.state = PORT_INITIALIZING;

                need_refresh_all_pgs = true;
            }
            else
            {
                portInfo.state = PORT_ADMIN_DOWN;

                need_remove_all_pgs = true;
            }

            SWSS_LOG_INFO("Recalculate shared buffer pool size due to port %s's admin_status updated to %s",
                          port.c_str(), (admin_up ? "up" : "down"));
        }

        // In case both need_remove_all_pgs and need_refresh_all_pgs are true, the need_remove_all_pgs will take effect.
        // This can happen when both speed (or mtu) is changed and the admin_status is down.
        // In this case, we just need record the new speed (or mtu) but don't need to refresh all PGs on the port since the port is administratively down
        if (need_remove_all_pgs)
        {
            task_status = removeAllPgsFromPort(port);
        }
        else if (need_refresh_all_pgs)
        {
            task_status = refreshPgsForPort(port, portInfo.speed, portInfo.cable_length, portInfo.mtu);
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

    SWSS_LOG_DEBUG("Processing command:%s table BUFFER_POOL key %s", op.c_str(), pool.c_str());
    if (op == SET_COMMAND)
    {
        // For set command:
        // 1. Create the corresponding table entries in APPL_DB
        // 2. Record the table in the internal cache m_bufferPoolLookup
        buffer_pool_t &bufferPool = m_bufferPoolLookup[pool];
        string newSHPSize = "0";

        bufferPool.dynamic_size = true;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            string &field = fvField(*i);
            string &value = fvValue(*i);

            SWSS_LOG_DEBUG("Field:%s, value:%s", field.c_str(), value.c_str());
            if (field == buffer_size_field_name)
            {
                bufferPool.dynamic_size = false;
            }
            if (field == buffer_pool_xoff_field_name)
            {
                newSHPSize = value;
            }
            if (field == buffer_pool_mode_field_name)
            {
                bufferPool.mode = value;
            }
            if (field == buffer_pool_type_field_name)
            {
                bufferPool.ingress = (value == buffer_value_ingress);
            }
            fvVector.emplace_back(field, value);
            SWSS_LOG_INFO("Inserting BUFFER_POOL table field %s value %s", field.c_str(), value.c_str());
        }

        bool dontUpdatePoolToDb = bufferPool.dynamic_size;
        if (pool == INGRESS_LOSSLESS_PG_POOL_NAME)
        {
            /*
             * "dontUpdatPoolToDb" is calculated for ingress_lossless_pool according to following rules:
             * Don't update | pool size | SHP enabled by size | SHP enabled by over subscribe ratio
             * True         | Dynamic   | Any                 | Any
             * False        | Static    | True                | Any
             * True         | Static    | False               | True
             * False        | Static    | False               | False
             */
            bool willSHPBeEnabledBySize = isNonZero(newSHPSize);
            if (newSHPSize != m_configuredSharedHeadroomPoolSize)
            {
                bool isSHPEnabledBySize = isNonZero(m_configuredSharedHeadroomPoolSize);

                m_configuredSharedHeadroomPoolSize = newSHPSize;
                refreshSharedHeadroomPool(false, isSHPEnabledBySize != willSHPBeEnabledBySize);
            }
            else if (!newSHPSize.empty())
            {
                SWSS_LOG_INFO("Shared headroom pool size updated without change (new %s vs current %s), skipped", newSHPSize.c_str(), m_configuredSharedHeadroomPoolSize.c_str());
            }

            if (!willSHPBeEnabledBySize)
            {
                // Don't need to program APPL_DB if shared headroom pool is enabled
                dontUpdatePoolToDb |= isNonZero(m_overSubscribeRatio);
            }
        }
        else if (isNonZero(newSHPSize))
        {
            SWSS_LOG_ERROR("Field xoff is supported for %s only, but got for %s, ignored", INGRESS_LOSSLESS_PG_POOL_NAME, pool.c_str());
        }

        if (!dontUpdatePoolToDb)
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

    SWSS_LOG_DEBUG("Processing command:%s BUFFER_PROFILE table key %s", op.c_str(), profileName.c_str());
    if (op == SET_COMMAND)
    {
        // For set command:
        // 1. Create the corresponding table entries in APPL_DB
        // 2. Record the table in the internal cache m_bufferProfileLookup
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
            const string &field = fvField(*i);
            string value = fvValue(*i);

            SWSS_LOG_DEBUG("Field:%s, value:%s", field.c_str(), value.c_str());
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
            fvVector.emplace_back(field, value);
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
                doUpdateBufferProfileForDynamicTh(profileApp);
            }
            else
            {
                profileApp.state = PROFILE_NORMAL;
                doUpdateBufferProfileForSize(profileApp);
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

            if (!profile.static_configured || !profile.dynamic_calculated)
            {
                m_applBufferProfileTable.del(profileName);
                m_stateBufferProfileTable.del(profileName);
            }

            m_bufferProfileLookup.erase(profileName);
            m_bufferProfileIgnored.erase(profileName);
        }
        else
        {
            SWSS_LOG_ERROR("Profile %s not found while being removed", profileName.c_str());
        }
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

    SWSS_LOG_DEBUG("Processing command:%s table BUFFER_PG key %s", op.c_str(), key.c_str());
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
        if (!bufferPg.configured_profile_name.empty())
        {
            m_bufferProfileLookup[bufferPg.configured_profile_name].port_pgs.erase(key);
            bufferPg.configured_profile_name = "";
        }

        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            const string &field = fvField(*i);
            string value = fvValue(*i);

            SWSS_LOG_DEBUG("Field:%s, value:%s", field.c_str(), value.c_str());
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
                        // In this case, we shouldn't set the dynamic calculated flag to true
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

            if (field != buffer_profile_field_name)
            {
                SWSS_LOG_ERROR("BUFFER_PG: Invalid field %s", field.c_str());
                return task_process_status::task_invalid_entry;
            }

            fvVector.emplace_back(field, value);
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
            bufferPg.running_profile_name = bufferPg.configured_profile_name;
        }

        if (!bufferPg.configured_profile_name.empty())
        {
            m_bufferProfileLookup[bufferPg.configured_profile_name].port_pgs.insert(key);
        }
    }
    else if (op == DEL_COMMAND)
    {
        // For del command:
        // 1. Removing it from APPL_DB
        // 2. Update internal caches
        string &runningProfileName = bufferPg.running_profile_name;
        string &configProfileName = bufferPg.configured_profile_name;

        if (!runningProfileName.empty())
        {
            m_bufferProfileLookup[runningProfileName].port_pgs.erase(key);
        }
        if (!configProfileName.empty() && configProfileName != runningProfileName)
        {
            m_bufferProfileLookup[configProfileName].port_pgs.erase(key);
        }

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
        SWSS_LOG_DEBUG("Profile %s has been removed from port %s PG %s", runningProfileName.c_str(), port.c_str(), key.c_str());
        if (m_portPgLookup[port].empty())
        {
            m_portPgLookup.erase(port);
            SWSS_LOG_DEBUG("Profile %s has been removed from port %s on all lossless PG", runningProfileName.c_str(), port.c_str());
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
 * configured entries only while APPL_DB contains dynamically generated entries
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

    // Transform the separator in key from "|" to ":"
    transformSeperator(key);

    string op = kfvOp(tuple);
    if (op == SET_COMMAND)
    {
        vector<FieldValueTuple> fvVector;

        SWSS_LOG_INFO("Inserting entry %s|%s from CONFIG_DB to APPL_DB", name.c_str(), key.c_str());

        for (auto i : kfvFieldsValues(tuple))
        {
            // Transform the separator in values from "|" to ":"
            if (fvField(i) == "pool")
                transformReference(fvValue(i));
            if (fvField(i) == "profile")
                transformReference(fvValue(i));
            if (fvField(i) == "profile_list")
                transformReference(fvValue(i));
            fvVector.emplace_back(fvField(i), fvValue(i));
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
