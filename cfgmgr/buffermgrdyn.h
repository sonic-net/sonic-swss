#ifndef __BUFFMGRDYN__
#define __BUFFMGRDYN__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <set>
#include <string>

namespace swss {

#define INGRESS_LOSSLESS_PG_POOL_NAME "ingress_lossless_pool"
#define DEFAULT_MTU_STR             "9100"

#define BUFFERMGR_TIMER_PERIOD 10

typedef enum {
    BUFFER_INGRESS = 0,
    BUFFER_PG = BUFFER_INGRESS,
    BUFFER_EGRESS = 1,
    BUFFER_QUEUE = BUFFER_EGRESS,
    BUFFER_DIR_MAX
} buffer_direction_t;

typedef struct {
    buffer_direction_t direction;
    bool dynamic_size;
    std::string total_size;
    std::string mode;
    std::string xoff;
    std::string zero_profile_name;
} buffer_pool_t;

// State of the profile.
// We maintain state for dynamic profiles only
// INITIALIZING state indicates a profile is under initializing
// NORMAL state indicates that profile is working normally
// A profile's state will be STALE When it is no longer referenced
// It will be removed after having been in this state for 1 minute
// This deferred mechanism yields two benefits
// 1. Avoid frequently removing/re-adding a profile
// 2. Avoid removing profile failure if orchagent handles BUFFER_PG
//    and BUFFER_PROFILE tables in an order which differs from which
//    buffermgrd calls
typedef enum {
    PROFILE_INITIALIZING,
    PROFILE_NORMAL
} profile_state_t;

typedef std::set<std::string> port_pg_set_t;
typedef struct {
    profile_state_t state;
    bool dynamic_calculated;
    bool static_configured;
    bool lossless;
    buffer_direction_t direction;

    // fields representing parameters by which the headroom is calculated
    std::string speed;
    std::string cable_length;
    std::string port_mtu;
    std::string gearbox_model;
    long lane_count;

    // APPL_DB.BUFFER_PROFILE fields
    std::string name;
    std::string size;
    std::string xon;
    std::string xon_offset;
    std::string xoff;
    std::string threshold;
    std::string pool_name;
    // port_pgs - stores pgs referencing this profile
    // An element will be added or removed when a PG added or removed
    port_pg_set_t port_pgs;
} buffer_profile_t;

typedef struct {
    std::string running_profile_name;
} buffer_object_t;

typedef struct : public buffer_object_t{
    bool lossless;
    bool dynamic_calculated;
    bool static_configured;
    // There will always be a running_profile which derived from:
    // - the configured profile for static one,
    // - the dynamically generated profile otherwise.
    std::string configured_profile_name;
} buffer_pg_t;

typedef enum {
    // Port is admin down. All PGs programmed to APPL_DB should be removed from the port
    PORT_ADMIN_DOWN,
    // Port is under initializing, which means its info hasn't been comprehensive for calculating headroom
    PORT_INITIALIZING,
    // All necessary information for calculating headroom is ready
    PORT_READY
} port_state_t;

typedef struct {
    port_state_t state;
    std::string speed;
    std::string cable_length;
    std::string mtu;
    std::string gearbox_model;

    bool auto_neg;
    std::string effective_speed;
    std::string adv_speeds;
    std::string supported_speeds;

    long lane_count;
    sai_uint32_t maximum_buffer_objects[BUFFER_DIR_MAX];
    std::set<std::string> supported_but_not_configured_buffer_objects[BUFFER_DIR_MAX];
} port_info_t;

//TODO:
//add map to store all configured PGs
//add map to store all configured profiles
//check whether the configure database update is for dynamically lossless,
//if yes, call further function to handle; if no, handle it directly in the callback

//map from port name to port info
typedef std::map<std::string, port_info_t> port_info_lookup_t;
//map from port info to profile
//for dynamically calculated profile
typedef std::map<std::string, buffer_profile_t> buffer_profile_lookup_t;
//map from name to pool
typedef std::map<std::string, buffer_pool_t> buffer_pool_lookup_t;
//map from pg to info
typedef std::map<std::string, buffer_pg_t> buffer_pg_lookup_t;
//map from port to all its pgs
typedef std::map<std::string, buffer_pg_lookup_t> port_pg_lookup_t;
//map from object name to its profile
typedef std::map<std::string, buffer_object_t> buffer_object_lookup_t;
//map from port to all its objects
typedef std::map<std::string, buffer_object_lookup_t> port_object_lookup_t;
//map from port to profile_list
typedef std::map<std::string, std::string> port_profile_list_lookup_t;
//map from gearbox model to gearbox delay
typedef std::map<std::string, std::string> gearbox_delay_t;

class BufferMgrDynamic : public Orch
{
public:
    BufferMgrDynamic(DBConnector *cfgDb, DBConnector *stateDb, DBConnector *applDb, const std::vector<TableConnector> &tables, std::shared_ptr<std::vector<KeyOpFieldsValuesTuple>> gearboxInfo, std::shared_ptr<std::vector<KeyOpFieldsValuesTuple>> zeroProfilesInfo);
    using Orch::doTask;

private:
    std::string m_platform;
    std::vector<buffer_direction_t> m_bufferDirections;
    const std::string m_bufferObjectNames[BUFFER_DIR_MAX];
    const std::string m_bufferDirectionNames[BUFFER_DIR_MAX];

