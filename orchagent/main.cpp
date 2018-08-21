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

#include <sys/time.h>
#include "timestamp.h"

#include <sairedis.h>
#include <logger.h>

#include "orchdaemon.h"
#include "saihelper.h"
#include "notifications.h"
#include <signal.h>
#include "warm_restart.h"

using namespace std;
using namespace swss;

extern sai_switch_api_t *sai_switch_api;
extern sai_router_interface_api_t *sai_router_intfs_api;

#define UNREFERENCED_PARAMETER(P)       (P)

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
MacAddress gMacAddress;

#define DEFAULT_BATCH_SIZE  128
int gBatchSize = DEFAULT_BATCH_SIZE;

bool gSairedisRecord = true;
bool gSwssRecord = true;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

void usage()
{
    cout << "usage: orchagent [-h] [-r record_type] [-d record_location] [-b batch_size] [-m MAC]" << endl;
    cout << "    -h: display this message" << endl;
    cout << "    -r record_type: record orchagent logs with type (default 3)" << endl;
    cout << "                    0: do not record logs" << endl;
    cout << "                    1: record SAI call sequence as sairedis.rec" << endl;
    cout << "                    2: record SwSS task sequence as swss.rec" << endl;
    cout << "                    3: enable both above two records" << endl;
    cout << "    -d record_location: set record logs folder location (default .)" << endl;
    cout << "    -b batch_size: set consumer table pop operation batch size (default 128)" << endl;
    cout << "    -m MAC: set switch MAC address" << endl;
}

void sighup_handler(int signo)
{
    /*
     * Don't do any logging since they are using mutexes.
     */
    gLogRotate = true;

    sai_attribute_t attr;
    attr.id = SAI_REDIS_SWITCH_ATTR_PERFORM_LOG_ROTATE;
    attr.value.booldata = true;

    if (sai_switch_api != NULL)
    {
        sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    }
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

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("orchagent");

    SWSS_LOG_ENTER();
    WarmStart::checkWarmStart("orchagent");

    if (signal(SIGHUP, sighup_handler) == SIG_ERR)
    {
        SWSS_LOG_ERROR("failed to setup SIGHUP action");
        exit(1);
    }

    int opt;
    sai_status_t status;

    string record_location = ".";

    while ((opt = getopt(argc, argv, "b:m:r:d:h")) != -1)
    {
        switch (opt)
        {
        case 'b':
            gBatchSize = atoi(optarg);
            break;
        case 'm':
            gMacAddress = MacAddress(optarg);
            break;
        case 'r':
            if (!strcmp(optarg, "0"))
            {
                gSairedisRecord = false;
                gSwssRecord = false;
            }
            else if (!strcmp(optarg, "1"))
            {
                gSwssRecord = false;
            }
            else if (!strcmp(optarg, "2"))
            {
                gSairedisRecord = false;
            }
            else if (!strcmp(optarg, "3"))
            {
                continue; /* default behavior */
            }
            else
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
        default: /* '?' */
            exit(EXIT_FAILURE);
        }
    }

    SWSS_LOG_NOTICE("--- Starting Orchestration Agent ---");

    initSaiApi();
    initSaiRedis(record_location);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY;
    attr.value.ptr = (void *)on_fdb_event;
    attrs.push_back(attr);

    /* Disable/enable SwSS recording */
    if (gSwssRecord)
    {
        gRecordFile = record_location + "/" + "swss.rec";
        gRecordOfs.open(gRecordFile, std::ofstream::out | std::ofstream::app);
        if (!gRecordOfs.is_open())
        {
            SWSS_LOG_ERROR("Failed to open SwSS recording file %s", gRecordFile.c_str());
            exit(EXIT_FAILURE);
        }
        gRecordOfs << getTimestamp() << "|recording started" << endl;
    }

    attr.id = SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY;
    attr.value.ptr = (void *)on_port_state_change;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY;
    attr.value.ptr = (void *)on_switch_shutdown_request;
    attrs.push_back(attr);

    if (gMacAddress)
    {
        attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
        memcpy(attr.value.mac, gMacAddress.getMac(), 6);
        attrs.push_back(attr);
    }

    status = sai_switch_api->create_switch(&gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create a switch, rv:%d", status);
        exit(EXIT_FAILURE);
    }
    SWSS_LOG_NOTICE("Create a switch");

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
    SWSS_LOG_NOTICE("Get switch virtual router ID %lx", gVirtualRouterId);

    /* Create a loopback underlay router interface */
    vector<sai_attribute_t> underlay_intf_attrs;

    sai_attribute_t underlay_intf_attr;
    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    underlay_intf_attr.value.oid = gVirtualRouterId;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    underlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    status = sai_router_intfs_api->create_router_interface(&gUnderlayIfId, gSwitchId, (uint32_t)underlay_intf_attrs.size(), underlay_intf_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create underlay router interface %d", status);
        exit(EXIT_FAILURE);
    }

    SWSS_LOG_NOTICE("Created underlay router interface ID %lx", gUnderlayIfId);

    /* Initialize orchestration components */
    DBConnector appl_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector config_db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector state_db(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

    auto orchDaemon = make_shared<OrchDaemon>(&appl_db, &config_db, &state_db);

    try
    {
        if (!orchDaemon->init())
        {
            SWSS_LOG_ERROR("Failed to initialize orchstration daemon");
            exit(EXIT_FAILURE);
        }

        if (!WarmStart::isWarmStart())
        {
            syncd_apply_view();
        }

        orchDaemon->start();
    }
    catch (char const *e)
    {
        SWSS_LOG_ERROR("Exception: %s", e);
    }
    catch (exception& e)
    {
        SWSS_LOG_ERROR("Failed due to exception: %s", e.what());
    }

    return 0;
}
