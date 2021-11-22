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

BufferMgrDynamic::BufferMgrDynamic(DBConnector *cfgDb, DBConnector *stateDb, DBConnector *applDb, const vector<TableConnector> &tables, shared_ptr<vector<KeyOpFieldsValuesTuple>> gearboxInfo, shared_ptr<vector<KeyOpFieldsValuesTuple>> zeroProfilesInfo) :
        Orch(tables),
        m_platform(),
        m_bufferDirections({BUFFER_INGRESS, BUFFER_EGRESS}),
        m_bufferObjectNames({"priority group", "queue"}),
        m_bufferDirectionNames({"ingress", "egress"}),
        m_applDb(applDb),
        m_zeroProfilesLoaded(false),
        m_supportRemoving(true),
        m_cfgDefaultLosslessBufferParam(cfgDb, CFG_DEFAULT_LOSSLESS_BUFFER_PARAMETER),
        m_applBufferPoolTable(applDb, APP_BUFFER_POOL_TABLE_NAME),
        m_applBufferProfileTable(applDb, APP_BUFFER_PROFILE_TABLE_NAME),
        m_applBufferObjectTables({ProducerStateTable(applDb, APP_BUFFER_PG_TABLE_NAME), ProducerStateTable(applDb, APP_BUFFER_QUEUE_TABLE_NAME)}),
        m_applBufferProfileListTables({ProducerStateTable(applDb, APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME), ProducerStateTable(applDb, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME)}),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateBufferMaximumTable(stateDb, STATE_BUFFER_MAXIMUM_VALUE_TABLE),
        m_stateBufferPoolTable(stateDb, STATE_BUFFER_POOL_TABLE_NAME),
        m_stateBufferProfileTable(stateDb, STATE_BUFFER_PROFILE_TABLE_NAME),
        m_applPortTable(applDb, APP_PORT_TABLE_NAME),
        m_portInitDone(false),
        m_bufferPoolReady(false),
        m_bufferObjectsPending(true),
        m_bufferCompletelyInitialized(false),
        m_mmuSizeNumber(0)
{
    SWSS_LOG_ENTER();

    // Initialize the handler map
    initTableHandlerMap();
    parseGearboxInfo(gearboxInfo);
    if (nullptr != zeroProfilesInfo)
        m_zeroPoolAndProfileInfo = *zeroProfilesInfo;

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

    m_platform = platform;

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

    // m_waitApplyAdditionalZeroProfiles represents for how long applying additional zero profiles will be deferred
    // after normal profiles and profiles for configured items have been applied
    // For warm reboot, it is not deferred as the additional zero profiles have been in the APPL_DB
    // In this case, they should be replayed as soon as possible
    // For fast/cold reboot and other initialization flow, it is defered for 30 seconds.
    // This is to accelerate the fast reboot converging time.
    if (WarmStart::isWarmStart())
    {
        m_waitApplyAdditionalZeroProfiles = 0;
        WarmStart::setWarmStartState("buffermgrd", WarmStart::INITIALIZED);
    }
    else
    {
        m_waitApplyAdditionalZeroProfiles = 3;
        WarmStart::setWarmStartState("buffermgrd", WarmStart::WSDISABLED);
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

/*
 * Zero buffer pools and profiles are introduced for reclaiming reserved buffer on unused ports.
 *
 * They are loaded into buffer manager through a json file provided from CLI on a per-platform basis
 * and will be applied to APPL_DB and STATE_DB on the ports once they are admin down.
 * They are loaded into APPL_DB in an order in which they occur in the json file, which means
 * it's vendor's responsibility to guarantee the order reflects the dependency among zero pools and profiles.
 * They are removed from APPL_DB once all ports are admin up.
 * The zero profiles are removed first and then the zero pools. This is to respect the dependency between them.
 *
 * The keys can be in format of:
 *  - <TABLE NAME>|<object name>: represents a zero buffer object, like a zero buffer pool or zero buffer profile
 *    All necessary fields of the object should be provided in the json file according to the type of the object.
 *    For the buffer profiles, if the buffer pools referenced are the normal pools, like {ingress|egress}_{lossless|lossy}_pool,
 *    the zero profile name will be stored in the referenced pool's "zero_profile_name" field for the purpose of
 *    - constructing the zero profile list or providing the zero profiles for PGs or queues
 *    - fetching the zero profile for a certain pool when applying the zero profile for configured items
 *
 *  - control_fields: represents the ids required for reclaiming unused buffers, including:
 *     - pgs_to_apply_zero_profile, represents the PGs on which the zero profiles will be applied for reclaiming unused buffers
 *       If it is not provided, zero profiles will be applied on all PGs.
 *     - ingress_zero_profile, represents the zero buffer profille which will be applied on PGs for reclaiming unused buffer.
 *       Typically, it is provided along with pgs_to_apply_zero_profile.
 *       In case pgs_to_apply_zero_profile is defined but ingress_zero_profile is not provided, the first zero profile on the ingress side in json file will be used.
 *     - queues_to_apply_zero_profile, represents the queues on which the zero profiles will be applied
 *       If it is not provided, zero profiles will be applied on all queues.
 *     - egress_zero_profile, represents the zero buffer profille which will be applied on queues for reclaiming unused buffer.
 *       Typically, it is provided along with queues_to_apply_zero_profile.
 *       In case queues_to_apply_zero_profile is defined but egress_zero_profile is not provided, the first zero profile on the egress side in json file will be used.
 *     - support_removing_buffer_items, represents whether the buffer items are supported to be removed.
 *    The number of queues and PGs are pushed into BUFFER_MAX_PARAM table in STATE_DB at the beginning of ports orchagent
 *    and will be learnt by buffer manager when it's starting.
 */
void BufferMgrDynamic::loadZeroPoolAndProfiles()
{
    for (auto &kfv : m_zeroPoolAndProfileInfo)
    {
        auto &table_key = kfvKey(kfv);

        if (table_key == "control_fields")
        {
            auto &fvs = kfvFieldsValues(kfv);
            for (auto &fv : fvs)
            {
                if (fvField(fv) == "pgs_to_apply_zero_profile")
                {
                    m_bufferObjectIdsToZero[BUFFER_PG] = fvValue(fv);
                }
                else if (fvField(fv) == "ingress_zero_profile")
                {
                    m_bufferZeroProfileName[BUFFER_PG] = fvValue(fv);
                }
                else if (fvField(fv) == "queues_to_apply_zero_profile")
                {
                    m_bufferObjectIdsToZero[BUFFER_QUEUE] = fvValue(fv);
                }
                else if (fvField(fv) == "egress_zero_profile")
                {
                    m_bufferZeroProfileName[BUFFER_QUEUE] = fvValue(fv);
                }
                else if (fvField(fv) == "support_removing_buffer_items")
                {
                    m_supportRemoving = (fvValue(fv) == "yes");
                }
            }

            continue;
        }

        auto const &table = parseObjectNameFromKey(table_key, 0);
        auto const &key = parseObjectNameFromKey(table_key, 1);

        if (table.empty() || key.empty())
        {
            SWSS_LOG_ERROR("Invalid format of key %s for zero profile info, won't initialize it",
                           kfvKey(kfv).c_str());
            return;
        }

        if (table == APP_BUFFER_POOL_TABLE_NAME)
        {
            m_applBufferPoolTable.set(key, kfvFieldsValues(kfv));
            m_stateBufferPoolTable.set(key, kfvFieldsValues(kfv));
            SWSS_LOG_NOTICE("Loaded zero buffer pool %s", key.c_str());
            m_zeroPoolNameSet.insert(key);
        }
        else if (table == APP_BUFFER_PROFILE_TABLE_NAME)
        {
            auto &fvs = kfvFieldsValues(kfv);
            bool poolNotFound = false;
            for (auto &fv : fvs)
            {
                if (fvField(fv) == "pool")
                {
                    const auto &poolName = fvValue(fv);
                    auto poolSearchRef = m_bufferPoolLookup.find(poolName);
                    if (poolSearchRef != m_bufferPoolLookup.end())
                    {
                        auto &poolObj = poolSearchRef->second;
                        if (poolObj.zero_profile_name.empty())
                        {
                            poolObj.zero_profile_name = key;
                            if (m_bufferZeroProfileName[poolObj.direction].empty())
                                m_bufferZeroProfileName[poolObj.direction] = poolObj.zero_profile_name;
                        }
                        else
                        {
                            SWSS_LOG_ERROR("Multiple zero profiles (%s, %s) detected for pool %s, takes the former and ignores the latter",
                                           poolObj.zero_profile_name.c_str(),
                                           key.c_str(),
                                           fvValue(fv).c_str());
                        }
                    }
                    else if (m_zeroPoolNameSet.find(poolName) == m_zeroPoolNameSet.end())
                    {
                        SWSS_LOG_WARN("Profile %s is not loaded as the referenced pool %s is not defined",
                                        key.c_str(),
                                        fvValue(fv).c_str());
                        poolNotFound = true;
                        break;
                    }

                    m_zeroProfiles.emplace_back(key, poolName);
                }
            }
            if (poolNotFound)
            {
                continue;
            }
            m_applBufferProfileTable.set(key, fvs);
            m_stateBufferProfileTable.set(key, fvs);
            SWSS_LOG_NOTICE("Loaded zero buffer profile %s", key.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown keys %s with zero table name %s isn't loaded to APPL_DB", key.c_str(), table.c_str());
            continue;
        }
    }

    // Consistency checking
    // 1. For any buffer pool, if there is no zero profile provided, removing buffer items must be supported
    // because the reserved buffer will be reclaimed by removing buffer items
    // 2. If pgs_to_apply_zero_profile or queues_to_apply_zero_profile is provided, removing buffer items must be supported
    // because the PGs or queues that are not in the ID list will be removed
    bool noReclaiming = false;
    if (!m_supportRemoving)
    {
        for (auto &poolRef: m_bufferPoolLookup)
        {
            if (poolRef.second.zero_profile_name.empty())
            {
                // For any buffer pool, zero profile must be provided
                SWSS_LOG_ERROR("Zero profile is not provided for pool %s while removing buffer items is not supported, reserved buffer can not be reclaimed correctly", poolRef.first.c_str());
                noReclaiming = true;
            }
        }

        if (!m_bufferObjectIdsToZero[BUFFER_PG].empty() || !m_bufferObjectIdsToZero[BUFFER_QUEUE].empty())
        {
            SWSS_LOG_ERROR("Unified IDs of queues or priority groups specified while removing buffer items is not supported, reserved buffer can not be reclaimed correctly");
            noReclaiming = true;
        }
    }

    if (noReclaiming)
    {
        unloadZeroPoolAndProfiles();
        m_zeroPoolAndProfileInfo.clear();
    }
    else
    {
        m_zeroProfilesLoaded = true;
    }
}

void BufferMgrDynamic::unloadZeroPoolAndProfiles()
{
    for (auto &zeroProfile : m_zeroProfiles)
    {
        auto &zeroProfileName = zeroProfile.first;
        auto &poolReferenced = zeroProfile.second;

        auto poolSearchRef = m_bufferPoolLookup.find(poolReferenced);
        if (poolSearchRef != m_bufferPoolLookup.end())
        {
            auto &poolObj = poolSearchRef->second;
            poolObj.zero_profile_name.clear();
        }
        m_applBufferProfileTable.del(zeroProfileName);
        m_stateBufferProfileTable.del(zeroProfileName);
        SWSS_LOG_NOTICE("Unloaded zero buffer profile %s", zeroProfileName.c_str());
    }

    m_zeroProfiles.clear();

    for (auto &zeroPool : m_zeroPoolNameSet)
    {
        m_applBufferPoolTable.del(zeroPool);
        m_stateBufferPoolTable.del(zeroPool);
        SWSS_LOG_NOTICE("Unloaded zero buffer pool %s", zeroPool.c_str());
    }

    m_zeroPoolNameSet.clear();

    m_zeroProfilesLoaded = false;
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
    m_bufferTableHandlerMap.insert(buffer_handler_pair(STATE_PORT_TABLE_NAME, &BufferMgrDynamic::handlePortStateTable));

    m_bufferSingleItemHandlerMap.insert(buffer_single_item_handler_pair(CFG_BUFFER_QUEUE_TABLE_NAME, &BufferMgrDynamic::handleSingleBufferQueueEntry));
    m_bufferSingleItemHandlerMap.insert(buffer_single_item_handler_pair(CFG_BUFFER_PG_TABLE_NAME, &BufferMgrDynamic::handleSingleBufferPgEntry));
    m_bufferSingleItemHandlerMap.insert(buffer_single_item_handler_pair(CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, &BufferMgrDynamic::handleSingleBufferPortIngressProfileListEntry));
    m_bufferSingleItemHandlerMap.insert(buffer_single_item_handler_pair(CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, &BufferMgrDynamic::handleSingleBufferPortEgressProfileListEntry));
}

// APIs to handle variant kinds of keys

// Transform key from CONFIG_DB format to APPL_DB format
void BufferMgrDynamic::transformSeperator(string &name)
{
    size_t pos;
    while ((pos = name.find("|")) != string::npos)
        name.replace(pos, 1, ":");
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

string BufferMgrDynamic::getDynamicProfileName(const string &speed, const string &cable, const string &mtu, const string &threshold, const string &gearbox_model, long lane_count)
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

    if (m_platform == "mellanox")
    {
        if ((speed != "400000") && (lane_count == 8))
        {
            // On Mellanox platform, ports with 8 lanes have different(double) xon value then other ports
            // For ports at speed other than 400G can have
            // - 8 lanes, double xon
            // - other number of lanes, normal xon
            // So they can not share the same buffer profiles.
            // An extra "_8lane" is added to the name of buffer profiles to distinguish both scenarios
            // Eg.
            // - A 100G port with 8 lanes will use buffer profile "pg_profile_100000_5m_8lane_profile"
            // - A 100G port with 4 lanes will use buffer profile "pg_profile_100000_5m_profile"
            // Currently, 400G ports can only have 8 lanes. So we don't add this to the profile
            buffer_profile_key = buffer_profile_key + "_8lane";
        }
    }

    return buffer_profile_key + "_profile";
}

string BufferMgrDynamic::getMaxSpeedFromList(string speedList)
{
    auto &&speedVec = tokenize(speedList, ',');
    unsigned long maxSpeedNum = 0, speedNum;
    for (auto &speedStr : speedVec)
    {
        speedNum = atol(speedStr.c_str());
        if (speedNum > maxSpeedNum)
        {
            maxSpeedNum = speedNum;
        }
    }

    return to_string(maxSpeedNum);
}

string BufferMgrDynamic::getPgPoolMode()
{
    return m_bufferPoolLookup[INGRESS_LOSSLESS_PG_POOL_NAME].mode;
}

// Conduct the effective speed and compare the new value against the old one
// Return true if they differ and false otherwise, meaning the headroom should be updated accordingly.
//
// The way to conduct the effective speed for headroom calculating
// If auto_neg enabled on the port
//   if adv_speed configured
//     max(adv_speed) => new effective speed
//   elif sup_speed learnt
//     max(sup_speed) => new effective speed
//   elif PORT READY
//     INITIALIZING => port.state
// else
//   port speed will be the new effective speed
// return whether effective speed has been updated
bool BufferMgrDynamic::needRefreshPortDueToEffectiveSpeed(port_info_t &portInfo, string &portName)
{
    string newEffectiveSpeed;

    if (portInfo.auto_neg)
    {
        if (isNonZero(portInfo.adv_speeds))
        {
            newEffectiveSpeed = move(getMaxSpeedFromList(portInfo.adv_speeds));
            SWSS_LOG_INFO("Port %s: maximum configured advertised speed (%s from %s) is taken as the effective speed",
                          portName.c_str(), portInfo.effective_speed.c_str(), portInfo.adv_speeds.c_str());
        }
        else if (isNonZero(portInfo.supported_speeds))
        {
            newEffectiveSpeed = move(getMaxSpeedFromList(portInfo.supported_speeds));
            SWSS_LOG_INFO("Port %s: maximum supported speed (%s from %s) is taken as the effective speed",
                          portName.c_str(), portInfo.effective_speed.c_str(), portInfo.supported_speeds.c_str());
        }
        else if (portInfo.state == PORT_READY)
        {
            portInfo.state = PORT_INITIALIZING;
            SWSS_LOG_NOTICE("Port %s: unable to deduct the effective speed because auto negotiation is enabled but neither configured advertised speed nor supported speed is available", portName.c_str());
        }
    }
    else
    {
        newEffectiveSpeed = portInfo.speed;
        SWSS_LOG_INFO("Port %s: speed (%s) is taken as the effective speed", portName.c_str(), portInfo.effective_speed.c_str());
    }

    bool effectiveSpeedChanged = (newEffectiveSpeed != portInfo.effective_speed);
    if (effectiveSpeedChanged)
    {
        portInfo.effective_speed = newEffectiveSpeed;
    }

    return effectiveSpeedChanged;
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
    argv.emplace_back(to_string(headroom.lane_count));

    try
    {
        auto ret = swss::runRedisScript(*m_applDb, m_headroomSha, keys, argv);

        if (ret.empty())
        {
            SWSS_LOG_WARN("Failed to calculate headroom for %s", headroom.name.c_str());
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

        if (!m_bufferPoolReady)
        {
            // In case all buffer pools have a configured size,
            // The m_bufferPoolReady will be set to true
            // It can happen on vs.
            bool hasDynamicSizePool = false, hasBufferPool = false;
            for (auto &poolRef : m_bufferPoolLookup)
            {
                hasBufferPool = true;
                if (poolRef.second.dynamic_size)
                {
                    hasDynamicSizePool = true;
                }
            }

            if (!hasBufferPool)
            {
                SWSS_LOG_INFO("No shared buffer pool configured, skip calculating shared buffer pool size");
                return;
            }

            if (hasBufferPool && !hasDynamicSizePool)
            {
                m_bufferPoolReady = true;
                SWSS_LOG_NOTICE("No pool requires calculating size dynamically. All buffer pools are ready");
            }
        }

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
            SWSS_LOG_WARN("Failed to recalculate the shared buffer pool size");
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

    if (!m_bufferPoolReady)
        m_bufferPoolReady = true;
}

void BufferMgrDynamic::checkSharedBufferPoolSize(bool force_update_during_initialization = false)
{
    // PortInitDone indicates all steps of port initialization has been done
    // Only after that does the buffer pool size update starts
    if (!m_portInitDone && !force_update_during_initialization)
    {
        SWSS_LOG_INFO("Skip buffer pool updating during initialization");
        return;
    }

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
            if (!m_bufferPoolReady)
            {
                // It's something like a placeholder especially for warm reboot flow
                // without all buffer pools created, buffer profiles are unable to be created,
                // which in turn causes buffer pgs and buffer queues unable to be created,
                // which prevents the port from being ready and eventually fails the warm reboot
                // After the buffer pools are created for the first time, we won't touch it
                // until portInitDone
                // Eventually, the correct values will pushed to APPL_DB and then ASIC_DB
                recalculateSharedBufferPool();
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

    fvVector.emplace_back("type", m_bufferDirectionNames[pool.direction]);

    if (!pool.xoff.empty())
        fvVector.emplace_back("xoff", pool.xoff);

    fvVector.emplace_back("mode", pool.mode);

    fvVector.emplace_back("size", pool.total_size);

    m_applBufferPoolTable.set(name, fvVector);

    m_stateBufferPoolTable.set(name, fvVector);
}

void BufferMgrDynamic::updateBufferProfileToDb(const string &name, const buffer_profile_t &profile)
{
    if (!m_bufferPoolReady)
    {
        SWSS_LOG_NOTICE("Buffer pools are not ready when configuring buffer profile %s, pending", name.c_str());
        m_bufferObjectsPending = true;
        return;
    }

    vector<FieldValueTuple> fvVector;
    string mode = getPgPoolMode();

    // profile threshold field name
    mode += "_th";

    if (profile.lossless)
    {
        fvVector.emplace_back("xon", profile.xon);
        if (!profile.xon_offset.empty()) {
            fvVector.emplace_back("xon_offset", profile.xon_offset);
        }
        fvVector.emplace_back("xoff", profile.xoff);
    }
    fvVector.emplace_back("size", profile.size);
    fvVector.emplace_back("pool", profile.pool_name);
    fvVector.emplace_back(mode, profile.threshold);

    m_applBufferProfileTable.set(name, fvVector);
    m_stateBufferProfileTable.set(name, fvVector);
}

// Database operation
// Set/remove BUFFER_PG or BUFFER_QUEUE table entry
void BufferMgrDynamic::updateBufferObjectToDb(const string &key, const string &profile, bool add, buffer_direction_t dir=BUFFER_PG)
{
    auto &table = m_applBufferObjectTables[dir];
    const auto &objType = m_bufferObjectNames[dir];

    if (add)
    {
        if (!m_bufferPoolReady)
        {
            SWSS_LOG_NOTICE("Buffer pools are not ready when configuring buffer %s %s, pending", objType.c_str(), key.c_str());
            m_bufferObjectsPending = true;
            return;
        }

        vector<FieldValueTuple> fvVector;
        fvVector.emplace_back(buffer_profile_field_name, profile);

        table.set(key, fvVector);
    }
    else
    {
        table.del(key);
    }
}

void BufferMgrDynamic::updateBufferObjectListToDb(const string &key, const string &profileList, buffer_direction_t dir)
{
    auto &table = m_applBufferProfileListTables[dir];
    const auto &direction = m_bufferDirectionNames[dir];

    if (!m_bufferPoolReady)
    {
        SWSS_LOG_NOTICE("Buffer pools are not ready when configuring buffer %s profile list %s, pending", direction.c_str(), key.c_str());
        m_bufferObjectsPending = true;
        return;
    }

    vector<FieldValueTuple> fvVector;

    fvVector.emplace_back(buffer_profile_list_field_name, profileList);

    table.set(key, fvVector);
}

// We have to check the headroom ahead of applying them
task_process_status BufferMgrDynamic::allocateProfile(const string &speed, const string &cable_len, const string &mtu, const string &threshold, const string &gearbox_model, long lane_count, string &profile_name)
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
        profile.lane_count = lane_count;
        profile.pool_name = INGRESS_LOSSLESS_PG_POOL_NAME;

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

/*
 * The following functions are defnied for reclaiming reserved buffers
 *  - constructZeroProfileListFromNormalProfileList
 *  - reclaimReservedBufferForPort
 *  - removeSupportedButNotConfiguredItemsOnPort
 *  - applyNormalBufferObjectsOnPort, handling queues and profile lists.
 *    The priority groups are handle by refreshPgsForPort
 *
 * The overall logic of reclaiming reserved buffers is handled in handlePortTable:
 * Shutdown flow (to reclaim unused buffer):
 *  1. reclaimReservedBufferForPort
 * Start up flow (to reserved buffer for the port)
 *  1. removeSupportedButNotConfiguredItemsOnPort
 *  2. applyNormalBufferObjectsOnPort
 *  3. refreshPgsForPort
 */

/*
 * constructZeroProfileListFromNormalProfileList
 *  Tool function for constructing zero profile list from normal profile list.
 *  There should be one buffer profile for each buffer pool on each side (ingress/egress) in the profile_list
 *  in BUFFER_PORT_INGRESS_PROFILE_LIST and BUFFER_PORT_EGRESS_PROFILE_LIST table.
 *  For example, on ingress side, there are ingress_lossless_profile and ingress_lossy_profile in the profile list
 *  for buffer pools ingress_lossless_pool and ingress_lossy_pool respectively.
 *  There should be one zero profile for each pool in the profile_list when reclaiming buffer on a port as well.
 *  The arguments is the normal profile list and the port.
 *  The logic is:
 *    For each buffer profile in the list
 *    1. Fetch the buffer pool referenced by the profile
 *    2. Fetch the zero buffer profile of the buffer pool. The pool is skipped in case there is no zero buffer profile defined for it.
 *    3. Construct the zero buffer profile by joining all zero buffer profiles, separating by ","
 */
string BufferMgrDynamic::constructZeroProfileListFromNormalProfileList(const string &normalProfileList, const string &port)
{
    string zeroProfileNameList;

    auto profileRefs = tokenize(normalProfileList, ',');
    for (auto &profileName : profileRefs)
    {
        const auto &zeroProfile = fetchZeroProfileFromNormalProfile(profileName);
        if (!zeroProfile.empty())
        {
            zeroProfileNameList += zeroProfile + ",";
        }
        else
        {
            SWSS_LOG_WARN("Unable to apply zero profile for profile %s on port %s because no zero profile configured for the pool",
                          profileName.c_str(),
                          port.c_str());
        }
    }

    if (!zeroProfileNameList.empty())
        zeroProfileNameList.pop_back();

    return zeroProfileNameList;
}

/*
 * removeSupportedButNotConfiguredItemsOnPort
 * Remove the supported-but-not-configured buffer items from the port.
 * They were applied when the port was shut down.
 * Called when a port is started up.
 *
 * Arguments:
 *  portInfo, represents the port information of the port
 *  portName, represents the name of the port
 * Return:
 *  None.
 */
void BufferMgrDynamic::removeSupportedButNotConfiguredItemsOnPort(port_info_t &portInfo, const string &portName)
{
    auto const &portPrefix = portName + delimiter;

    for (auto dir : m_bufferDirections)
    {
        if (!portInfo.supported_but_not_configured_buffer_objects[dir].empty())
        {
            for (auto &it: portInfo.supported_but_not_configured_buffer_objects[dir])
            {
                m_applBufferObjectTables[dir].del(portPrefix + it);
            }
            portInfo.supported_but_not_configured_buffer_objects[dir].clear();
        }
    }
}

/*
 * applyNormalBufferObjectsOnPort
 * Apply normal buffer profiles on buffer queues, and buffer profile lists.
 * The buffer priority group will be handled by refreshPgsForPort
 * Called when a port is started up.
 *
 * Arguments:
 *  port, represents the name of the port
 * Return:
 *  None.
 */
void BufferMgrDynamic::applyNormalBufferObjectsOnPort(const string &port)
{
    vector<FieldValueTuple> fvVector;
    auto &portQueues = m_portQueueLookup[port];

    for (auto &queue : portQueues)
    {
        SWSS_LOG_NOTICE("Profile %s has been applied on queue %s", queue.second.running_profile_name.c_str(), queue.first.c_str());
        updateBufferObjectToDb(queue.first, queue.second.running_profile_name, true, BUFFER_QUEUE);
    }

    for (auto dir : m_bufferDirections)
    {
        auto &profileList = m_portProfileListLookups[dir][port];
        if (!profileList.empty())
        {
            fvVector.emplace_back(buffer_profile_list_field_name, profileList);
            m_applBufferProfileListTables[dir].set(port, fvVector);
            fvVector.clear();
        }
    }
}

string &BufferMgrDynamic::fetchZeroProfileFromNormalProfile(const string &profile)
{
    auto const &profileRef = m_bufferProfileLookup.find(profile);
    auto const &poolName = (profileRef == m_bufferProfileLookup.end() ? INGRESS_LOSSLESS_PG_POOL_NAME : profileRef->second.pool_name);
    auto &poolInfo = m_bufferPoolLookup[poolName];

    return poolInfo.zero_profile_name;
}

bool BufferMgrDynamic::isReadyToReclaimBufferOnPort(const string &port)
{
    auto &portInfo = m_portInfoLookup[port];

    for (auto dir : m_bufferDirections)
    {
        if (m_bufferObjectIdsToZero[dir].empty() && 0 == portInfo.maximum_buffer_objects[dir])
        {
            SWSS_LOG_NOTICE("Maximum supported priority groups and queues have not been populated in STATE_DB for port %s, reclaiming reserved buffer deferred", port.c_str());
            return false;
        }
    }

    return true;
}

/*
 * reclaimReservedBufferForPort
 * Called when a port is admin down
 * Reserved buffer is reclaimed by applying zero profiles on the buffer item or just removing them.
 *
 * Parameters:
 *  - port: the name of the port
 * Return:
 *  - task_process_status
 *
 * Purpose:
 * 1. Reclaim all the priority groups and queues of the port from APPL_DB
 * 2. Remove all the buffer profiles that are dynamically calculated and no longer referenced
 * 3. Reclaim the ingress/egress profile list of the port form APPL_DB
 *
 * The flow:
 * 1. Load the zero pools and profiles into APPL_DB if they have been provided but not loaded.
 * 2. Handle priority group, and queues:
 *    Two modes for them to be reclaimed:
 *    - Apply zero buffer profiles on all configured and supported-but-not-configured items.
 *      Eg.
 *      - 16 queues supported, 0-2, 5-6 are configured as lossy and 3-4 are configured as lossless
 *      - Zero profiles will be applied on
 *        - 0-2, 5-6, 7-15: egress_lossy_zero_profile
 *        - 3-4: egress_lossless_zero_profile
 *    - Apply zero buffer profiles on items specified by vendor and remove all other items.
 *      Eg.
 *      - 8 PGs supported, 0 is configured as lossy and 3-4 are configured as lossless
 *      - PGs to be applied zero profile on is 0, 3-4 will be removed
 *    Queues and priority groups share the common logic.
 * 3. Handle ingress and egress buffer profile list, which shared the common logic:
 *    - Construct the zero buffer profile list from the normal buffer profile list.
 *    - Apply the zero buffer profile list on the port.
 */
template<class T> task_process_status BufferMgrDynamic::reclaimReservedBufferForPort(const string &port, T &portObjectLookup, buffer_direction_t dir)
{
    auto &portBufferObjects = portObjectLookup[port];
    auto &portInfo = m_portInfoLookup[port];
    set<string> profilesToBeReleased;
    const string &portKeyPrefix = port + delimiter;
    const string &objectTypeName = m_bufferObjectNames[dir];

    if (!m_zeroPoolAndProfileInfo.empty() && (!m_zeroProfilesLoaded))
    {
        loadZeroPoolAndProfiles();
    }

    bool applyZeroProfileOnSpecifiedObjects = !m_bufferObjectIdsToZero[dir].empty();

    SWSS_LOG_NOTICE("Reclaiming buffer reserved for all %s(s) from port %s", objectTypeName.c_str(), port.c_str());

    unsigned long objectsMap = (1 << portInfo.maximum_buffer_objects[dir]) - 1;
    for (auto &it : portBufferObjects)
    {
        auto &key = it.first;
        auto &portObject = it.second;
        const string &runningProfile = portObject.running_profile_name;
        const string &zeroProfile = fetchZeroProfileFromNormalProfile(runningProfile);

        SWSS_LOG_INFO("Reclaiming buffer reserved for %s %s", objectTypeName.c_str(), key.c_str());

        if (!applyZeroProfileOnSpecifiedObjects)
        {
            // Apply zero profiles on each configured buffer objects and supported-but-not-configured buffer objects
            // Fetch the zero profile of the pool referenced by the configured profile
            if (!zeroProfile.empty())
            {
                SWSS_LOG_INFO("Applying zero profile %s on %s %s", zeroProfile.c_str(), objectTypeName.c_str(), key.c_str());
                updateBufferObjectToDb(key, zeroProfile, true, dir);
            }
            else
            {
                // No zero profile defined for the pool. Reclaim buffer by removing the buffer object
                SWSS_LOG_INFO("Zero profile is not defined for profile %s, reclaim reserved buffer by removing %s %s", runningProfile.c_str(), objectTypeName.c_str(), key.c_str());
                updateBufferObjectToDb(key, runningProfile, false, dir);
            }

            objectsMap ^= generateBitMapFromIdsStr(parseObjectNameFromKey(key, 1));
        }
        else
        {
            // Apply the zero profile on specified buffer objects only.
            // Remove each buffer object first
            // In this case, removing must be supported (checked when the json was loaded)
            SWSS_LOG_INFO("Zero profile will be applied on %s. Remove %s %s first", m_bufferObjectIdsToZero[dir].c_str(), objectTypeName.c_str(), key.c_str());
            updateBufferObjectToDb(key, runningProfile, false, dir);
        }

        if (dir == BUFFER_PG && !runningProfile.empty())
        {
            m_bufferProfileLookup[runningProfile].port_pgs.erase(key);
            profilesToBeReleased.insert(runningProfile);
            portObject.running_profile_name.clear();
        }
    }

    if (applyZeroProfileOnSpecifiedObjects)
    {
        updateBufferObjectToDb(portKeyPrefix + m_bufferObjectIdsToZero[dir], m_bufferZeroProfileName[dir], true, dir);
    }
    else if (!m_bufferZeroProfileName[dir].empty())
    {
        // Apply zero profiles on supported-but-not-configured buffer objects
        portInfo.supported_but_not_configured_buffer_objects[dir] = generateIdListFromMap(objectsMap, portInfo.maximum_buffer_objects[dir]);
        if (m_waitApplyAdditionalZeroProfiles == 0)
        {
            for(auto &it: portInfo.supported_but_not_configured_buffer_objects[dir])
            {
                updateBufferObjectToDb(portKeyPrefix + it, m_bufferZeroProfileName[dir], true, dir);
            }
        }
        else
        {
            m_pendingSupportedButNotConfiguredPorts[dir].insert(port);
        }
    }

    // Remove the old profile which is probably not referenced anymore.
    if (!profilesToBeReleased.empty())
    {
        for (auto &oldProfile : profilesToBeReleased)
        {
            releaseProfile(oldProfile);
        }
    }

    vector<FieldValueTuple> fvVector;

    SWSS_LOG_NOTICE("Reclaiming buffer reserved for ingress profile list from port %s", port.c_str());
    const auto &profileList = m_portProfileListLookups[dir][port];
    if (!profileList.empty())
    {
        const string &zeroIngressProfileNameList = constructZeroProfileListFromNormalProfileList(profileList, port);
        fvVector.emplace_back(buffer_profile_list_field_name, zeroIngressProfileNameList);
        m_applBufferProfileListTables[dir].set(port, fvVector);
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
    long laneCount = portInfo.lane_count;
    bool isHeadroomUpdated = false;
    buffer_pg_lookup_t &portPgs = m_portPgLookup[port];
    set<string> profilesToBeReleased;

    if (portInfo.state == PORT_ADMIN_DOWN)
    {
        SWSS_LOG_INFO("Nothing to be done since the port %s is administratively down", port.c_str());
        return task_process_status::task_success;
    }

    if (!m_bufferPoolReady)
    {
        SWSS_LOG_INFO("Nothing to be done since the buffer pool is not ready");
        return task_process_status::task_success;
    }

    SWSS_LOG_NOTICE("Refresh priority groups for port %s", port.c_str());

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
            newProfile = getDynamicProfileName(speed, cable_length, mtu, threshold, gearbox_model, laneCount);
            auto rc = allocateProfile(speed, cable_length, mtu, threshold, gearbox_model, laneCount, newProfile);
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
        updateBufferObjectToDb(key, newProfile, true);
        isHeadroomUpdated = true;
    }

    if (isHeadroomUpdated)
    {
        checkSharedBufferPoolSize();
    }
    else
    {
        SWSS_LOG_DEBUG("Nothing to do for port %s since no PG configured on it", port.c_str());
    }

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

    if (m_portInitDone)
    {
        checkSharedBufferPoolSize();
    }
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
        task_status = refreshPgsForPort(port, portInfo.effective_speed, portInfo.cable_length, portInfo.mtu, pg_key);
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
            task_status = refreshPgsForPort(port, portInfo.effective_speed, portInfo.cable_length, portInfo.mtu, pg_key);
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
    updateBufferObjectToDb(pg_key, null_str, false);

    SWSS_LOG_NOTICE("Remove BUFFER_PG %s (profile %s, %s)", pg_key.c_str(), bufferPg.running_profile_name.c_str(), bufferPg.configured_profile_name.c_str());

    // Recalculate pool size
    checkSharedBufferPoolSize();

    if (portInfo.state != PORT_ADMIN_DOWN)
    {
        if (!portInfo.effective_speed.empty() && !portInfo.cable_length.empty())
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

            if (port.state != PORT_READY)
                continue;

            rc = refreshPgsForPort(portName, port.effective_speed, port.cable_length, port.mtu);
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

/*
 * handleBufferMaxParam, handles the BUFFER_MAX_PARAMETER table which contains some thresholds related to buffer.
 * The available fields depend on the key of the item:
 * 1. "global", available keys:
 *    - mmu_size, represents the maximum buffer size of the chip
 * 2. "<port name>", available keys:
 *    - max_priority_groups, represents the maximum number of priority groups of the port.
 *      It is pushed into STATE_DB by ports orchagent when it starts and will be used to generate the default priority groups to reclaim.
 *      When reserved buffer is reclaimed for a port, and no priority group IDs specified in the "control_fields" in json file,
 *      the default priority groups will be used to apply zero buffer profiles on all priority groups.
 *    - max_queues, represents the maximum number of queues of the port.
 *      It is used in the same way as max_priority_groups.
 *    - max_headroom_size, represents the maximum headroom size of the port.
 *      It is used for checking whether the accumulative headroom of the port exceeds the port's threshold
 *      before applying a new priority group on a port or changing an existing buffer profile.
 *      It is referenced by lua plugin "check headroom size" only and will not be handled in this function.
 */
task_process_status BufferMgrDynamic::handleBufferMaxParam(KeyOpFieldsValuesTuple &tuple)
{
    string &op = kfvOp(tuple);
    string &key = kfvKey(tuple);

    if (op == SET_COMMAND)
    {
        if (key != "global")
        {
            const auto &portSearchRef = m_portInfoLookup.find(key);
            if (portSearchRef == m_portInfoLookup.end())
            {
                SWSS_LOG_INFO("BUFFER_MAX_PARAM: Port %s is not configured, need retry", key.c_str());
                return task_process_status::task_need_retry;
            }

            auto &portInfo = portSearchRef->second;
            for (auto i : kfvFieldsValues(tuple))
            {
                auto &value = fvValue(i);
                if (fvField(i) == "max_priority_groups")
                {
                    auto pgCount = atol(value.c_str());
                    if (pgCount <= 0)
                    {
                        SWSS_LOG_ERROR("BUFFER_MAX_PARAM: Invaild priority group count %s of port %s", value.c_str(), key.c_str());
                        return task_process_status::task_failed;
                    }

                    SWSS_LOG_INFO("BUFFER_MAX_PARAM: Got port %s's max priority group %s", key.c_str(), value.c_str());

                    portInfo.maximum_buffer_objects[BUFFER_PG] = (sai_uint32_t)pgCount;
                }
                else if (fvField(i) == "max_queues")
                {
                    auto queueCount = atol(value.c_str());
                    if (queueCount <= 0)
                    {
                        SWSS_LOG_ERROR("BUFFER_MAX_PARAM: Invaild queue count %s of port %s", value.c_str(), key.c_str());
                        return task_process_status::task_failed;
                    }

                    SWSS_LOG_INFO("BUFFER_MAX_PARAM: Got port %s's max queue %s", key.c_str(), value.c_str());

                    portInfo.maximum_buffer_objects[BUFFER_QUEUE] = (sai_uint32_t)queueCount;
                }
            }
        }
        else
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
            string &effectiveSpeed = portInfo.effective_speed;
            string &mtu = portInfo.mtu;

            SWSS_LOG_DEBUG("Handling CABLE_LENGTH table field %s length %s", port.c_str(), cable_length.c_str());
            SWSS_LOG_DEBUG("Port Info for %s before handling %s %s %s",
                           port.c_str(),
                           portInfo.effective_speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());

            if (portInfo.cable_length == cable_length)
            {
                continue;
            }

            portInfo.cable_length = cable_length;
            if (effectiveSpeed.empty())
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
                task_status = refreshPgsForPort(port, effectiveSpeed, cable_length, mtu);
                break;

            case PORT_READY:
                task_status = refreshPgsForPort(port, effectiveSpeed, cable_length, mtu);
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
                           portInfo.effective_speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());
        }
    }

    if (failed_item_count > 0)
    {
        return task_process_status::task_failed;
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handlePortStateTable(KeyOpFieldsValuesTuple &tuple)
{
    auto &port = kfvKey(tuple);
    string op = kfvOp(tuple);

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "supported_speeds")
            {
                auto &portInfo = m_portInfoLookup[port];
                if (fvValue(i) != portInfo.supported_speeds)
                {
                    portInfo.supported_speeds = fvValue(i);
                    SWSS_LOG_INFO("Port %s: supported speeds updated to %s", port.c_str(), portInfo.supported_speeds.c_str());
                    if (portInfo.auto_neg && needRefreshPortDueToEffectiveSpeed(portInfo, port))
                    {
                        if (isNonZero(portInfo.cable_length) && portInfo.state != PORT_ADMIN_DOWN)
                        {
                            portInfo.state = PORT_READY;
                            refreshPgsForPort(port, portInfo.effective_speed, portInfo.cable_length, portInfo.mtu);
                        }
                    }
                }
            }
        }
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
    bool effective_speed_updated = false, mtu_updated = false, admin_status_updated = false, admin_up = false;
    bool need_check_speed = false;
    bool first_time_create = (m_portInfoLookup.find(port) == m_portInfoLookup.end());

    SWSS_LOG_DEBUG("Processing command:%s PORT table key %s", op.c_str(), port.c_str());

    port_info_t &portInfo = m_portInfoLookup[port];

    SWSS_LOG_DEBUG("Port Info for %s before handling %s %s %s",
                   port.c_str(),
                   portInfo.effective_speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());

    task_process_status task_status = task_process_status::task_success;

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "lanes")
            {
                auto &lanes = fvValue(i);
                portInfo.lane_count = count(lanes.begin(), lanes.end(), ',') + 1;
            }
            else if (fvField(i) == "speed")
            {
                if (fvValue(i) != portInfo.speed)
                {
                    need_check_speed = true;
                    auto old_speed = move(portInfo.speed);
                    portInfo.speed = fvValue(i);
                    SWSS_LOG_INFO("Port %s: speed updated from %s to %s", port.c_str(), old_speed.c_str(), portInfo.speed.c_str());
                }
            }
            else if (fvField(i) == "mtu")
            {
                if (fvValue(i) != portInfo.mtu)
                {
                    auto old_mtu = move(portInfo.mtu);
                    mtu_updated = true;
                    portInfo.mtu = fvValue(i);
                    SWSS_LOG_INFO("Port %s: MTU updated from %s to %s", port.c_str(), old_mtu.c_str(), portInfo.mtu.c_str());
                }
            }
            else if (fvField(i) == "admin_status")
            {
                admin_up = (fvValue(i) == "up");
                auto old_admin_up = (portInfo.state != PORT_ADMIN_DOWN);
                admin_status_updated = (admin_up != old_admin_up);
            }
            else if (fvField(i) == "adv_speeds")
            {
                if (fvValue(i) != portInfo.adv_speeds)
                {
                    auto old_adv_speeds = move(portInfo.adv_speeds);
                    if (fvValue(i) == "all")
                    {
                        portInfo.adv_speeds.clear();
                    }
                    else
                    {
                        portInfo.adv_speeds = fvValue(i);
                    }
                    need_check_speed = true;
                    SWSS_LOG_INFO("Port %s: advertised speed updated from %s to %s", port.c_str(), old_adv_speeds.c_str(), portInfo.adv_speeds.c_str());
                }
            }
            else if (fvField(i) == "autoneg")
            {
                auto auto_neg = (fvValue(i) == "on");
                if (auto_neg != portInfo.auto_neg)
                {
                    portInfo.auto_neg = auto_neg;
                    need_check_speed = true;
                    SWSS_LOG_INFO("Port %s: auto negotiation %s", port.c_str(), (portInfo.auto_neg ? "enabled" : "disabled"));
                }
            }
        }

        if (need_check_speed && needRefreshPortDueToEffectiveSpeed(portInfo, port))
        {
            effective_speed_updated = true;
        }

        string &cable_length = portInfo.cable_length;
        string &mtu = portInfo.mtu;
        string &effective_speed = portInfo.effective_speed;

        bool need_refresh_all_buffer_objects = false, need_handle_admin_down = false, was_admin_down = false;

        if (effective_speed_updated || mtu_updated)
        {
            if (!cable_length.empty() && !effective_speed.empty())
            {
                SWSS_LOG_INFO("Updating BUFFER_PG for port %s due to effective speed and/or MTU updated", port.c_str());

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
                    need_refresh_all_buffer_objects = true;
                    break;

                case PORT_READY:
                    need_refresh_all_buffer_objects = true;
                    break;

                case PORT_ADMIN_DOWN:
                    SWSS_LOG_INFO("Nothing to be done when port %s's effective speed or cable length updated since the port is administratively down", port.c_str());
                    break;

                default:
                    SWSS_LOG_ERROR("Port %s: invalid port state %d when handling port update", port.c_str(), portInfo.state);
                    break;
                }

                SWSS_LOG_DEBUG("Port Info for %s after handling effective speed %s cable %s gb %s",
                               port.c_str(),
                               portInfo.effective_speed.c_str(), portInfo.cable_length.c_str(), portInfo.gearbox_model.c_str());
            }
            else
            {
                SWSS_LOG_WARN("Cable length or effective speed for %s hasn't been configured yet, unable to calculate headroom", port.c_str());
                // We don't retry here because it doesn't make sense until both cable length and speed are configured.
            }
        }

        if (admin_status_updated)
        {
            if (admin_up)
            {
                if (!portInfo.effective_speed.empty() && !portInfo.cable_length.empty())
                    portInfo.state = PORT_READY;
                else
                    portInfo.state = PORT_INITIALIZING;

                need_refresh_all_buffer_objects = true;
                was_admin_down = true;
            }
            else
            {
                portInfo.state = PORT_ADMIN_DOWN;

                need_handle_admin_down = true;
            }

            SWSS_LOG_INFO("Recalculate shared buffer pool size due to port %s's admin_status updated to %s",
                          port.c_str(), (admin_up ? "up" : "down"));
        }
        else if (first_time_create)
        {
            // The port is initialized as PORT_ADMIN_DOWN state.
            // The admin status not updated after port created means the port is still in admin down state
            // We need to apply zero buffers accordingly
            need_handle_admin_down = true;
        }

        // In case both need_handle_admin_down and need_refresh_all_buffer_objects are true, the need_handle_admin_down will take effect.
        // This can happen when both effective speed (or mtu) is changed and the admin_status is down.
        // In this case, we just need record the new effective speed (or mtu) but don't need to refresh all PGs on the port since the port is administratively down
        if (need_handle_admin_down)
        {
            m_adminDownPorts.insert(port);

            if (!m_bufferPoolReady)
            {
                // Applying zero profiles requires all the buffer pools referenced by zero profiles to be ready.
                // In case buffer pools haven't been ready yet, the procedure has to be deferred.
                m_pendingApplyZeroProfilePorts.insert(port);
                SWSS_LOG_NOTICE("Admin-down port %s is not handled for now because buffer pools are not configured yet", port.c_str());
            }
            else
            {
                if (isReadyToReclaimBufferOnPort(port))
                {
                    reclaimReservedBufferForPort(port, m_portPgLookup, BUFFER_PG);
                    reclaimReservedBufferForPort(port, m_portQueueLookup, BUFFER_QUEUE);
                    checkSharedBufferPoolSize();
                }
                else
                {
                    m_pendingApplyZeroProfilePorts.insert(port);
                    SWSS_LOG_NOTICE("Admin-down port %s is not handled for now because maximum numbers of queues and PGs have not been populated into STATE_DB", port.c_str());
                    // Return task_success as the port has been inserted into m_pendingApplyZeroProfilePorts
                }
            }

            task_status = task_process_status::task_success;
        }
        else if (need_refresh_all_buffer_objects)
        {
            if (was_admin_down)
            {
                removeSupportedButNotConfiguredItemsOnPort(portInfo, port);
                applyNormalBufferObjectsOnPort(port);
                m_adminDownPorts.erase(port);
                m_pendingApplyZeroProfilePorts.erase(port);
                if (m_adminDownPorts.empty())
                    unloadZeroPoolAndProfiles();
            }
            task_status = refreshPgsForPort(port, portInfo.effective_speed, portInfo.cable_length, portInfo.mtu);
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
            else if (field == buffer_pool_xoff_field_name)
            {
                newSHPSize = value;
            }
            else if (field == buffer_pool_mode_field_name)
            {
                bufferPool.mode = value;
            }
            else if (field == buffer_pool_type_field_name)
            {
                if (value == buffer_value_ingress)
                    bufferPool.direction = BUFFER_INGRESS;
                else
                    bufferPool.direction = BUFFER_EGRESS;
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
                    auto &poolName = value;
                    if (poolName.empty())
                    {
                        SWSS_LOG_ERROR("BUFFER_PROFILE: Invalid format of reference to pool: %s", value.c_str());
                        return task_process_status::task_invalid_entry;
                    }

                    auto poolRef = m_bufferPoolLookup.find(poolName);
                    if (poolRef == m_bufferPoolLookup.end())
                    {
                        SWSS_LOG_WARN("Pool %s hasn't been configured yet, need retry", poolName.c_str());
                        return task_process_status::task_need_retry;
                    }
                    profileApp.pool_name = poolName;
                    profileApp.direction = poolRef->second.direction;
                }
                else
                {
                    SWSS_LOG_ERROR("Pool for BUFFER_PROFILE %s hasn't been specified", field.c_str());
                    return task_process_status::task_failed;
                }
            }
            else if (field == buffer_xon_field_name)
            {
                profileApp.xon = value;
            }
            else if (field == buffer_xoff_field_name)
            {
                profileApp.xoff = value;
                profileApp.lossless = true;
            }
            else if (field == buffer_xon_offset_field_name)
            {
                profileApp.xon_offset = value;
            }
            else if (field == buffer_size_field_name)
            {
                profileApp.size = value;
            }
            else if (field == buffer_dynamic_th_field_name)
            {
                profileApp.threshold = value;
            }
            else if (field == buffer_static_th_field_name)
            {
                profileApp.threshold = value;
            }
            else if (field == buffer_headroom_type_field_name)
            {
                profileApp.dynamic_calculated = (value == "dynamic");
                if (profileApp.dynamic_calculated)
                {
                    // For dynamic calculated headroom, user can provide this field only
                    // We need to supply lossless and ingress
                    profileApp.lossless = true;
                    profileApp.direction = BUFFER_INGRESS;
                }
            }
            SWSS_LOG_INFO("Inserting BUFFER_PROFILE table field %s value %s", field.c_str(), value.c_str());
        }

        // Don't insert dynamically calculated profiles into APPL_DB
        if (profileApp.lossless)
        {
            if (profileApp.direction != BUFFER_INGRESS)
            {
                SWSS_LOG_ERROR("BUFFER_PROFILE %s is ingress but referencing an egress pool %s", profileName.c_str(), profileApp.pool_name.c_str());
                return task_process_status::task_success;
            }

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
            SWSS_LOG_NOTICE("BUFFER_PROFILE %s has been inserted into APPL_DB directly", profileName.c_str());
            updateBufferProfileToDb(profileName, profileApp);
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

void BufferMgrDynamic::handleSetSingleBufferObjectOnAdminDownPort(buffer_direction_t direction, const string &port, const string &key, const string &profile)
{
    auto &idsToZero = m_bufferObjectIdsToZero[direction];
    auto &table = m_applBufferObjectTables[direction];
    auto const &objectName = m_bufferObjectNames[direction];
    auto &portInfo = m_portInfoLookup[port];

    if (m_portInitDone)
    {
        if (idsToZero.empty())
        {
            // If initialization finished, no extra handle required.
            // Check whether the key overlaps with supported but not configured map
            auto const &idsToAdd = parseObjectNameFromKey(key, 1);
            auto idsToAddBitmap = generateBitMapFromIdsStr(idsToAdd);
            auto &supportedButNotConfiguredItems = portInfo.supported_but_not_configured_buffer_objects[direction];
            unsigned long overlappedUnconfiguredIdsMap = 0;
            string overlappedUnconfiguredIdsStr;
            const string &portPrefix = port + delimiter;
            for (auto const &ids : supportedButNotConfiguredItems)
            {
                /*
                 * An exmaple to explain the logic
                 * Item no                                     01234567
                 * Supported but not
                 * configured items:                           -**-****
                 * Items to be added:                          -----**-
                 * Slice to be removed
                 * in the next iternation:                     ----****
                 * Legend: *: valid -: empty
                 * Currently, supported but not configured items: 1-2, 4-7
                 * Items to be added: 5-6
                 * It will result in
                 * - items 4-7 to be removed from APPL_DB
                 * - items 4, 5-6, 7 to be added to APPL_DB
                 */
                auto idsBitmap = generateBitMapFromIdsStr(ids);
                if ((idsToAddBitmap & idsBitmap) == idsToAddBitmap)
                {
                    // Ids to add overlaps with this slice of unconfigured IDs
                    // Remove the slice and regenerate them
                    // It should be slice 4-7 in the above example
                    auto const &keyToRemove = portPrefix + ids;
                    SWSS_LOG_INFO("Buffer %s %s overlapped with existing zero item %s, remove the latter first",
                                  objectName.c_str(), key.c_str(), keyToRemove.c_str());
                    table.del(keyToRemove);
                    overlappedUnconfiguredIdsMap = (idsBitmap ^ idsToAddBitmap);
                    overlappedUnconfiguredIdsStr = ids;
                    break;
                }
                /*
                 * Result after iteration:
                 * overlappedUnconfiguredIdsMap: ----*--* (Items to be added has been removed from this map)
                 * overlappedUnconfiguredIdsStr: ----****
                 */
            }
            // Regenerate unconfigured IDs
            supportedButNotConfiguredItems.erase(overlappedUnconfiguredIdsStr);
            auto const &splitIds = generateIdListFromMap(overlappedUnconfiguredIdsMap, portInfo.maximum_buffer_objects[direction]);
            /*
             * Slices returned: ----*--*
             * Readd zero profiles for each slice in the set
             */
            for (auto const &splitId : splitIds)
            {
                auto const &keyToAdd = portPrefix + splitId;
                updateBufferObjectToDb(keyToAdd, m_bufferZeroProfileName[direction], true, direction);
                supportedButNotConfiguredItems.insert(splitId);
                SWSS_LOG_INFO("Add %s zero item %s", objectName.c_str(), keyToAdd.c_str());
            }

            auto const &zeroProfile = fetchZeroProfileFromNormalProfile(profile);
            if (!zeroProfile.empty())
            {
                updateBufferObjectToDb(key, zeroProfile, true, direction);
                SWSS_LOG_INFO("Add configured %s item %s as zero profile", objectName.c_str(), key.c_str());
            }
            else
            {
                updateBufferObjectToDb(key, zeroProfile, false, direction);
                SWSS_LOG_INFO("No zero profile defined for %s, reclaiming buffer by removing buffer %s %s",
                              profile.c_str(), objectName.c_str(), key.c_str());
            }
        }
        return;
    }

    if (!idsToZero.empty())
    {
        if (port + delimiter + idsToZero != key)
        {
            // For admin-down ports, if zero profiles have been applied on specified items,
            // notify APPL_DB to remove only if the item ID doesn't equal specified items
            // and the system is during initialization
            // This is to guarantee the port can be "ready" in orchagent
            // In case the port is admin down during initialization, the PG will be removed from the port,
            // which effectively notifies bufferOrch to add the item to the m_ready_list
            table.del(key);
        }
    }
    else
    {
        // Notify APPL_DB to add zero profile to items
        // An side effect is the items will be ready in orchagent (added to m_ready_list)
        string &zeroProfile = fetchZeroProfileFromNormalProfile(profile);
        if (!zeroProfile.empty())
        {
            SWSS_LOG_INFO("Reclaim reserved buffer for %s %s by applying zero profile %s", objectName.c_str(), key.c_str(), zeroProfile.c_str());
            updateBufferObjectToDb(key, zeroProfile, true, direction);
        }
        else
        {
            SWSS_LOG_INFO("Reclaim reserved buffer for %s %s by removing it due to empty zero profile", objectName.c_str(), key.c_str());
            updateBufferObjectToDb(key, zeroProfile, false, direction);
        }
    }
}

void BufferMgrDynamic::handleDelSingleBufferObjectOnAdminDownPort(buffer_direction_t direction, const string &port, const string &key, port_info_t &portInfo)
{
    auto &idsToZero = m_bufferObjectIdsToZero[direction];
    auto &supportedNotConfiguredItems = portInfo.supported_but_not_configured_buffer_objects[direction];
    auto const &objectName = m_bufferObjectNames[direction];

    if (idsToZero.empty())
    {
        // For admin down ports, if zero profiles have been applied to all configured items
        // do NOT remove it otherwise SDK default value will be set for the items
        // Move the key to supported_but_not_configured_items so that the slice of items
        // can be removed when the port is started up
        auto const &idsToDelStr = parseObjectNameFromKey(key, 1);
        auto const &keyPrefix = port + delimiter;
        auto idsToDelMap = generateBitMapFromIdsStr(idsToDelStr);
        // If this item can be combined with any existing items, let's do.
        // adjancentItemsSet will hold all adjancent slices.
        // Otherwise, just add it to the list.
        set<string> adjancentItemsSet;
        for (auto const &idStr : supportedNotConfiguredItems)
        {
            unsigned long idsMap = generateBitMapFromIdsStr(idStr);
            // Make sure there is no overlap
            if ((idsMap & idsToDelMap) == 0)
            {
                if (isItemIdsMapContinuous(idsMap|idsToDelMap, portInfo.maximum_buffer_objects[direction]))
                {
                    SWSS_LOG_INFO("Removing buffer %s item %s, found adjancent item %s, remove the later first",
                                  objectName.c_str(), key.c_str(), idStr.c_str());
                    adjancentItemsSet.insert(idStr);
                    updateBufferObjectToDb(keyPrefix + idStr, "", false, direction);
                    idsToDelMap |= idsMap;
                }
            }
        }

        if (!adjancentItemsSet.empty())
        {
            SWSS_LOG_INFO("Removing buffer %s item %s from APPL_DB because it will be combined", objectName.c_str(), key.c_str());
            updateBufferObjectToDb(key, "", false, direction);

            for (auto const &idStr : adjancentItemsSet)
            {
                supportedNotConfiguredItems.erase(idStr);
            }

            auto const combinedIdsStr = generateIdListFromMap(idsToDelMap, portInfo.maximum_buffer_objects[direction]);
            // combinedIdsStr should contain only one id string.
            for (auto const &idStr : combinedIdsStr)
            {
                SWSS_LOG_INFO("Removing buffer %s item %s, got combined item %s",
                              objectName.c_str(), key.c_str(), idStr.c_str());
                supportedNotConfiguredItems.insert(idStr);
                // According to the logic in reclaimReservedBufferForPort,
                // only if the m_bufferZeroProfileName is not empty will supported_but_not_configured list be generated.
                // Now that adjancentItemsSet, which is a sub set of supported_but_not_configured, is NOT empty,
                // m_bufferZeroProfileName must NOT be empty.
                updateBufferObjectToDb(keyPrefix + idStr, m_bufferZeroProfileName[direction], true, direction);
            }
        }
        else if (!m_bufferZeroProfileName[direction].empty())
        {
            SWSS_LOG_INFO("Removing buffer %s item %s", objectName.c_str(), key.c_str());
            supportedNotConfiguredItems.insert(idsToDelStr);
        }
    }

    // For admin down ports, if zero profiles configured on specified items only
    // it has been removed from APPL_DB when the port is admin down
    // Just removing it from m_portQueueLookup or m_portPgLookup should suffice.
    // Do NOT touch APPL_DB
}

task_process_status BufferMgrDynamic::handleSingleBufferPgEntry(const string &key, const string &port, const KeyOpFieldsValuesTuple &tuple)
{
    string op = kfvOp(tuple);
    buffer_pg_t &bufferPg = m_portPgLookup[port][key];
    port_info_t &portInfo = m_portInfoLookup[port];

    SWSS_LOG_DEBUG("Processing command:%s table BUFFER_PG key %s", op.c_str(), key.c_str());
    if (op == SET_COMMAND)
    {
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
            bufferPg.configured_profile_name.clear();
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
                const string &profileName = value;
                if (profileName.empty())
                {
                    SWSS_LOG_ERROR("BUFFER_PG: Invalid format of reference to profile: %s", value.c_str());
                    return task_process_status::task_invalid_entry;
                }

                auto searchRef = m_bufferProfileLookup.find(profileName);
                if (searchRef == m_bufferProfileLookup.end())
                {
                    // In this case, we shouldn't set the dynamic calculated flag to true
                    // It will be updated when its profile configured.
                    bufferPg.dynamic_calculated = false;
                    SWSS_LOG_WARN("Profile %s hasn't been configured yet, skip", profileName.c_str());
                    return task_process_status::task_need_retry;
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

            SWSS_LOG_INFO("Inserting BUFFER_PG table field %s value %s", field.c_str(), value.c_str());
        }

        if (pureDynamic)
        {
            // Generic dynamically calculated headroom
            bufferPg.dynamic_calculated = true;
            bufferPg.lossless = true;
        }

        if (PORT_ADMIN_DOWN == portInfo.state)
        {
            // In case the port is admin down during initialization, the PG will be removed from the port,
            // which effectively notifies bufferOrch to add the item to the m_ready_list
            handleSetSingleBufferObjectOnAdminDownPort(BUFFER_PG, port, key, bufferPg.configured_profile_name);
        }
        else if (bufferPg.lossless)
        {
            doUpdatePgTask(key, port);
        }
        else
        {
            SWSS_LOG_NOTICE("Inserting BUFFER_PG table entry %s into APPL_DB directly", key.c_str());
            bufferPg.running_profile_name = bufferPg.configured_profile_name;
            updateBufferObjectToDb(key, bufferPg.running_profile_name, true);
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

        if (!m_supportRemoving)
        {
            SWSS_LOG_ERROR("Removing priority group %s is not supported on the platform", key.c_str());
            return task_process_status::task_failed;
        }

        if (!runningProfileName.empty())
        {
            m_bufferProfileLookup[runningProfileName].port_pgs.erase(key);
        }
        if (!configProfileName.empty() && configProfileName != runningProfileName)
        {
            m_bufferProfileLookup[configProfileName].port_pgs.erase(key);
        }

        if (portInfo.state == PORT_ADMIN_DOWN)
        {
            handleDelSingleBufferObjectOnAdminDownPort(BUFFER_PG, port, key, portInfo);
        }
        else if (bufferPg.lossless)
        {
            doRemovePgTask(key, port);
        }
        else
        {
            SWSS_LOG_NOTICE("Removing BUFFER_PG table entry %s from APPL_DB directly", key.c_str());
            m_applBufferObjectTables[BUFFER_PG].del(key);
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

task_process_status BufferMgrDynamic::checkBufferProfileDirection(const string &profiles, buffer_direction_t dir)
{
    // Fetch profile and check whether it's an egress profile
    vector<string> profileRefList = tokenize(profiles, ',');
    for (auto &profileName : profileRefList)
    {
        auto profileSearchRef = m_bufferProfileLookup.find(profileName);
        if (profileSearchRef == m_bufferProfileLookup.end())
        {
            SWSS_LOG_NOTICE("Profile %s doesn't exist, need retry", profileName.c_str());
            return task_process_status::task_need_retry;
        }

        auto &profileObj = profileSearchRef->second;
        if (dir != profileObj.direction)
        {
            SWSS_LOG_ERROR("Profile %s's direction is %s but %s is expected, applying profile failed",
                           profileName.c_str(),
                           m_bufferDirectionNames[profileObj.direction].c_str(),
                           m_bufferDirectionNames[dir].c_str());
            return task_process_status::task_failed;
        }
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleSingleBufferQueueEntry(const string &key, const string &port, const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();

    const auto &op = kfvOp(tuple);
    const auto &queues = key;
    auto &portInfo = m_portInfoLookup[port];

    if (op == SET_COMMAND)
    {
        auto &portQueue = m_portQueueLookup[port][queues];

        SWSS_LOG_INFO("Inserting entry BUFFER_QUEUE_TABLE:%s to APPL_DB", key.c_str());

        for (auto i : kfvFieldsValues(tuple))
        {
            // Transform the separator in values from "|" to ":"
            if (fvField(i) == buffer_profile_field_name)
            {
                auto rc = checkBufferProfileDirection(fvValue(i), BUFFER_EGRESS);
                if (rc != task_process_status::task_success)
                    return rc;
                portQueue.running_profile_name = fvValue(i);
                SWSS_LOG_NOTICE("Queue %s has been configured on the system, referencing profile %s", key.c_str(), fvValue(i).c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Unknown field %s in BUFFER_QUEUE|%s", fvField(i).c_str(), key.c_str());
                continue;
            }

            SWSS_LOG_INFO("Inserting field %s value %s", fvField(i).c_str(), fvValue(i).c_str());
        }

        // TODO: check overlap. Currently, assume there is no overlap

        if (PORT_ADMIN_DOWN == portInfo.state)
        {
            handleSetSingleBufferObjectOnAdminDownPort(BUFFER_QUEUE, port, key, portQueue.running_profile_name);
        }
        else
        {
            updateBufferObjectToDb(key, portQueue.running_profile_name, true, BUFFER_QUEUE);
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (!m_supportRemoving)
        {
            SWSS_LOG_ERROR("Removing queue %s is not supported on the platform", key.c_str());
            return task_process_status::task_failed;
        }
        SWSS_LOG_INFO("Removing entry %s from APPL_DB", key.c_str());
        m_portQueueLookup[port].erase(queues);
        if (PORT_ADMIN_DOWN == portInfo.state)
        {
            handleDelSingleBufferObjectOnAdminDownPort(BUFFER_QUEUE, port, key, portInfo);
        }
        else
        {
            m_applBufferObjectTables[BUFFER_QUEUE].del(key);
        }
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleSingleBufferPortProfileListEntry(const string &key, buffer_direction_t dir, const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();

    const string &port = key;
    const string &op = kfvOp(tuple);
    const string &tableName = dir == BUFFER_INGRESS ? APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME : APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME;
    ProducerStateTable &appTable = m_applBufferProfileListTables[dir];
    port_profile_list_lookup_t &profileListLookup = m_portProfileListLookups[dir];

    if (op == SET_COMMAND)
    {
        SWSS_LOG_INFO("Inserting entry %s:%s to APPL_DB", tableName.c_str(), key.c_str());

        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == buffer_profile_list_field_name)
            {
                auto rc = checkBufferProfileDirection(fvValue(i), dir);
                if (rc != task_process_status::task_success)
                    return rc;
                profileListLookup[port] = fvValue(i);
                SWSS_LOG_NOTICE("%s %s has been configured on the system, referencing profile list %s", tableName.c_str(), key.c_str(), fvValue(i).c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Unknown field %s in %s", fvField(i).c_str(), key.c_str());
                continue;
            }
        }

        auto &portInfo = m_portInfoLookup[port];
        if (PORT_ADMIN_DOWN != portInfo.state)
        {
            // Only apply profile list on admin up port
            // For admin-down ports, zero profile list has been applied on the port when it entered admin-down state
            updateBufferObjectListToDb(key, profileListLookup[port], dir);
        }
    }
    else if (op == DEL_COMMAND)
    {
        // Not supported on Mellanox platform for now.
        SWSS_LOG_INFO("Removing entry %s:%s from APPL_DB", tableName.c_str(), key.c_str());
        profileListLookup.erase(port);
        appTable.del(key);
    }

    return task_process_status::task_success;
}

task_process_status BufferMgrDynamic::handleSingleBufferPortIngressProfileListEntry(const string &key, const string &port, const KeyOpFieldsValuesTuple &tuple)
{
    return handleSingleBufferPortProfileListEntry(key, BUFFER_INGRESS, tuple);
}

task_process_status BufferMgrDynamic::handleSingleBufferPortEgressProfileListEntry(const string &key, const string &port, const KeyOpFieldsValuesTuple &tuple)
{
    return handleSingleBufferPortProfileListEntry(key, BUFFER_EGRESS, tuple);
}

/*
 * handleBufferObjectTables
 *
 * For the buffer object tables, like BUFFER_PG, BUFFER_QUEUE, BUFFER_PORT_INGRESS_PROFILE_LIST, and BUFFER_PORT_EGRESS_PROFILE_LIST,
 * their keys can be either a single port or a port list separated by a comma, which can be handled by a common logic.
 * So the logic to parse the keys is common and the logic to handle each table is different.
 * This function is introduced to handle the common logic and a handler map (buffer_single_item_handler_map) is introduced
 * to dispatch to different handler according to the table name.
 *
 * Arguments:
 *  tuple, the KeyOpFieldsValuesTuple, containing key, op, list of tuples consisting of field and value pair
 *  table, the name of the table
 *  keyWithIds, whether the keys contains sublevel IDs of objects.
 *   - For queue and priority groups, it is true since the keys format is like <port list>|IDs
 *     Eg: input key "Ethernet0,Ethernet4,Ethernet8,Ethernet12|3-4" will be decomposed to the following list
 *     [Ethernet0|3-4, Ethernet4|3-4, Ethernet8|3-4, Ethernet12|3-4]
 *   - For profile list tables, it is false since the keys format is <port list>
 *     Eg: input key "Ethernet0,Ethernet4,Ethernet8,Ethernet12" will be decomposed to the following list
 *     [Ethernet0, Ethernet4, Ethernet8, Ethernet12]
 *
 * It is wrapped by the following functions which are the table handler of the above tables:
 *  - handleBufferPgTable
 *  - handleBufferQueueTable
 *  - handleBufferPortIngressProfileListTable
 *  - handleBufferPortEgressProfileListTable
 * and will call the following table handlers which handles table update for a single port:
 *  - handleSingleBufferPgEntry
 *  - handleSingleBufferQueueEntry
 *  - handleSingleBufferPortIngressProfileListEntry
 *  - handleSingleBufferPortEgressProfileListEntry
 *
 * The flow:
 * 1. Parse the key.
 * 2. Fetch the handler according to the table name
 * 3. For each port in the key list,
 *    - Construct a new key as
 *      - A single port + IDs if keyWidthIds is true
 *      - Or a single port only if keyWidthIds is false
 *    - Call the corresponding handler.
 */
task_process_status BufferMgrDynamic::handleBufferObjectTables(KeyOpFieldsValuesTuple &tuple, const string &table, bool keyWithIds)
{
    SWSS_LOG_ENTER();
    string key = kfvKey(tuple);

    transformSeperator(key);

    string ports = parseObjectNameFromKey(key);
    if (ports.empty())
    {
        SWSS_LOG_ERROR("Invalid key format %s for %s table", key.c_str(), table.c_str());
        return task_process_status::task_invalid_entry;
    }

    string ids;
    if (keyWithIds)
    {
        ids = parseObjectNameFromKey(key, 1);
        if (ids.empty())
        {
            SWSS_LOG_ERROR("Invalid key format %s for %s table", key.c_str(), table.c_str());
            return task_process_status::task_invalid_entry;
        }
    }

    auto portsList = tokenize(ports, ',');

    task_process_status rc = task_process_status::task_success;
    auto &handler = m_bufferSingleItemHandlerMap[table];

    if (portsList.size() == 1)
    {
        rc = (this->*handler)(key, ports, tuple);
    }
    else
    {
        for (auto port : portsList)
        {
            string singleKey;
            if (keyWithIds)
                singleKey = port + ':' + ids;
            else
                singleKey = port;
            rc = (this->*handler)(singleKey, port, tuple);
            if (rc == task_process_status::task_need_retry)
                return rc;
        }
    }

    return rc;
}

task_process_status BufferMgrDynamic::handleBufferPgTable(KeyOpFieldsValuesTuple &tuple)
{
    return handleBufferObjectTables(tuple, CFG_BUFFER_PG_TABLE_NAME, true);
}

task_process_status BufferMgrDynamic::handleBufferQueueTable(KeyOpFieldsValuesTuple &tuple)
{
    return handleBufferObjectTables(tuple, CFG_BUFFER_QUEUE_TABLE_NAME, true);
}

task_process_status BufferMgrDynamic::handleBufferPortIngressProfileListTable(KeyOpFieldsValuesTuple &tuple)
{
    return handleBufferObjectTables(tuple, CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, false);
}

task_process_status BufferMgrDynamic::handleBufferPortEgressProfileListTable(KeyOpFieldsValuesTuple &tuple)
{
    return handleBufferObjectTables(tuple, CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, false);
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

/*
 * We do not apply any buffer profiles and objects until the buffer pools are ready.
 * This is to prevent buffer orchagent from keeping retrying if buffer profiles and objects are inserted to APPL_DB while buffer pools are not.
 *
 * Originally, all buffer profiles and objects were applied to APPL_DB as soon as they were learnt from CONFIG_DB.
 * The buffer profiles, and objects will be available as soon as they are received from CONFIG_DB when the system is starting
 * but it takes more seconds for the buffer pools to be applied to APPL_DB
 * because the sizes need to be calculated first and can be calculated only after some other data available.
 * This causes buffer orchagent receives buffer profiles and objects first and then buffer pools.
 * The buffer orchagent has to keep retrying in such case, which wastes time and incurs error messages.
 *
 * Now, we will guarantee the buffer profiles and objects to be applied only after buffer pools are ready.
 * This is achieved by the following steps:
 *  - m_bufferPoolReady indicates whether the buffer pools are ready.
 *  - When the buffer profiles and objects are received from CONFIG_DB,
 *    they will be handled and stored in internal data struct
 *    (but not applied to APPL_DB until the buffer pools are ready, AKA. m_bufferPoolReady == true)
 *  - Once buffer pools are ready, all stored buffer profiles and objects will be applied to APPL_DB by this function.
 *
 * Applying additional zero profiles, which is supported but not configured profiles, if any, will be deferred in non-warmreboot.
 * This is to accelerate the start flow especially for fast reboot.
 *
 * The progress of pushing buffer pools into APPL_DB has also been accelerated by the lua plugin for calculating buffer sizes.
 * Originally, if the data are not available for calculating buffer sizes, it just return an empty vectors and buffer manager will retry
 * Now, the lua plugin will return the sizes calculated by the following logic:
 *  - If there are buffer sizes in APPL_DB, return the sizes in APPL_DB.
 *  - Otherwise, return all 0 as sizes.
 * This makes sure there will be available sizes of buffer pools as sonn as possible when the system is starting
 * The sizes will be updated to non zero when the data for calculating it are ready.
 * Even this is Mellanox specific, other vendors can also take advantage of this logic.
 */
void BufferMgrDynamic::handlePendingBufferObjects()
{
    if (m_bufferPoolReady)
    {
        if (!m_pendingApplyZeroProfilePorts.empty())
        {
            // No action as no zero profiles defined.
            if (!m_zeroPoolAndProfileInfo.empty() && !m_zeroProfilesLoaded)
            {
                loadZeroPoolAndProfiles();
            }
        }

        if (m_bufferObjectsPending)
        {
            // Apply all pending buffer profiles
            for (auto &profile : m_bufferProfileLookup)
            {
                updateBufferProfileToDb(profile.first, profile.second);
            }

            for (auto &port : m_portInfoLookup)
            {
                if (port.second.state != PORT_ADMIN_DOWN)
                {
                    // The admin-down ports should not be touched here. Two scenarios
                    // 1. If admin-status is handled ahead of m_bufferPoolReady being true,
                    //    They should be in m_pendingApplyZeroProfilePorts and will be handled later in this function
                    // 2. Otherwise, they should have been handled and zero profiles have been applied.
                    refreshPgsForPort(port.first, port.second.effective_speed, port.second.cable_length, port.second.mtu);

                    // Apply pending buffer queues
                    auto &queues = m_portQueueLookup[port.first];
                    for (auto &queue : queues)
                    {
                        updateBufferObjectToDb(queue.first, queue.second.running_profile_name, true, BUFFER_QUEUE);
                    }

                    // Apply all pending buffer profile lists
                    for (auto dir : m_bufferDirections)
                    {
                        auto &profileList = m_portProfileListLookups[dir][port.first];
                        if (!profileList.empty())
                            updateBufferObjectListToDb(port.first, profileList, dir);
                    }
                }
            }

            m_bufferObjectsPending = false;
        }

        if (!m_pendingApplyZeroProfilePorts.empty())
        {
            // m_pendingApplyZeroProfilePorts contains all the admin down ports
            // We do NOT iterate all admin down ports but the set containing ports need to be applied zero profiles
            set<string> portsNeedRetry;
            for ( auto &port : m_pendingApplyZeroProfilePorts)
            {
                if (isReadyToReclaimBufferOnPort(port))
                {
                    reclaimReservedBufferForPort(port, m_portPgLookup, BUFFER_PG);
                    reclaimReservedBufferForPort(port, m_portQueueLookup, BUFFER_QUEUE);
                    SWSS_LOG_NOTICE("Admin-down port %s is handled after buffer pools have been configured", port.c_str());
                }
                else
                {
                    SWSS_LOG_NOTICE("Admin-down port %s is still not ready to reclaim after buffer pools have been configured, need retry", port.c_str());
                    portsNeedRetry.insert(port);
                }
            }
            m_pendingApplyZeroProfilePorts = move(portsNeedRetry);
        }

        // configuredItemsDone means all buffer objects (profiles, priority groups, queues, profile lists) in the CONFIG_DB have been applied to APPL_DB
        // - on admin up ports, normal profiles are applied
        // - on admin down ports, zero profiles are applied
        bool configuredItemsDone = !m_bufferObjectsPending && m_pendingApplyZeroProfilePorts.empty();

        if (WarmStart::isWarmStart())
        {
            // For warm restart, all buffer items have been applied now
            if (configuredItemsDone)
            {
                WarmStart::setWarmStartState("buffermgrd", WarmStart::REPLAYED);
                // There is no operation to be performed for buffermgrd reconcillation
                // Hence mark it reconciled right away
                WarmStart::setWarmStartState("buffermgrd", WarmStart::RECONCILED);
                m_bufferCompletelyInitialized = true;
                SWSS_LOG_NOTICE("All bufer configuration has been applied. Buffer initialization done");
            }
        }
        else
        {
            // For fast reboot, cold reboot and other initialization flow, the additional zero profiles will be applied
            // 30 seconds later once normal profiles and zero profiles of configured items have been applied
            // This is to accelerate fast reboot flow.
            // m_waitApplyAdditionalZeroProfiles is initialized as 3 during buffer manager initialization
            // Timer's period is 10. So total deferred time is 3*10 = 30 seconds
            if (m_waitApplyAdditionalZeroProfiles > 0)
            {
                if (m_portInitDone && configuredItemsDone)
                {
                    SWSS_LOG_NOTICE("Additional zero profiles will be appied after %d seconds", m_waitApplyAdditionalZeroProfiles * 10);
                    m_waitApplyAdditionalZeroProfiles --;
                }
            }
            else
            {
                // For admin down ports, apply supported but not configured items
                for (auto dir : m_bufferDirections)
                {
                    for ( auto &port : m_pendingSupportedButNotConfiguredPorts[dir])
                    {
                        auto &portInfo = m_portInfoLookup[port];
                        for(auto &it: portInfo.supported_but_not_configured_buffer_objects[dir])
                        {
                            auto const &key = port + delimiter + it;
                            SWSS_LOG_INFO("Applying additional zero profiles on port %s", key.c_str());
                            updateBufferObjectToDb(key, m_bufferZeroProfileName[dir], true, dir);
                        }
                    }
                    m_pendingSupportedButNotConfiguredPorts[dir].clear();
                }

                if (configuredItemsDone)
                {
                    m_bufferCompletelyInitialized = true;
                    SWSS_LOG_NOTICE("All bufer configuration has been applied. Buffer initialization done");
                }
            }
        }
    }
}

void BufferMgrDynamic::doTask(SelectableTimer &timer)
{
    checkSharedBufferPoolSize(true);
    if (!m_bufferCompletelyInitialized)
        handlePendingBufferObjects();
}