    typedef task_process_status (BufferMgrDynamic::*buffer_table_handler)(KeyOpFieldsValuesTuple &t);
    typedef std::map<std::string, buffer_table_handler> buffer_table_handler_map;
    typedef std::pair<std::string, buffer_table_handler> buffer_handler_pair;

    buffer_table_handler_map m_bufferTableHandlerMap;

    typedef task_process_status (BufferMgrDynamic::*buffer_single_item_handler)(const std::string &key, const std::string &port, const KeyOpFieldsValuesTuple &tuple);
    typedef std::map<std::string, buffer_single_item_handler> buffer_single_item_handler_map;
    typedef std::pair<std::string, buffer_single_item_handler> buffer_single_item_handler_pair;

    buffer_single_item_handler_map m_bufferSingleItemHandlerMap;

    bool m_portInitDone;
    bool m_bufferPoolReady;
    bool m_bufferObjectsPending;
    bool m_bufferCompletelyInitialized;

    std::string m_configuredSharedHeadroomPoolSize;

    std::shared_ptr<DBConnector> m_applDb = nullptr;
    SelectableTimer *m_buffermgrPeriodtimer = nullptr;

    // Fields for zero pool and profiles
    std::vector<KeyOpFieldsValuesTuple> m_zeroPoolAndProfileInfo;
    std::set<std::string> m_zeroPoolNameSet;
    std::vector<std::pair<std::string, std::string>> m_zeroProfiles;
    bool m_zeroProfilesLoaded;
    bool m_supportRemoving;
    std::string m_bufferZeroProfileName[BUFFER_DIR_MAX];
    std::string m_bufferObjectIdsToZero[BUFFER_DIR_MAX];

    // PORT table and caches
    Table m_statePortTable;
    // m_portInfoLookup
    // key: port name
    // updated only when a port's speed and cable length updated
    port_info_lookup_t m_portInfoLookup;
    std::set<std::string> m_adminDownPorts;
    std::set<std::string> m_pendingApplyZeroProfilePorts;
    std::set<std::string> m_pendingSupportedButNotConfiguredPorts[BUFFER_DIR_MAX];
    int m_waitApplyAdditionalZeroProfiles;

    // BUFFER_POOL table and cache
    ProducerStateTable m_applBufferPoolTable;
    Table m_stateBufferPoolTable;
    buffer_pool_lookup_t m_bufferPoolLookup;

    // BUFFER_PROFILE table and caches
    ProducerStateTable m_applBufferProfileTable;
    Table m_stateBufferProfileTable;
    // m_bufferProfileLookup - the cache for the following set:
    // 1. CFG_BUFFER_PROFILE
    // 2. Dynamically calculated headroom info stored in APPL_BUFFER_PROFILE
    // key: profile name
    buffer_profile_lookup_t m_bufferProfileLookup;

    // BUFFER_PG table and caches
    ProducerStateTable m_applBufferObjectTables[BUFFER_DIR_MAX];
    // m_portPgLookup - the cache for CFG_BUFFER_PG and APPL_BUFFER_PG
    // 1st level key: port name, 2nd level key: PGs
    // Updated in:
    // 1. handleBufferPgTable, update from database
    // 2. refreshPriorityGroupsForPort, speed/cable length updated
    port_pg_lookup_t m_portPgLookup;

    // BUFFER_QUEUE table and caches
    // m_portQueueLookup - the cache for BUFFER_QUEUE table
    // 1st level key: port name, 2nd level key: queues
    port_object_lookup_t m_portQueueLookup;

    // BUFFER_INGRESS_PROFILE_LIST/BUFFER_EGRESS_PROFILE_LIST table and caches
    ProducerStateTable m_applBufferProfileListTables[BUFFER_DIR_MAX];
    port_profile_list_lookup_t m_portProfileListLookups[BUFFER_DIR_MAX];

    //  table and caches
    port_profile_list_lookup_t m_portEgressProfileListLookup;

    // Other tables
    Table m_cfgDefaultLosslessBufferParam;

    Table m_stateBufferMaximumTable;

    Table m_applPortTable;

    bool m_supportGearbox;
    gearbox_delay_t m_gearboxDelay;
    std::string m_identifyGearboxDelay;

    // Vendor specific lua plugins for calculating headroom and buffer pool
    // Loaded when the buffer manager starts
    // Executed whenever the headroom and pool size need to be updated
    std::string m_headroomSha;
    std::string m_bufferpoolSha;
    std::string m_checkHeadroomSha;

    // Parameters for headroom generation
    std::string m_mmuSize;
    unsigned long m_mmuSizeNumber;
    std::string m_defaultThreshold;

    std::string m_overSubscribeRatio;

