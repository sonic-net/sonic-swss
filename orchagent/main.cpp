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
#include "orchdaemon.h"
#include "logger.h"

using namespace std;
using namespace swss;

extern sai_switch_notification_t switch_notifications;

#define UNREFERENCED_PARAMETER(P)       (P)

/* Initialize all global api pointers */
sai_switch_api_t*           sai_switch_api;
sai_virtual_router_api_t*   sai_virtual_router_api;
sai_port_api_t*             sai_port_api;
sai_vlan_api_t*             sai_vlan_api;
sai_router_interface_api_t* sai_router_intfs_api;
sai_hostif_api_t*           sai_hostif_api;
sai_neighbor_api_t*         sai_neighbor_api;
sai_next_hop_api_t*         sai_next_hop_api;
sai_next_hop_group_api_t*   sai_next_hop_group_api;
sai_route_api_t*            sai_route_api;
sai_lag_api_t*              sai_lag_api;
sai_policer_api_t*          sai_policer_api;
sai_tunnel_api_t*           sai_tunnel_api;
sai_queue_api_t*            sai_queue_api;
sai_scheduler_api_t*        sai_scheduler_api;
sai_scheduler_group_api_t*  sai_scheduler_group_api;
sai_wred_api_t*             sai_wred_api;
sai_qos_map_api_t*          sai_qos_map_api;
sai_buffer_api_t*           sai_buffer_api;
sai_acl_api_t*              sai_acl_api;
sai_mirror_api_t*           sai_mirror_api;
sai_fdb_api_t*              sai_fdb_api;

/* Global variables */
map<string, string> gProfileMap;
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
MacAddress gMacAddress;

bool gSairedisRecord = true;
bool gSwssRecord = true;
ofstream gRecordOfs;
string gRecordFile;

/* Global database mutex */
mutex gDbMutex;

const char *test_profile_get_value (
    _In_ sai_switch_profile_id_t profile_id,
    _In_ const char *variable)
{
    SWSS_LOG_ENTER();

    auto it = gProfileMap.find(variable);

    if (it == gProfileMap.end())
        return NULL;
    return it->second.c_str();
}

int test_profile_get_next_value (
    _In_ sai_switch_profile_id_t profile_id,
    _Out_ const char **variable,
    _Out_ const char **value)
{
    SWSS_LOG_ENTER();

    static auto it = gProfileMap.begin();

    if (value == NULL)
    {
        // Restarts enumeration
        it = gProfileMap.begin();
    }
    else if (it == gProfileMap.end())
    {
        return -1;
    }
    else
    {
        *variable = it->first.c_str();
        *value = it->second.c_str();
        it++;
    }

    if (it != gProfileMap.end())
        return 0;
    else
        return -1;
}

const service_method_table_t test_services = {
    test_profile_get_value,
    test_profile_get_next_value
};

