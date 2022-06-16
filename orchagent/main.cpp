extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include <fstream>
#include <iostream>
#include <unordered_map>
#include <map>
#include <memory>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <unistd.h>
#include <inttypes.h>
#include <sstream>
#include <stdexcept>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include "timestamp.h"

#include <sairedis.h>
#include <logger.h>

#include "orchdaemon.h"
#include "sai_serialize.h"
#include "saihelper.h"
#include "notifications.h"
#include <signal.h>
#include "warm_restart.h"
#include "gearboxutils.h"

using namespace std;
using namespace swss;

extern sai_switch_api_t *sai_switch_api;
extern sai_router_interface_api_t *sai_router_intfs_api;

#define UNREFERENCED_PARAMETER(P)       (P)

#define UNDERLAY_RIF_DEFAULT_MTU 9100

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
MacAddress gMacAddress;
MacAddress gVxlanMacAddress;

extern size_t gMaxBulkSize;

#define DEFAULT_BATCH_SIZE  128
int gBatchSize = DEFAULT_BATCH_SIZE;

bool gSairedisRecord = true;
bool gSwssRecord = true;
bool gResponsePublisherRecord = false;
bool gLogRotate = false;
bool gSaiRedisLogRotate = false;
bool gResponsePublisherLogRotate = false;
bool gSyncMode = false;
sai_redis_communication_mode_t gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
string gAsicInstance;

extern bool gIsNatSupported;

ofstream gRecordOfs;
string gRecordFile;
ofstream gResponsePublisherRecordOfs;
string gResponsePublisherRecordFile;

#define SAIREDIS_RECORD_ENABLE 0x1
#define SWSS_RECORD_ENABLE (0x1 << 1)
#define RESPONSE_PUBLISHER_RECORD_ENABLE (0x1 << 2)

string gMySwitchType = "";
int32_t gVoqMySwitchId = -1;
int32_t gVoqMaxCores = 0;
uint32_t gCfgSystemPorts = 0;
string gMyHostName = "";
string gMyAsicName = "";

void usage()
{
    cout << "usage: orchagent [-h] [-r record_type] [-d record_location] [-f swss_rec_filename] [-j sairedis_rec_filename] [-b batch_size] [-m MAC] [-i INST_ID] [-s] [-z mode] [-k bulk_size]" << endl;
    cout << "    -h: display this message" << endl;
    cout << "    -r record_type: record orchagent logs with type (default 3)" << endl;
    cout << "                    Bit 0: sairedis.rec, Bit 1: swss.rec, Bit 2: responsepublisher.rec. For example:" << endl;
    cout << "                    0: do not record logs" << endl;
    cout << "                    1: record SAI call sequence as sairedis.rec" << endl;
    cout << "                    2: record SwSS task sequence as swss.rec" << endl;
    cout << "                    3: enable both above two records" << endl;
    cout << "                    7: enable sairedis.rec, swss.rec and responsepublisher.rec" << endl;
    cout << "    -d record_location: set record logs folder location (default .)" << endl;
    cout << "    -b batch_size: set consumer table pop operation batch size (default 128)" << endl;
    cout << "    -m MAC: set switch MAC address" << endl;
    cout << "    -i INST_ID: set the ASIC instance_id in multi-asic platform" << endl;
    cout << "    -s enable synchronous mode (deprecated, use -z)" << endl;
    cout << "    -z redis communication mode (redis_async|redis_sync|zmq_sync), default: redis_async" << endl;
    cout << "    -f swss_rec_filename: swss record log filename(default 'swss.rec')" << endl;
    cout << "    -j sairedis_rec_filename: sairedis record log filename(default sairedis.rec)" << endl;
    cout << "    -k max bulk size in bulk mode (default 1000)" << endl;
}

void sighup_handler(int signo)
{
    /*
     * Don't do any logging since they are using mutexes.
     */
    gLogRotate = true;
    gSaiRedisLogRotate = true;
    gResponsePublisherLogRotate = true;
}

