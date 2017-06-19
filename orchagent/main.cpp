extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <unistd.h>

#include <sairedis.h>
#include <logger.h>

#include "orchdaemon.h"
#include "saihelper.h"
#include <signal.h>

using namespace std;
using namespace swss;

extern sai_switch_notification_t switch_notifications;

extern sai_switch_api_t *sai_switch_api;
extern sai_router_interface_api_t *sai_router_intfs_api;

#define UNREFERENCED_PARAMETER(P)       (P)

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
MacAddress gMacAddress;

#define DEFAULT_BATCH_SIZE  128
int gBatchSize = DEFAULT_BATCH_SIZE;

bool gSairedisRecord = true;
bool gSwssRecord = true;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

/* Global database mutex */
mutex gDbMutex;

string getTimestamp()
{
    char buffer[64];
    struct timeval tv;
    gettimeofday(&tv, NULL);

    size_t size = strftime(buffer, 32 ,"%Y-%m-%d.%T.", localtime(&tv.tv_sec));
    snprintf(&buffer[size], 32, "%06ld", tv.tv_usec);

    return string(buffer);
}

void usage()
{
    cout << "usage: orchagent [-h] [-r record_type] [-d record_location] [-b batch_size] [-m MAC]" << endl;
    cout << "    -h: display this message" << endl;
    cout << "    -r record_type: record orchagent logs with type (default 3)" << endl;
    cout << "                    0: do not record logs" << endl;
    cout << "                    1: record SAI call sequence as sairedis*.rec" << endl;
    cout << "                    2: record SwSS task sequence as swss*.rec" << endl;
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
        sai_switch_api->set_switch_attribute(&attr);
    }
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("orchagent");

    SWSS_LOG_ENTER();

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

    SWSS_LOG_NOTICE("sai_switch_api: initializing switch");
    status = sai_switch_api->initialize_switch(0, "", "", &switch_notifications);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to initialize switch %d", status);
        exit(EXIT_FAILURE);
    }

    sai_attribute_t attr;

    /* Disable/enable SAI Redis recording */
    if (gSairedisRecord)
    {
        attr.id = SAI_REDIS_SWITCH_ATTR_RECORDING_OUTPUT_DIR;
        attr.value.s8list.count = record_location.size();
        attr.value.s8list.list = (signed char *) record_location.c_str();

        status = sai_switch_api->set_switch_attribute(&attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set SAI Redis recording output folder to %s, rv:%d", record_location.c_str(), status);
            exit(EXIT_FAILURE);
        }
    }

    attr.id = SAI_REDIS_SWITCH_ATTR_RECORD;
    attr.value.booldata = gSairedisRecord;

    status = sai_switch_api->set_switch_attribute(&attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set SAI Redis recording to %s, rv:%d", gSairedisRecord ? "true" : "false", status);
        exit(EXIT_FAILURE);
    }

    /* Disable/enable SwSS recording */
    if (gSwssRecord)
    {
        gRecordFile = record_location + "/" + "swss." + getTimestamp() + ".rec";
        gRecordOfs.open(gRecordFile);
        if (!gRecordOfs.is_open())
        {
            SWSS_LOG_ERROR("Failed to open SwSS recording file %s", gRecordFile.c_str());
            exit(EXIT_FAILURE);
        }
    }

    SWSS_LOG_NOTICE("Notify syncd INIT_VIEW");

    attr.id = SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD;
    attr.value.s32 = SAI_REDIS_NOTIFY_SYNCD_INIT_VIEW;

    status = sai_switch_api->set_switch_attribute(&attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to notify syncd INIT_VIEW %d", status);
        exit(EXIT_FAILURE);
    }

    SWSS_LOG_NOTICE("Enable redis pipeline");

    attr.id = SAI_REDIS_SWITCH_ATTR_USE_PIPELINE;
    attr.value.booldata = true;

    status = sai_switch_api->set_switch_attribute(&attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to enable redis pipeline %d", status);
        exit(EXIT_FAILURE);
    }

    attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
    if (!gMacAddress)
    {
        status = sai_switch_api->get_switch_attribute(1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get MAC address from switch %d", status);
            exit(EXIT_FAILURE);
        }
        else
        {
            gMacAddress = attr.value.mac;
        }
    }
    else
    {
        memcpy(attr.value.mac, gMacAddress.getMac(), 6);
        status = sai_switch_api->set_switch_attribute(&attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set MAC address to switch %d", status);
            exit(EXIT_FAILURE);
        }
    }

    /* Get the default virtual router ID */
    attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;

    status = sai_switch_api->get_switch_attribute(1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Fail to get switch virtual router ID %d", status);
        exit(EXIT_FAILURE);
    }

    gVirtualRouterId = attr.value.oid;
    SWSS_LOG_NOTICE("Get switch virtual router ID %lx", gVirtualRouterId);

    /* Create a loopback underlay router interface */
    sai_attribute_t underlay_intf_attrs[2];
    underlay_intf_attrs[0].id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    underlay_intf_attrs[0].value.oid = gVirtualRouterId;
    underlay_intf_attrs[1].id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    underlay_intf_attrs[1].value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;

    status = sai_router_intfs_api->create_router_interface(&gUnderlayIfId, 2, underlay_intf_attrs);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create underlay router interface %d", status);
        exit(EXIT_FAILURE);
    }

    SWSS_LOG_NOTICE("Created underlay router interface ID %lx", gUnderlayIfId);

    /* Initialize orchestration components */
    DBConnector *appl_db = new DBConnector(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    OrchDaemon *orchDaemon = new OrchDaemon(appl_db);
    if (!orchDaemon->init())
    {
        SWSS_LOG_ERROR("Failed to initialize orchstration daemon");
        exit(EXIT_FAILURE);
    }

    try
    {
        SWSS_LOG_NOTICE("Notify syncd APPLY_VIEW");

        attr.id = SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD;
        attr.value.s32 = SAI_REDIS_NOTIFY_SYNCD_APPLY_VIEW;
        status = sai_switch_api->set_switch_attribute(&attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to notify syncd APPLY_VIEW %d", status);
            exit(EXIT_FAILURE);
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