    // Initializers
    void initTableHandlerMap();
    void parseGearboxInfo(std::shared_ptr<std::vector<KeyOpFieldsValuesTuple>> gearboxInfo);
    void loadZeroPoolAndProfiles();
    void unloadZeroPoolAndProfiles();

    // Tool functions to parse keys and references
    std::string getPgPoolMode();
    void transformSeperator(std::string &name);
    std::string parseObjectNameFromKey(const std::string &key, size_t pos/* = 1*/);
    std::string getDynamicProfileName(const std::string &speed, const std::string &cable, const std::string &mtu, const std::string &threshold, const std::string &gearbox_model, long lane_count);
    inline bool isNonZero(const std::string &value) const
    {
        return !value.empty() && value != "0";
    }
    std::string getMaxSpeedFromList(std::string speedList);
    std::string &fetchZeroProfileFromNormalProfile(const std::string &profile);

    // APPL_DB table operations
    void updateBufferPoolToDb(const std::string &name, const buffer_pool_t &pool);
    void updateBufferProfileToDb(const std::string &name, const buffer_profile_t &profile);
    void updateBufferObjectToDb(const std::string &key, const std::string &profile, bool add, buffer_direction_t dir);
    void updateBufferObjectListToDb(const std::string &key, const std::string &profileList, buffer_direction_t dir);

    // Meta flows
    bool needRefreshPortDueToEffectiveSpeed(port_info_t &portInfo, std::string &portName);
    void calculateHeadroomSize(buffer_profile_t &headroom);
    void checkSharedBufferPoolSize(bool force_update_during_initialization);
    void recalculateSharedBufferPool();
    task_process_status allocateProfile(const std::string &speed, const std::string &cable, const std::string &mtu, const std::string &threshold, const std::string &gearbox_model, long lane_count, std::string &profile_name);
    void releaseProfile(const std::string &profile_name);
    bool isHeadroomResourceValid(const std::string &port, const buffer_profile_t &profile, const std::string &new_pg);
    void refreshSharedHeadroomPool(bool enable_state_updated_by_ratio, bool enable_state_updated_by_size);
    task_process_status checkBufferProfileDirection(const std::string &profiles, buffer_direction_t dir);
    std::string constructZeroProfileListFromNormalProfileList(const std::string &normalProfileList, const std::string &port);
    void removeSupportedButNotConfiguredItemsOnPort(port_info_t &portInfo, const std::string &port);
    void applyNormalBufferObjectsOnPort(const std::string &port);
    void handlePendingBufferObjects();
    void handleSetSingleBufferObjectOnAdminDownPort(buffer_direction_t direction, const std::string &port, const std::string &key, const std::string &profile);
    void handleDelSingleBufferObjectOnAdminDownPort(buffer_direction_t direction, const std::string &port, const std::string &key, port_info_t &portInfo);
    bool isReadyToReclaimBufferOnPort(const std::string &port);

    // Main flows
    template<class T> task_process_status reclaimReservedBufferForPort(const std::string &port, T &obj, buffer_direction_t dir);
    task_process_status refreshPgsForPort(const std::string &port, const std::string &speed, const std::string &cable_length, const std::string &mtu, const std::string &exactly_matched_key);
    task_process_status doUpdatePgTask(const std::string &pg_key, const std::string &port);
    task_process_status doRemovePgTask(const std::string &pg_key, const std::string &port);
    task_process_status doUpdateBufferProfileForDynamicTh(buffer_profile_t &profile);
    task_process_status doUpdateBufferProfileForSize(buffer_profile_t &profile, bool update_pool_size);

    // Table update handlers
    task_process_status handleBufferMaxParam(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleDefaultLossLessBufferParam(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleCableLenTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handlePortStateTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handlePortTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleBufferPoolTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleBufferProfileTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleSingleBufferPgEntry(const std::string &key, const std::string &port, const KeyOpFieldsValuesTuple &tuple);
    task_process_status handleSingleBufferQueueEntry(const std::string &key, const std::string &port, const KeyOpFieldsValuesTuple &tuple);
    task_process_status handleSingleBufferPortProfileListEntry(const std::string &key, buffer_direction_t dir, const KeyOpFieldsValuesTuple &tuple);
    task_process_status handleSingleBufferPortIngressProfileListEntry(const std::string &key, const std::string &port, const KeyOpFieldsValuesTuple &tuple);
    task_process_status handleSingleBufferPortEgressProfileListEntry(const std::string &key, const std::string &port, const KeyOpFieldsValuesTuple &tuple);
    task_process_status handleBufferObjectTables(KeyOpFieldsValuesTuple &tuple, const std::string &table, bool keyWithIds);
    task_process_status handleBufferPgTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleBufferQueueTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleBufferPortIngressProfileListTable(KeyOpFieldsValuesTuple &tuple);
    task_process_status handleBufferPortEgressProfileListTable(KeyOpFieldsValuesTuple &tuple);
    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);
};

}

#endif /* __BUFFMGRDYN__ */