void syncd_apply_view()
{
    SWSS_LOG_NOTICE("Notify syncd APPLY_VIEW");

    sai_status_t status;
    sai_attribute_t attr;
    attr.id = SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD;
    attr.value.s32 = SAI_REDIS_NOTIFY_SYNCD_APPLY_VIEW;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to notify syncd APPLY_VIEW %d", status);
        exit(EXIT_FAILURE);
    }
}

/*
 * If Gearbox is enabled...
 * Create and initialize the external Gearbox PHYs. Upon success, store the
 * new PHY OID in the database to be used later when creating the Gearbox
 * ports.
*/
void init_gearbox_phys(DBConnector *applDb)
{
    Table* tmpGearboxTable = new Table(applDb, "_GEARBOX_TABLE");
    map<int, gearbox_phy_t> gearboxPhyMap;
    GearboxUtils gearbox;

    if (gearbox.isGearboxEnabled(tmpGearboxTable))
    {
        gearboxPhyMap = gearbox.loadPhyMap(tmpGearboxTable);
        SWSS_LOG_DEBUG("BOX: gearboxPhyMap size = %d.", (int) gearboxPhyMap.size());

        for (auto it = gearboxPhyMap.begin(); it != gearboxPhyMap.end(); ++it)
        {
            SWSS_LOG_NOTICE("BOX: Initialize PHY %d.", it->first);

            if (initSaiPhyApi(&it->second) != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("BOX: Failed to initialize PHY %d.", it->first);
            }
            else
            {
                SWSS_LOG_NOTICE("BOX: Created new PHY phy_id:%d phy_oid:%s.", it->second.phy_id, it->second.phy_oid.c_str());
                tmpGearboxTable->hset("phy:"+to_string(it->second.phy_id), "phy_oid", it->second.phy_oid.c_str());
                tmpGearboxTable->hset("phy:"+to_string(it->second.phy_id), "firmware_major_version", it->second.firmware_major_version.c_str());
            }
        }
    }
    delete tmpGearboxTable;
}

void getCfgSwitchType(DBConnector *cfgDb, string &switch_type)
{
    Table cfgDeviceMetaDataTable(cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);

    if (!cfgDeviceMetaDataTable.hget("localhost", "switch_type", switch_type))
    {
        //Switch type is not configured. Consider it default = "switch" (regular switch)
        switch_type = "switch";
    }

    if (switch_type != "voq" && switch_type != "fabric" && switch_type != "chassis-packet" && switch_type != "switch")
    {
        SWSS_LOG_ERROR("Invalid switch type %s configured", switch_type.c_str());
    	//If configured switch type is none of the supported, assume regular switch
        switch_type = "switch";
    }
}