void initSaiApi()
{
    SWSS_LOG_ENTER();

    sai_api_initialize(0, (service_method_table_t *)&test_services);

    sai_api_query(SAI_API_SWITCH,               (void **)&sai_switch_api);
    sai_api_query(SAI_API_VIRTUAL_ROUTER,       (void **)&sai_virtual_router_api);
    sai_api_query(SAI_API_PORT,                 (void **)&sai_port_api);
    sai_api_query(SAI_API_FDB,                  (void **)&sai_fdb_api);
    sai_api_query(SAI_API_VLAN,                 (void **)&sai_vlan_api);
    sai_api_query(SAI_API_HOST_INTERFACE,       (void **)&sai_hostif_api);
    sai_api_query(SAI_API_MIRROR,               (void **)&sai_mirror_api);
    sai_api_query(SAI_API_ROUTER_INTERFACE,     (void **)&sai_router_intfs_api);
    sai_api_query(SAI_API_NEIGHBOR,             (void **)&sai_neighbor_api);
    sai_api_query(SAI_API_NEXT_HOP,             (void **)&sai_next_hop_api);
    sai_api_query(SAI_API_NEXT_HOP_GROUP,       (void **)&sai_next_hop_group_api);
    sai_api_query(SAI_API_ROUTE,                (void **)&sai_route_api);
    sai_api_query(SAI_API_LAG,                  (void **)&sai_lag_api);
    sai_api_query(SAI_API_POLICER,              (void **)&sai_policer_api);
    sai_api_query(SAI_API_TUNNEL,               (void **)&sai_tunnel_api);
    sai_api_query(SAI_API_QUEUE,                (void **)&sai_queue_api);
    sai_api_query(SAI_API_SCHEDULER,            (void **)&sai_scheduler_api);
    sai_api_query(SAI_API_WRED,                 (void **)&sai_wred_api);
    sai_api_query(SAI_API_QOS_MAPS,             (void **)&sai_qos_map_api);
    sai_api_query(SAI_API_BUFFERS,              (void **)&sai_buffer_api);
    sai_api_query(SAI_API_SCHEDULER_GROUP,      (void **)&sai_scheduler_group_api);
    sai_api_query(SAI_API_ACL,                  (void **)&sai_acl_api);

    sai_log_set(SAI_API_SWITCH,                 SAI_LOG_NOTICE);
    sai_log_set(SAI_API_VIRTUAL_ROUTER,         SAI_LOG_NOTICE);
    sai_log_set(SAI_API_PORT,                   SAI_LOG_NOTICE);
    sai_log_set(SAI_API_FDB,                    SAI_LOG_NOTICE);
    sai_log_set(SAI_API_VLAN,                   SAI_LOG_NOTICE);
    sai_log_set(SAI_API_HOST_INTERFACE,         SAI_LOG_NOTICE);
    sai_log_set(SAI_API_MIRROR,                 SAI_LOG_NOTICE);
    sai_log_set(SAI_API_ROUTER_INTERFACE,       SAI_LOG_NOTICE);
    sai_log_set(SAI_API_NEIGHBOR,               SAI_LOG_NOTICE);
    sai_log_set(SAI_API_NEXT_HOP,               SAI_LOG_NOTICE);
    sai_log_set(SAI_API_NEXT_HOP_GROUP,         SAI_LOG_NOTICE);
    sai_log_set(SAI_API_ROUTE,                  SAI_LOG_NOTICE);
    sai_log_set(SAI_API_LAG,                    SAI_LOG_NOTICE);
    sai_log_set(SAI_API_POLICER,                SAI_LOG_NOTICE);
    sai_log_set(SAI_API_TUNNEL,                 SAI_LOG_NOTICE);
    sai_log_set(SAI_API_QUEUE,                  SAI_LOG_NOTICE);
    sai_log_set(SAI_API_SCHEDULER,              SAI_LOG_NOTICE);
    sai_log_set(SAI_API_WRED,                   SAI_LOG_NOTICE);
    sai_log_set(SAI_API_QOS_MAPS,               SAI_LOG_NOTICE);
    sai_log_set(SAI_API_BUFFERS,                SAI_LOG_NOTICE);
    sai_log_set(SAI_API_SCHEDULER_GROUP,        SAI_LOG_NOTICE);
    sai_log_set(SAI_API_ACL,                    SAI_LOG_NOTICE);
}

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
    cout << "usage: orchagent [-h] [-r record_type] [-d record_location] [-m MAC]" << endl;
    cout << "    -h: display this message" << endl;
    cout << "    -r record_type: record orchagent logs with type (default 3)" << endl;
    cout << "                    0: do not record logs" << endl;
    cout << "                    1: record SAI call sequence as sairedis*.rec" << endl;
    cout << "                    2: record SwSS task sequence as swss*.rec" << endl;
    cout << "                    3: enable both above two records" << endl;
    cout << "    -d record_location: set record logs folder location (default .)" << endl;
    cout << "    -m MAC: set switch MAC address" << endl;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("orchagent");

    SWSS_LOG_ENTER();

    int opt;
    sai_status_t status;

    string record_location = ".";

    while ((opt = getopt(argc, argv, "m:r:d:h")) != -1)
    {
        switch (opt)
        {
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
        gRecordFile = "swss." + getTimestamp() + ".rec";
        gRecordOfs.open(record_location + "/" + gRecordFile);
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