bool getSystemPortConfigList(DBConnector *cfgDb, DBConnector *appDb, vector<sai_system_port_config_t> &sysportcfglist)
{
    Table cfgDeviceMetaDataTable(cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    Table cfgSystemPortTable(cfgDb, CFG_SYSTEM_PORT_TABLE_NAME);
    Table appSystemPortTable(appDb, APP_SYSTEM_PORT_TABLE_NAME);

    if (gMySwitchType != "voq")
    {
        //Non VOQ switch. Nothing to read
        return true;
    }

    string value;
    if (!cfgDeviceMetaDataTable.hget("localhost", "switch_id", value))
    {
        //VOQ switch id is not configured.
        SWSS_LOG_ERROR("VOQ switch id is not configured");
        return false;
    }

    if (value.size())
        gVoqMySwitchId = stoi(value);

    if (gVoqMySwitchId < 0)
    {
        SWSS_LOG_ERROR("Invalid VOQ switch id %d configured", gVoqMySwitchId);
        return false;
    }

    if (!cfgDeviceMetaDataTable.hget("localhost", "max_cores", value))
    {
        //VOQ max cores is not configured.
        SWSS_LOG_ERROR("VOQ max cores is not configured");
        return false;
    }

    if (value.size())
        gVoqMaxCores = stoi(value);

    if (gVoqMaxCores == 0)
    {
        SWSS_LOG_ERROR("Invalid VOQ max cores %d configured", gVoqMaxCores);
        return false;
    }

    if (!cfgDeviceMetaDataTable.hget("localhost", "hostname", value))
    {
        // hostname is not configured.
        SWSS_LOG_ERROR("Host name is not configured");
        return false;
    }
    gMyHostName = value;

    if (!gMyHostName.size())
    {
        SWSS_LOG_ERROR("Invalid host name %s configured", gMyHostName.c_str());
        return false;
    }

    if (!cfgDeviceMetaDataTable.hget("localhost", "asic_name", value))
    {
        // asic_name is not configured.
        SWSS_LOG_ERROR("Asic name is not configured");
        return false;
    }
    gMyAsicName = value;

    if (!gMyAsicName.size())
    {
        SWSS_LOG_ERROR("Invalid asic name %s configured", gMyAsicName.c_str());
        return false;
    }

    vector<string> spKeys;
    cfgSystemPortTable.getKeys(spKeys);

    //Retrieve system port configurations
    vector<FieldValueTuple> spFv;
    sai_system_port_config_t sysport;
    for (auto &k : spKeys)
    {
        cfgSystemPortTable.get(k, spFv);

        for (auto &fv : spFv)
        {
            if (fv.first == "switch_id")
            {
                sysport.attached_switch_id = stoi(fv.second);
                continue;
            }
            if (fv.first == "core_index")
            {
                sysport.attached_core_index = stoi(fv.second);
                continue;
            }
            if (fv.first == "core_port_index")
            {
                sysport.attached_core_port_index = stoi(fv.second);
                continue;
            }
            if (fv.first == "speed")
            {
                sysport.speed = stoi(fv.second);
                continue;
            }
            if (fv.first == "system_port_id")
            {
                sysport.port_id = stoi(fv.second);
                continue;
            }
            if (fv.first == "num_voq")
            {
                sysport.num_voq = stoi(fv.second);
                continue;
            }
        }
        //Add to system port config list
        sysportcfglist.push_back(sysport);

        //Also push to APP DB
        appSystemPortTable.set(k, spFv);
    }

    SWSS_LOG_NOTICE("Created System Port config list for %d system ports", (int32_t) sysportcfglist.size());

    return true;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("orchagent");

    SWSS_LOG_ENTER();

    WarmStart::initialize("orchagent", "swss");
    WarmStart::checkWarmStart("orchagent", "swss");

    if (signal(SIGHUP, sighup_handler) == SIG_ERR)
    {
        SWSS_LOG_ERROR("failed to setup SIGHUP action");
        exit(1);
    }

    int opt;
    sai_status_t status;

    string record_location = ".";
    string swss_rec_filename = "swss.rec";
    string sairedis_rec_filename = "sairedis.rec";
    string responsepublisher_rec_filename = "responsepublisher.rec";
    int record_type = 3; // Only swss and sairedis recordings enabled by default.

    while ((opt = getopt(argc, argv, "b:m:r:f:j:d:i:hsz:k:")) != -1)
    {
        switch (opt)
        {
        case 'b':
            gBatchSize = atoi(optarg);
            break;
        case 'i':
            {
                // Limit asic instance string max length
                size_t len = strnlen(optarg, SAI_MAX_HARDWARE_ID_LEN);
                // Check if input is longer and warn
                if (len == SAI_MAX_HARDWARE_ID_LEN && optarg[len+1] != '\0')
                {
                    SWSS_LOG_WARN("ASIC instance_id length > SAI_MAX_HARDWARE_ID_LEN, LIMITING !!");
                }
                // If longer, truncate into a string
                gAsicInstance.assign(optarg, len);
            }
            break;
        case 'm':
            gMacAddress = MacAddress(optarg);
            break;
        case 'r':
            // Disable all recordings if atoi() fails i.e. returns 0 due to
            // invalid command line argument.
            record_type = atoi(optarg);
            if (record_type < 0 || record_type > 7) 
            {
                usage();
                exit(EXIT_FAILURE);
            }
            break;
        case 'd':
            record_location = optarg;
            if (access(record_location.c_str(), W_OK))
            {
                SWSS_LOG_ERROR("Failed to access writable directory %s", record_location.c_str());
                exit(EXIT_FAILURE);
            }
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 's':
            gSyncMode = true;
            SWSS_LOG_NOTICE("Enabling synchronous mode");
            break;
        case 'z':
            sai_deserialize_redis_communication_mode(optarg, gRedisCommunicationMode);
            break;
        case 'f':

            if (optarg)
            {
                swss_rec_filename = optarg;
            }
            break;
        case 'j':
            if (optarg)
            {
                sairedis_rec_filename = optarg;
            }
            break;
        case 'k':
            {
                auto limit = atoi(optarg);
                if (limit > 0)
                {
                    gMaxBulkSize = limit;
                    SWSS_LOG_NOTICE("Setting maximum bulk size in bulk mode as %zu", gMaxBulkSize);
                }
                else
                {
                    SWSS_LOG_ERROR("Invalid input for maximum bulk size in bulk mode: %d. Ignoring.", limit);
                }
            }
            break;
        default: /* '?' */
            exit(EXIT_FAILURE);
        }
    }

    SWSS_LOG_NOTICE("--- Starting Orchestration Agent ---");

    initSaiApi();
    initSaiRedis(record_location, sairedis_rec_filename);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
    attr.value.booldata = true;
    attrs.push_back(attr);
    attr.id = SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY;
    attr.value.ptr = (void *)on_fdb_event;
    attrs.push_back(attr);

    // Initialize recording parameters.
    gSairedisRecord =
        (record_type & SAIREDIS_RECORD_ENABLE) == SAIREDIS_RECORD_ENABLE;
    gSwssRecord = (record_type & SWSS_RECORD_ENABLE) == SWSS_RECORD_ENABLE;
    gResponsePublisherRecord =
        (record_type & RESPONSE_PUBLISHER_RECORD_ENABLE) ==
        RESPONSE_PUBLISHER_RECORD_ENABLE;

    /* Disable/enable SwSS recording */
    if (gSwssRecord)
    {
        gRecordFile = record_location + "/" + swss_rec_filename;
        gRecordOfs.open(gRecordFile, std::ofstream::out | std::ofstream::app);
        if (!gRecordOfs.is_open())
        {
            SWSS_LOG_ERROR("Failed to open SwSS recording file %s", gRecordFile.c_str());
            exit(EXIT_FAILURE);
        }
        gRecordOfs << getTimestamp() << "|recording started" << endl;
    }

    // Disable/Enable response publisher recording.
    if (gResponsePublisherRecord) 
    {
        gResponsePublisherRecordFile = record_location + "/" + responsepublisher_rec_filename;
        gResponsePublisherRecordOfs.open(gResponsePublisherRecordFile, std::ofstream::out | std::ofstream::app);
        if (!gResponsePublisherRecordOfs.is_open())
        {
            SWSS_LOG_ERROR("Failed to open Response Publisher recording file %s",
                    gResponsePublisherRecordFile.c_str());
            gResponsePublisherRecord = false;
        } 
        else 
        {
            gResponsePublisherRecordOfs << getTimestamp() << "|recording started"
                << endl;
        }
    }

    attr.id = SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY;
    attr.value.ptr = (void *)on_port_state_change;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY;
    attr.value.ptr = (void *)on_switch_shutdown_request;
    attrs.push_back(attr);

    // Instantiate database connectors
    DBConnector appl_db("APPL_DB", 0);
    DBConnector config_db("CONFIG_DB", 0);
    DBConnector state_db("STATE_DB", 0);

    // Get switch_type
    getCfgSwitchType(&config_db, gMySwitchType);

    if (gMySwitchType != "fabric" && gMacAddress)
    {
        attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
        memcpy(attr.value.mac, gMacAddress.getMac(), 6);
        attrs.push_back(attr);
    }

    // SAI_REDIS_SWITCH_ATTR_SYNC_MODE attribute only setBuffer and g_syncMode to true
    // since it is not using ASIC_DB, we can execute it before create_switch
    // when g_syncMode is set to true here, create_switch will wait the response from syncd
    if (gSyncMode)
    {
        SWSS_LOG_WARN("sync mode is depreacated, use -z param");

        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC;
    }

    attr.id = SAI_REDIS_SWITCH_ATTR_REDIS_COMMUNICATION_MODE;
    attr.value.s32 = gRedisCommunicationMode;

    sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (!gAsicInstance.empty())
    {
        attr.id = SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO;
        attr.value.s8list.count = (uint32_t)gAsicInstance.size();
        // TODO: change SAI definition of `list` to `const char *`
        attr.value.s8list.list = (int8_t *)const_cast<char *>(gAsicInstance.c_str());
        attrs.push_back(attr);
    }

    // Get info required for VOQ system and connect to CHASSISS_APP_DB
    shared_ptr<DBConnector> chassis_app_db;
    vector<sai_system_port_config_t> sysportconfiglist;
    if ((gMySwitchType == "voq") &&
            (getSystemPortConfigList(&config_db, &appl_db, sysportconfiglist)))
    {
        attr.id = SAI_SWITCH_ATTR_TYPE;
        attr.value.u32 = SAI_SWITCH_TYPE_VOQ;
        attrs.push_back(attr);

        attr.id = SAI_SWITCH_ATTR_SWITCH_ID;
        attr.value.u32 = gVoqMySwitchId;
        attrs.push_back(attr);

        attr.id = SAI_SWITCH_ATTR_MAX_SYSTEM_CORES;
        attr.value.u32 = gVoqMaxCores;
        attrs.push_back(attr);

        gCfgSystemPorts = (uint32_t) sysportconfiglist.size();
        if (gCfgSystemPorts)
        {
            attr.id = SAI_SWITCH_ATTR_SYSTEM_PORT_CONFIG_LIST;
            attr.value.sysportconfiglist.count = gCfgSystemPorts;
            attr.value.sysportconfiglist.list = sysportconfiglist.data();
            attrs.push_back(attr);
        }
        else
        {
            SWSS_LOG_ERROR("Voq switch create with 0 system ports!");
            exit(EXIT_FAILURE);
        }

        //Connect to CHASSIS_APP_DB in redis-server in control/supervisor card as per
        //connection info in database_config.json
        chassis_app_db = make_shared<DBConnector>("CHASSIS_APP_DB", 0, true);
    }
    else if (gMySwitchType == "fabric")
    {
        SWSS_LOG_NOTICE("Switch type is fabric");
        attr.id = SAI_SWITCH_ATTR_TYPE;
        attr.value.u32 = SAI_SWITCH_TYPE_FABRIC;
        attrs.push_back(attr);
    }

    /* Must be last Attribute */
    attr.id = SAI_REDIS_SWITCH_ATTR_CONTEXT;
    attr.value.u64 = gSwitchId;
    attrs.push_back(attr);

    if (gMySwitchType == "voq" || gMySwitchType == "fabric" || gMySwitchType == "chassis-packet")
    {
        /* We set this long timeout in order for orchagent to wait enough time for
         * response from syncd. It is needed since switch create takes more time
         * than default time to create switch if there are lots of front panel ports
         * and systems ports to initialize
         */

        if (gMySwitchType == "voq" || gMySwitchType == "chassis-packet")
        {
            attr.value.u64 = (5 * SAI_REDIS_DEFAULT_SYNC_OPERATION_RESPONSE_TIMEOUT);
        }
        else if (gMySwitchType == "fabric")
        {
            attr.value.u64 = (10 * SAI_REDIS_DEFAULT_SYNC_OPERATION_RESPONSE_TIMEOUT);
        }

        attr.id = SAI_REDIS_SWITCH_ATTR_SYNC_OPERATION_RESPONSE_TIMEOUT;
        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("Failed to set SAI REDIS response timeout");
        }
        else
        {
            SWSS_LOG_NOTICE("SAI REDIS response timeout set successfully to %" PRIu64 " ", attr.value.u64);
        }
    }

    status = sai_switch_api->create_switch(&gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create a switch, rv:%d", status);
        exit(EXIT_FAILURE);
    }
    SWSS_LOG_NOTICE("Create a switch, id:%" PRIu64, gSwitchId);

    if (gMySwitchType == "voq" || gMySwitchType == "fabric" || gMySwitchType == "chassis-packet")
    {
        /* Set syncd response timeout back to the default value */
        attr.id = SAI_REDIS_SWITCH_ATTR_SYNC_OPERATION_RESPONSE_TIMEOUT;
        attr.value.u64 = SAI_REDIS_DEFAULT_SYNC_OPERATION_RESPONSE_TIMEOUT;
        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("Failed to set SAI REDIS response timeout to default");
        }
        else
        {
            SWSS_LOG_NOTICE("SAI REDIS response timeout set successfully to default: %" PRIu64 " ", attr.value.u64);
        }
    }

    if (gMySwitchType != "fabric")
    {
        /* Get switch source MAC address if not provided */
        if (!gMacAddress)
        {
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to get MAC address from switch, rv:%d", status);
                exit(EXIT_FAILURE);
            }
            else
            {
                gMacAddress = attr.value.mac;
            }
        }

        /* Get the default virtual router ID */
        attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;

        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Fail to get switch virtual router ID %d", status);
            exit(EXIT_FAILURE);
        }

        gVirtualRouterId = attr.value.oid;
        SWSS_LOG_NOTICE("Get switch virtual router ID %" PRIx64, gVirtualRouterId);

        /* Get the NAT supported info */
        attr.id = SAI_SWITCH_ATTR_AVAILABLE_SNAT_ENTRY;

        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("Failed to get the SNAT available entry count, rv:%d", status);
        }
        else
        {
            if (attr.value.u32 != 0)
            {
                gIsNatSupported = true;
            }
        }

        /* Create a loopback underlay router interface */
        vector<sai_attribute_t> underlay_intf_attrs;

        sai_attribute_t underlay_intf_attr;
        underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
        underlay_intf_attr.value.oid = gVirtualRouterId;
        underlay_intf_attrs.push_back(underlay_intf_attr);

        underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
        underlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
        underlay_intf_attrs.push_back(underlay_intf_attr);

        underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
        underlay_intf_attr.value.u32 = UNDERLAY_RIF_DEFAULT_MTU;
        underlay_intf_attrs.push_back(underlay_intf_attr);

        status = sai_router_intfs_api->create_router_interface(&gUnderlayIfId, gSwitchId, (uint32_t)underlay_intf_attrs.size(), underlay_intf_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create underlay router interface %d", status);
            exit(EXIT_FAILURE);
        }

        SWSS_LOG_NOTICE("Created underlay router interface ID %" PRIx64, gUnderlayIfId);

        /* Initialize orchestration components */

        init_gearbox_phys(&appl_db);
    }

    shared_ptr<OrchDaemon> orchDaemon;
    if (gMySwitchType != "fabric")
    {
        orchDaemon = make_shared<OrchDaemon>(&appl_db, &config_db, &state_db, chassis_app_db.get());
        if (gMySwitchType == "voq")
        {
            orchDaemon->setFabricEnabled(true);
        }
    }
    else
    {
        orchDaemon = make_shared<FabricOrchDaemon>(&appl_db, &config_db, &state_db, chassis_app_db.get());
    }

    if (!orchDaemon->init())
    {
        SWSS_LOG_ERROR("Failed to initialize orchestration daemon");
        exit(EXIT_FAILURE);
    }

    /*
    * In syncd view comparison solution, apply view has been sent
    * immediately after restore is done
    */
    if (!WarmStart::isWarmStart())
    {
        syncd_apply_view();
    }

    orchDaemon->start();

    return 0;
}
