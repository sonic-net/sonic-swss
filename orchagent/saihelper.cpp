extern "C" {

#include "sai.h"
#include "saistatus.h"
#include "saiextensions.h"
#include "sairedis.h"
}

#include <inttypes.h>
#include <string.h>
#include <fstream>
#include <map>
#include <logger.h>
#include <sairedis.h>
#include <set>
#include <tuple>
#include <vector>
#include <linux/limits.h>
#include <net/if.h>
#include "timestamp.h"
#include "sai_serialize.h"
#include "saihelper.h"
#include "orch.h"

using namespace std;
using namespace swss;

#define _STR(s) #s
#define STR(s) _STR(s)

#define CONTEXT_CFG_FILE "/usr/share/sonic/hwsku/context_config.json"
#define SAI_REDIS_SYNC_OPERATION_RESPONSE_TIMEOUT (480*1000)

// hwinfo = "INTERFACE_NAME/PHY ID", mii_ioctl_data->phy_id is a __u16
#define HWINFO_MAX_SIZE IFNAMSIZ + 1 + 5

/* Initialize all global api pointers */
sai_switch_api_t*           sai_switch_api;
sai_bridge_api_t*           sai_bridge_api;
sai_virtual_router_api_t*   sai_virtual_router_api;
sai_port_api_t*             sai_port_api;
sai_vlan_api_t*             sai_vlan_api;
sai_router_interface_api_t* sai_router_intfs_api;
sai_hostif_api_t*           sai_hostif_api;
sai_neighbor_api_t*         sai_neighbor_api;
sai_next_hop_api_t*         sai_next_hop_api;
sai_next_hop_group_api_t*   sai_next_hop_group_api;
sai_route_api_t*            sai_route_api;
sai_mpls_api_t*             sai_mpls_api;
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
sai_hash_api_t*             sai_hash_api;
sai_udf_api_t*              sai_udf_api;
sai_mirror_api_t*           sai_mirror_api;
sai_fdb_api_t*              sai_fdb_api;
sai_dtel_api_t*             sai_dtel_api;
sai_samplepacket_api_t*     sai_samplepacket_api;
sai_debug_counter_api_t*    sai_debug_counter_api;
sai_nat_api_t*              sai_nat_api;
sai_isolation_group_api_t*  sai_isolation_group_api;
sai_system_port_api_t*      sai_system_port_api;
sai_macsec_api_t*           sai_macsec_api;
sai_srv6_api_t**            sai_srv6_api;;
sai_l2mc_group_api_t*       sai_l2mc_group_api;
sai_counter_api_t*          sai_counter_api;
sai_bfd_api_t*              sai_bfd_api;
sai_my_mac_api_t*           sai_my_mac_api;
sai_generic_programmable_api_t* sai_generic_programmable_api;
sai_dash_appliance_api_t*           sai_dash_appliance_api;
sai_dash_acl_api_t*                 sai_dash_acl_api;
sai_dash_vnet_api_t                 sai_dash_vnet_api;
sai_dash_outbound_ca_to_pa_api_t*   sai_dash_outbound_ca_to_pa_api;
sai_dash_pa_validation_api_t *      sai_dash_pa_validation_api;
sai_dash_outbound_routing_api_t*    sai_dash_outbound_routing_api;
sai_dash_inbound_routing_api_t*     sai_dash_inbound_routing_api;
sai_dash_eni_api_t*                 sai_dash_eni_api;
sai_dash_vip_api_t*                 sai_dash_vip_api;
sai_dash_direction_lookup_api_t*    sai_dash_direction_lookup_api;
sai_twamp_api_t*                    sai_twamp_api;
sai_tam_api_t*                      sai_tam_api;
sai_stp_api_t*                      sai_stp_api;

extern sai_object_id_t gSwitchId;
extern bool gTraditionalFlexCounter;

vector<sai_object_id_t> gGearboxOids;

unique_ptr<DBConnector> gFlexCounterDb;
unique_ptr<ProducerTable> gFlexCounterGroupTable;
unique_ptr<ProducerTable> gFlexCounterTable;
unique_ptr<DBConnector> gGearBoxFlexCounterDb;
unique_ptr<ProducerTable> gGearBoxFlexCounterGroupTable;
unique_ptr<ProducerTable> gGearBoxFlexCounterTable;

static map<string, sai_switch_hardware_access_bus_t> hardware_access_map =
{
    { "mdio",  SAI_SWITCH_HARDWARE_ACCESS_BUS_MDIO },
    { "i2c", SAI_SWITCH_HARDWARE_ACCESS_BUS_I2C },
    { "cpld", SAI_SWITCH_HARDWARE_ACCESS_BUS_CPLD }
};

map<string, string> gProfileMap;

sai_status_t mdio_read(uint64_t platform_context,
  uint32_t mdio_addr, uint32_t reg_addr,
  uint32_t number_of_registers, uint32_t *data)
{
    return SAI_STATUS_NOT_IMPLEMENTED;
}

sai_status_t mdio_write(uint64_t platform_context,
  uint32_t mdio_addr, uint32_t reg_addr,
  uint32_t number_of_registers, uint32_t *data)
{
    return SAI_STATUS_NOT_IMPLEMENTED;
}

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

const sai_service_method_table_t test_services = {
    test_profile_get_value,
    test_profile_get_next_value
};

void initSaiApi()
{
    SWSS_LOG_ENTER();

    if (ifstream(CONTEXT_CFG_FILE))
    {
        SWSS_LOG_NOTICE("Context config file %s exists", CONTEXT_CFG_FILE);
        gProfileMap[SAI_REDIS_KEY_CONTEXT_CONFIG] = CONTEXT_CFG_FILE;
    }

    sai_api_initialize(0, (const sai_service_method_table_t *)&test_services);

    sai_api_query(SAI_API_SWITCH,               (void **)&sai_switch_api);
    sai_api_query(SAI_API_BRIDGE,               (void **)&sai_bridge_api);
    sai_api_query(SAI_API_VIRTUAL_ROUTER,       (void **)&sai_virtual_router_api);
    sai_api_query(SAI_API_PORT,                 (void **)&sai_port_api);
    sai_api_query(SAI_API_FDB,                  (void **)&sai_fdb_api);
    sai_api_query(SAI_API_VLAN,                 (void **)&sai_vlan_api);
    sai_api_query(SAI_API_HOSTIF,               (void **)&sai_hostif_api);
    sai_api_query(SAI_API_MIRROR,               (void **)&sai_mirror_api);
    sai_api_query(SAI_API_ROUTER_INTERFACE,     (void **)&sai_router_intfs_api);
    sai_api_query(SAI_API_NEIGHBOR,             (void **)&sai_neighbor_api);
    sai_api_query(SAI_API_NEXT_HOP,             (void **)&sai_next_hop_api);
    sai_api_query(SAI_API_NEXT_HOP_GROUP,       (void **)&sai_next_hop_group_api);
    sai_api_query(SAI_API_ROUTE,                (void **)&sai_route_api);
    sai_api_query(SAI_API_MPLS,                 (void **)&sai_mpls_api);
    sai_api_query(SAI_API_LAG,                  (void **)&sai_lag_api);
    sai_api_query(SAI_API_POLICER,              (void **)&sai_policer_api);
    sai_api_query(SAI_API_TUNNEL,               (void **)&sai_tunnel_api);
    sai_api_query(SAI_API_QUEUE,                (void **)&sai_queue_api);
    sai_api_query(SAI_API_SCHEDULER,            (void **)&sai_scheduler_api);
    sai_api_query(SAI_API_WRED,                 (void **)&sai_wred_api);
    sai_api_query(SAI_API_QOS_MAP,              (void **)&sai_qos_map_api);
    sai_api_query(SAI_API_BUFFER,               (void **)&sai_buffer_api);
    sai_api_query(SAI_API_SCHEDULER_GROUP,      (void **)&sai_scheduler_group_api);
    sai_api_query(SAI_API_ACL,                  (void **)&sai_acl_api);
    sai_api_query(SAI_API_HASH,                 (void **)&sai_hash_api);
    sai_api_query(SAI_API_UDF,                  (void **)&sai_udf_api);
    sai_api_query(SAI_API_DTEL,                 (void **)&sai_dtel_api);
    sai_api_query(SAI_API_SAMPLEPACKET,         (void **)&sai_samplepacket_api);
    sai_api_query(SAI_API_DEBUG_COUNTER,        (void **)&sai_debug_counter_api);
    sai_api_query(SAI_API_NAT,                  (void **)&sai_nat_api);
    sai_api_query(SAI_API_ISOLATION_GROUP,      (void **)&sai_isolation_group_api);
    sai_api_query(SAI_API_SYSTEM_PORT,          (void **)&sai_system_port_api);
    sai_api_query(SAI_API_MACSEC,               (void **)&sai_macsec_api);
    sai_api_query(SAI_API_SRV6,                 (void **)&sai_srv6_api);
    sai_api_query(SAI_API_L2MC_GROUP,           (void **)&sai_l2mc_group_api);
    sai_api_query(SAI_API_COUNTER,              (void **)&sai_counter_api);
    sai_api_query(SAI_API_BFD,                  (void **)&sai_bfd_api);
    sai_api_query(SAI_API_MY_MAC,               (void **)&sai_my_mac_api);
    sai_api_query(SAI_API_GENERIC_PROGRAMMABLE, (void **)&sai_generic_programmable_api);
    sai_api_query((sai_api_t)SAI_API_DASH_APPLIANCE,            (void**)&sai_dash_appliance_api);
    sai_api_query((sai_api_t)SAI_API_DASH_ACL,                  (void**)&sai_dash_acl_api);
    sai_api_query((sai_api_t)SAI_API_DASH_VNET,                 (void**)&sai_dash_vnet_api);
    sai_api_query((sai_api_t)SAI_API_DASH_OUTBOUND_CA_TO_PA,    (void**)&sai_dash_outbound_ca_to_pa_api);
    sai_api_query((sai_api_t)SAI_API_DASH_PA_VALIDATION,        (void**)&sai_dash_pa_validation_api);
    sai_api_query((sai_api_t)SAI_API_DASH_OUTBOUND_ROUTING,     (void**)&sai_dash_outbound_routing_api);
    sai_api_query((sai_api_t)SAI_API_DASH_INBOUND_ROUTING,      (void**)&sai_dash_inbound_routing_api);
    sai_api_query((sai_api_t)SAI_API_DASH_ENI,                  (void**)&sai_dash_eni_api);
    sai_api_query((sai_api_t)SAI_API_DASH_VIP,                  (void**)&sai_dash_vip_api);
    sai_api_query((sai_api_t)SAI_API_DASH_DIRECTION_LOOKUP,     (void**)&sai_dash_direction_lookup_api);
    sai_api_query(SAI_API_TWAMP,                (void **)&sai_twamp_api);
    sai_api_query(SAI_API_TAM,                  (void **)&sai_tam_api);
    sai_api_query(SAI_API_STP,                  (void **)&sai_stp_api);

    sai_log_set(SAI_API_SWITCH,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_BRIDGE,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_VIRTUAL_ROUTER,         SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_PORT,                   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_FDB,                    SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_VLAN,                   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_HOSTIF,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_MIRROR,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_ROUTER_INTERFACE,       SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_NEIGHBOR,               SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_NEXT_HOP,               SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_NEXT_HOP_GROUP,         SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_ROUTE,                  SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_MPLS,                   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_LAG,                    SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_POLICER,                SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_TUNNEL,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_QUEUE,                  SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_SCHEDULER,              SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_WRED,                   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_QOS_MAP,                SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_BUFFER,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_SCHEDULER_GROUP,        SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_ACL,                    SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_HASH,                   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_UDF,                    SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_DTEL,                   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_SAMPLEPACKET,           SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_DEBUG_COUNTER,          SAI_LOG_LEVEL_NOTICE);
    sai_log_set((sai_api_t)SAI_API_NAT,         SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_SYSTEM_PORT,            SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_MACSEC,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_SRV6,                   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_L2MC_GROUP,             SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_COUNTER,                SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_BFD,                    SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_MY_MAC,                 SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_GENERIC_PROGRAMMABLE,   SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_TWAMP,                  SAI_LOG_LEVEL_NOTICE);
    sai_log_set(SAI_API_TAM,                    SAI_LOG_LEVEL_NOTICE);
}

void initFlexCounterTables()
{
    if (gTraditionalFlexCounter)
    {
        gFlexCounterDb = std::make_unique<DBConnector>("FLEX_COUNTER_DB", 0);
        gFlexCounterTable = std::make_unique<ProducerTable>(gFlexCounterDb.get(), FLEX_COUNTER_TABLE);
        gFlexCounterGroupTable = std::make_unique<ProducerTable>(gFlexCounterDb.get(), FLEX_COUNTER_GROUP_TABLE);

        gGearBoxFlexCounterDb = std::make_unique<DBConnector>("GB_FLEX_COUNTER_DB", 0);
        gGearBoxFlexCounterTable = std::make_unique<ProducerTable>(gGearBoxFlexCounterDb.get(), FLEX_COUNTER_TABLE);
        gGearBoxFlexCounterGroupTable = std::make_unique<ProducerTable>(gGearBoxFlexCounterDb.get(), FLEX_COUNTER_GROUP_TABLE);
    }
}

void initSaiRedis()
{
    /**
     * NOTE: Notice that all Redis attributes here are using SAI_NULL_OBJECT_ID
     * as the switch ID, because those operations don't require actual switch
     * to be performed, and they should be executed before creating switch.
     */

    sai_attribute_t attr;
    sai_status_t status;

    auto record_filename = Recorder::Instance().sairedis.getFile();
    auto record_location = Recorder::Instance().sairedis.getLoc();

    /* set recording dir before enable recording */
    if (Recorder::Instance().sairedis.isRecord())
    {
        attr.id = SAI_REDIS_SWITCH_ATTR_RECORDING_OUTPUT_DIR;
        attr.value.s8list.count = (uint32_t)record_location.size();
        attr.value.s8list.list = (int8_t*)const_cast<char *>(record_location.c_str());

        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set SAI Redis recording output folder to %s, rv:%d",
                record_location.c_str(), status);
            exit(EXIT_FAILURE);
        }

        attr.id = SAI_REDIS_SWITCH_ATTR_RECORDING_FILENAME;
        attr.value.s8list.count = (uint32_t)record_filename.size();
        attr.value.s8list.list = (int8_t*)const_cast<char *>(record_filename.c_str());

        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set SAI Redis recording logfile to %s, rv:%d",
                record_filename.c_str(), status);
            exit(EXIT_FAILURE);
        }

    }

    /* Disable/enable SAI Redis recording */
    attr.id = SAI_REDIS_SWITCH_ATTR_RECORD;
    attr.value.booldata = Recorder::Instance().sairedis.isRecord();

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to %s SAI Redis recording, rv:%d",
            Recorder::Instance().sairedis.isRecord() ? "enable" : "disable", status);
        exit(EXIT_FAILURE);
    }

    attr.id = SAI_REDIS_SWITCH_ATTR_USE_PIPELINE;
    attr.value.booldata = true;

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to enable redis pipeline, rv:%d", status);
        exit(EXIT_FAILURE);
    }
    SWSS_LOG_NOTICE("Enable redis pipeline");

    char *platform = getenv("platform");
    if (platform && (strstr(platform, MLNX_PLATFORM_SUBSTRING) || strstr(platform, XS_PLATFORM_SUBSTRING)))
    {
        /* We set this long timeout in order for Orchagent to wait enough time for
         * response from syncd. It is needed since in init, systemd syncd startup
         * script first calls FW upgrade script (that might take up to 7 minutes
         * in systems with Gearbox) and only then launches syncd container */
        attr.id = SAI_REDIS_SWITCH_ATTR_SYNC_OPERATION_RESPONSE_TIMEOUT;
        attr.value.u64 = SAI_REDIS_SYNC_OPERATION_RESPONSE_TIMEOUT;
        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set SAI REDIS response timeout");
            exit(EXIT_FAILURE);
        }

        SWSS_LOG_NOTICE("SAI REDIS response timeout set successfully to %" PRIu64 " ", attr.value.u64);
    }

    attr.id = SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD;
    attr.value.s32 = SAI_REDIS_NOTIFY_SYNCD_INIT_VIEW;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to notify syncd INIT_VIEW, rv:%d gSwitchId %" PRIx64, status, gSwitchId);
        exit(EXIT_FAILURE);
    }
    SWSS_LOG_NOTICE("Notify syncd INIT_VIEW");

    if (platform && (strstr(platform, MLNX_PLATFORM_SUBSTRING) || strstr(platform, XS_PLATFORM_SUBSTRING)))
    {
        /* Set timeout back to the default value */
        attr.id = SAI_REDIS_SWITCH_ATTR_SYNC_OPERATION_RESPONSE_TIMEOUT;
        attr.value.u64 = SAI_REDIS_DEFAULT_SYNC_OPERATION_RESPONSE_TIMEOUT;
        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set SAI REDIS response timeout");
            exit(EXIT_FAILURE);
        }

        SWSS_LOG_NOTICE("SAI REDIS response timeout set successfully to %" PRIu64 " ", attr.value.u64);
    }
}

sai_status_t initSaiPhyApi(swss::gearbox_phy_t *phy)
{
    sai_object_id_t phyOid;
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    sai_status_t status;
    char fwPath[PATH_MAX];
    char hwinfo[HWINFO_MAX_SIZE + 1];

    SWSS_LOG_ENTER();

    attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_TYPE;
    attr.value.u32 = SAI_SWITCH_TYPE_PHY;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_SWITCH_PROFILE_ID;
    attr.value.u32 = 0;
    attrs.push_back(attr);

    if( phy->hwinfo.length() > HWINFO_MAX_SIZE ) {
       SWSS_LOG_ERROR( "hwinfo string attribute is too long." );
       return SAI_STATUS_FAILURE;
    }
    strncpy(hwinfo, phy->hwinfo.c_str(), phy->hwinfo.length());

    attr.id = SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO;
    attr.value.s8list.count = (uint32_t) phy->hwinfo.length();
    attr.value.s8list.list = (int8_t *) hwinfo;
    attrs.push_back(attr);

    if (phy->firmware.length() == 0)
    {
        attr.id = SAI_SWITCH_ATTR_FIRMWARE_LOAD_METHOD;
        attr.value.u32 = SAI_SWITCH_FIRMWARE_LOAD_METHOD_NONE;
        attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_SWITCH_ATTR_FIRMWARE_LOAD_METHOD;
        attr.value.u32 = SAI_SWITCH_FIRMWARE_LOAD_METHOD_INTERNAL;
        attrs.push_back(attr);

        strncpy(fwPath, phy->firmware.c_str(), PATH_MAX - 1);

        attr.id = SAI_SWITCH_ATTR_FIRMWARE_PATH_NAME;
        attr.value.s8list.list = (int8_t *) fwPath;
        attr.value.s8list.count = (uint32_t) strlen(fwPath) + 1;
        attrs.push_back(attr);

        attr.id = SAI_SWITCH_ATTR_FIRMWARE_LOAD_TYPE;
        attr.value.u32 = SAI_SWITCH_FIRMWARE_LOAD_TYPE_AUTO;
        attrs.push_back(attr);
    }

    attr.id = SAI_SWITCH_ATTR_REGISTER_READ;
    attr.value.ptr = (void *) mdio_read;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_REGISTER_WRITE;
    attr.value.ptr = (void *) mdio_write;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_HARDWARE_ACCESS_BUS;
    attr.value.u32 = hardware_access_map[phy->access];
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_PLATFROM_CONTEXT;
    attr.value.u64 = phy->address;
    attrs.push_back(attr);

    /* Must be last Attribute */
    attr.id = SAI_REDIS_SWITCH_ATTR_CONTEXT;
    attr.value.u64 = phy->context_id;
    attrs.push_back(attr);

    status = sai_switch_api->create_switch(&phyOid, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("BOX: Failed to create PHY:%d rtn:%d", phy->phy_id, status);
        return status;
    }
    SWSS_LOG_NOTICE("BOX: Created PHY:%d Oid:0x%" PRIx64, phy->phy_id, phyOid);

    phy->phy_oid = sai_serialize_object_id(phyOid);

    if (phy->firmware.length() != 0)
    {
        attr.id = SAI_SWITCH_ATTR_FIRMWARE_MAJOR_VERSION;
        status = sai_switch_api->get_switch_attribute(phyOid, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("BOX: Failed to get firmware major version for hwinfo:%s, phy:%d, rtn:%d",
                           phy->hwinfo.c_str(), phy->phy_id, status);
            return status;
        }
        else
        {
            phy->firmware_major_version = string(attr.value.chardata);
        }
    }

    gGearboxOids.push_back(phyOid);

    return status;
}

task_process_status handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis create
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses (e.g., SAI_STATUS_ITEM_ALREADY_EXISTS)
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    switch (api)
    {
        case SAI_API_FDB:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiCreateStatus");
                    return task_success;
                case SAI_STATUS_ITEM_ALREADY_EXISTS:
                    /*
                     *  In FDB creation, there are scenarios where the hardware learns an FDB entry before orchagent.
                     *  In such cases, the FDB SAI creation would report the status of SAI_STATUS_ITEM_ALREADY_EXISTS,
                     *  and orchagent should ignore the error and treat it as entry was explicitly created.
                     */
                    return task_success;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        case SAI_API_HOSTIF:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    return task_success;
                case SAI_STATUS_FAILURE:
                    /*
                     * Host interface maybe failed due to lane not available.
                     * In some scenarios, like SONiC virtual machine, the invalid lane may be not enabled by VM configuration,
                     * So just ignore the failure and report an error log.
                     */
                    return task_ignore;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        case SAI_API_ROUTE:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiCreateStatus");
                    return task_success;
                case SAI_STATUS_ITEM_ALREADY_EXISTS:
                case SAI_STATUS_NOT_EXECUTED:
                    /* With VNET routes, the same route can be learned via multiple
                    sources, like via BGP. Handle this gracefully */
                    return task_success;
                case SAI_STATUS_TABLE_FULL:
                    return task_need_retry;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        case SAI_API_NEIGHBOR:
        case SAI_API_NEXT_HOP:
        case SAI_API_NEXT_HOP_GROUP:
            switch(status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiCreateStatus");
                    return task_success;
                case SAI_STATUS_ITEM_ALREADY_EXISTS:
                    return task_success;
                case SAI_STATUS_TABLE_FULL:
                    return task_need_retry;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        default:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiCreateStatus");
                    return task_success;
                default:
                    SWSS_LOG_ERROR("Encountered failure in create operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
    }
    return task_need_retry;
}

task_process_status handleSaiSetStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis set
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    if (status == SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiSetStatus");
        return task_success;
    }

    switch (api)
    {
        case SAI_API_PORT:
            switch (status)
            {
                case SAI_STATUS_INVALID_ATTR_VALUE_0:
                    /*
                     * If user gives an invalid attribute value, no need to retry or exit orchagent, just fail the current task
                     * and let user correct the configuration.
                     */
                    SWSS_LOG_ERROR("Encountered SAI_STATUS_INVALID_ATTR_VALUE_0 in set operation, task failed, SAI API: %s, status: %s",
                            sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    return task_failed;
                default:
                    SWSS_LOG_ERROR("Encountered failure in set operation, exiting orchagent, SAI API: %s, status: %s",
                            sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        case SAI_API_TUNNEL:
            switch (status)
            {
                case SAI_STATUS_ATTR_NOT_SUPPORTED_0:
                    SWSS_LOG_ERROR("Encountered SAI_STATUS_ATTR_NOT_SUPPORTED_0 in set operation, task failed, SAI API: %s, status: %s",
                            sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    return task_failed;
                default:
                    SWSS_LOG_ERROR("Encountered failure in set operation, exiting orchagent, SAI API: %s, status: %s",
                            sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        case SAI_API_BUFFER:
            switch (status)
            {
                case SAI_STATUS_INSUFFICIENT_RESOURCES:
                    SWSS_LOG_ERROR("Encountered SAI_STATUS_INSUFFICIENT_RESOURCES in set operation, task failed, SAI API: %s, status: %s",
                                   sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    return task_failed;
                default:
                    SWSS_LOG_ERROR("Encountered failure in set operation, exiting orchagent, SAI API: %s, status: %s",
                            sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        default:
            SWSS_LOG_ERROR("Encountered failure in set operation, exiting orchagent, SAI API: %s, status: %s",
                        sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
            handleSaiFailure(true);
            break;
    }

    return task_need_retry;
}

task_process_status handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis remove
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses (e.g., SAI_STATUS_OBJECT_IN_USE,
     *          SAI_STATUS_ITEM_NOT_FOUND)
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    switch (api)
    {
        case SAI_API_ROUTE:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiRemoveStatus");
                    return task_success;
                case SAI_STATUS_ITEM_NOT_FOUND:
                case SAI_STATUS_NOT_EXECUTED:
                    /* When the same route is learned via multiple sources,
                       there can be a duplicate remove operation. Handle this gracefully */
                    return task_success;
                default:
                    SWSS_LOG_ERROR("Encountered failure in remove operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        case SAI_API_NEIGHBOR:
        case SAI_API_NEXT_HOP:
        case SAI_API_NEXT_HOP_GROUP:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiRemoveStatus");
                    return task_success;
                case SAI_STATUS_ITEM_NOT_FOUND:
                    return task_success;
                default:
                    SWSS_LOG_ERROR("Encountered failure in remove operation, exiting orchagent, SAI API: %s, status: %s",
                                sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
            break;
        default:
            switch (status)
            {
                case SAI_STATUS_SUCCESS:
                    SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiRemoveStatus");
                    return task_success;
                default:
                    SWSS_LOG_ERROR("Encountered failure in remove operation, exiting orchagent, SAI API: %s, status: %s",
                        sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
                    handleSaiFailure(true);
                    break;
            }
    }
    return task_need_retry;
}

task_process_status handleSaiGetStatus(sai_api_t api, sai_status_t status, void *context)
{
    /*
     * This function aims to provide coarse handling of failures in sairedis get
     * operation (i.e., notify users by throwing excepions when failures happen).
     * Return value: task_success - Handled the status successfully. No need to retry this SAI operation.
     *               task_need_retry - Cannot handle the status. Need to retry the SAI operation.
     *               task_failed - Failed to handle the status but another attempt is unlikely to resolve the failure.
     * TODO: 1. Add general handling logic for specific statuses
     *       2. Develop fine-grain failure handling mechanisms and replace this coarse handling
     *          in each orch.
     *       3. Take the type of sai api into consideration.
     */
    switch (status)
    {
        case SAI_STATUS_SUCCESS:
            SWSS_LOG_WARN("SAI_STATUS_SUCCESS is not expected in handleSaiGetStatus");
            return task_success;
        case SAI_STATUS_NOT_IMPLEMENTED:
            SWSS_LOG_ERROR("Encountered failure in get operation due to the function is not implemented, exiting orchagent, SAI API: %s",
                        sai_serialize_api(api).c_str());
            throw std::logic_error("SAI get function not implemented");
        default:
            SWSS_LOG_ERROR("Encountered failure in get operation, SAI API: %s, status: %s",
                        sai_serialize_api(api).c_str(), sai_serialize_status(status).c_str());
    }
    return task_failed;
}

bool parseHandleSaiStatusFailure(task_process_status status)
{
    /*
     * This function parses task process status from SAI failure handling function to whether a retry is needed.
     * Return value: true - no retry is needed.
     *               false - retry is needed.
     */
    switch (status)
    {
        case task_need_retry:
            return false;
        case task_failed:
            return true;
        default:
            SWSS_LOG_WARN("task_process_status %d is not expected in parseHandleSaiStatusFailure", status);
    }
    return true;
}

/* Handling SAI failure. Request redis to invoke SAI failure dump and abort if set*/
void handleSaiFailure(bool abort_on_failure)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD;
    attr.value.s32 =  SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP;
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to take sai failure dump %d", status);
    }
    if (abort_on_failure)
    {
        abort();
    }
}


static inline void initSaiRedisCounterEmptyParameter(sai_s8_list_t &sai_s8_list)
{
    sai_s8_list.list = nullptr;
    sai_s8_list.count = 0;
}

static inline void initSaiRedisCounterEmptyParameter(sai_redis_flex_counter_group_parameter_t &flex_counter_group_param)
{
    initSaiRedisCounterEmptyParameter(flex_counter_group_param.poll_interval);
    initSaiRedisCounterEmptyParameter(flex_counter_group_param.operation);
    initSaiRedisCounterEmptyParameter(flex_counter_group_param.stats_mode);
    initSaiRedisCounterEmptyParameter(flex_counter_group_param.plugin_name);
    initSaiRedisCounterEmptyParameter(flex_counter_group_param.plugins);
}

static inline void initSaiRedisCounterParameterFromString(sai_s8_list_t &sai_s8_list, const std::string &str)
{
    if (str.length() > 0)
    {
        sai_s8_list.list = (int8_t*)const_cast<char *>(str.c_str());
        sai_s8_list.count = (uint32_t)str.length();
    }
    else
    {
        initSaiRedisCounterEmptyParameter(sai_s8_list);
    }
}

static inline void notifySyncdCounterOperation(bool is_gearbox, const sai_attribute_t &attr)
{
    if (sai_switch_api == nullptr)
    {
        // This can happen during destruction of the orchagent daemon.
        SWSS_LOG_ERROR("sai_switch_api is NULL");
        return;
    }

    if (!is_gearbox)
    {
        sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    }
    else
    {
        for (auto gearbox_oid : gGearboxOids)
        {
            sai_switch_api->set_switch_attribute(gearbox_oid, &attr);
        }
    }
}

static inline void operateFlexCounterDbSingleField(std::vector<FieldValueTuple> &fvTuples,
                                            const string &field, const string &value)
{
    if (!field.empty() && !value.empty())
    {
        fvTuples.emplace_back(field, value);
    }
}

static inline void operateFlexCounterGroupDatabase(const string &group,
                                            const string &poll_interval,
                                            const string &stats_mode,
                                            const string &plugin_name,
                                            const string &plugins,
                                            const string &operation,
                                            bool is_gearbox)
{
    std::vector<FieldValueTuple> fvTuples;
    auto &flexCounterGroupTable = is_gearbox ? gGearBoxFlexCounterGroupTable : gFlexCounterGroupTable;

    operateFlexCounterDbSingleField(fvTuples, POLL_INTERVAL_FIELD, poll_interval);
    operateFlexCounterDbSingleField(fvTuples, STATS_MODE_FIELD, stats_mode);
    operateFlexCounterDbSingleField(fvTuples, plugin_name, plugins);
    operateFlexCounterDbSingleField(fvTuples, FLEX_COUNTER_STATUS_FIELD, operation);

    flexCounterGroupTable->set(group, fvTuples);
}
void setFlexCounterGroupParameter(const string &group,
                                  const string &poll_interval,
                                  const string &stats_mode,
                                  const string &plugin_name,
                                  const string &plugins,
                                  const string &operation,
                                  bool is_gearbox)
{
    if (gTraditionalFlexCounter)
    {
        operateFlexCounterGroupDatabase(group, poll_interval, stats_mode, plugin_name, plugins, operation, is_gearbox);
        return;
    }

    sai_attribute_t attr;
    sai_redis_flex_counter_group_parameter_t flex_counter_group_param;

    attr.id = SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER_GROUP;
    attr.value.ptr = &flex_counter_group_param;

    initSaiRedisCounterParameterFromString(flex_counter_group_param.counter_group_name, group);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.poll_interval, poll_interval);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.operation, operation);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.stats_mode, stats_mode);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.plugin_name, plugin_name);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.plugins, plugins);

    notifySyncdCounterOperation(is_gearbox, attr);
}

void setFlexCounterGroupOperation(const string &group,
                                  const string &operation,
                                  bool is_gearbox)
{
    if (gTraditionalFlexCounter)
    {
        operateFlexCounterGroupDatabase(group, "", "", "", "", operation, is_gearbox);
        return;
    }

    sai_attribute_t attr;
    sai_redis_flex_counter_group_parameter_t flex_counter_group_param;

    attr.id = SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER_GROUP;
    attr.value.ptr = &flex_counter_group_param;

    initSaiRedisCounterEmptyParameter(flex_counter_group_param);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.counter_group_name, group);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.operation, operation);

    notifySyncdCounterOperation(is_gearbox, attr);
}

void setFlexCounterGroupPollInterval(const string &group,
                                     const string &poll_interval,
                                     bool is_gearbox)
{
    if (gTraditionalFlexCounter)
    {
        operateFlexCounterGroupDatabase(group, poll_interval, "", "", "", "", is_gearbox);
        return;
    }

    sai_attribute_t attr;
    sai_redis_flex_counter_group_parameter_t flex_counter_group_param;

    attr.id = SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER_GROUP;
    attr.value.ptr = &flex_counter_group_param;

    initSaiRedisCounterEmptyParameter(flex_counter_group_param);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.counter_group_name, group);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.poll_interval, poll_interval);

    notifySyncdCounterOperation(is_gearbox, attr);
}

void setFlexCounterGroupStatsMode(const std::string &group,
                                  const std::string &stats_mode,
                                  bool is_gearbox)
{
    if (gTraditionalFlexCounter)
    {
        operateFlexCounterGroupDatabase(group, "", stats_mode, "", "", "", is_gearbox);
        return;
    }

    sai_attribute_t attr;
    sai_redis_flex_counter_group_parameter_t flex_counter_group_param;

    attr.id = SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER_GROUP;
    attr.value.ptr = &flex_counter_group_param;

    initSaiRedisCounterEmptyParameter(flex_counter_group_param);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.counter_group_name, group);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.stats_mode, stats_mode);

    notifySyncdCounterOperation(is_gearbox, attr);
}

void delFlexCounterGroup(const std::string &group,
                         bool is_gearbox)
{
    if (gTraditionalFlexCounter)
    {
        auto &flexCounterGroupTable = is_gearbox ? gGearBoxFlexCounterGroupTable : gFlexCounterGroupTable;

        if (flexCounterGroupTable != nullptr)
        {
            flexCounterGroupTable->del(group);
        }

        return;
    }

    sai_attribute_t attr;
    sai_redis_flex_counter_group_parameter_t flex_counter_group_param;

    attr.id = SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER_GROUP;
    attr.value.ptr = &flex_counter_group_param;

    initSaiRedisCounterEmptyParameter(flex_counter_group_param);
    initSaiRedisCounterParameterFromString(flex_counter_group_param.counter_group_name, group);

    notifySyncdCounterOperation(is_gearbox, attr);
}

void startFlexCounterPolling(sai_object_id_t switch_oid,
                             const std::string &key,
                             const std::string &counter_ids,
                             const std::string &counter_field_name,
                             const std::string &stats_mode)
{
    if (gTraditionalFlexCounter)
    {
        std::vector<FieldValueTuple> fvTuples;
        auto &flexCounterTable = switch_oid == gSwitchId ? gFlexCounterTable : gGearBoxFlexCounterTable;

        operateFlexCounterDbSingleField(fvTuples, counter_field_name, counter_ids);
        operateFlexCounterDbSingleField(fvTuples, STATS_MODE_FIELD, stats_mode);

        flexCounterTable->set(key, fvTuples);

        return;
    }

    sai_attribute_t attr;
    sai_redis_flex_counter_parameter_t flex_counter_param;

    attr.id = SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER;
    attr.value.ptr = &flex_counter_param;

    initSaiRedisCounterParameterFromString(flex_counter_param.counter_key, key);
    initSaiRedisCounterParameterFromString(flex_counter_param.counter_ids, counter_ids);
    initSaiRedisCounterParameterFromString(flex_counter_param.counter_field_name, counter_field_name);
    initSaiRedisCounterParameterFromString(flex_counter_param.stats_mode, stats_mode);

    sai_switch_api->set_switch_attribute(switch_oid, &attr);
}

void stopFlexCounterPolling(sai_object_id_t switch_oid,
                            const std::string &key)
{
    if (gTraditionalFlexCounter)
    {
        auto &flexCounterTable = switch_oid == gSwitchId ? gFlexCounterTable : gGearBoxFlexCounterTable;

        if (flexCounterTable != nullptr)
        {
            flexCounterTable->del(key);
        }

        return;
    }

    sai_attribute_t attr;
    sai_redis_flex_counter_parameter_t flex_counter_param;

    attr.id = SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER;
    attr.value.ptr = &flex_counter_param;

    initSaiRedisCounterParameterFromString(flex_counter_param.counter_key, key);
    initSaiRedisCounterEmptyParameter(flex_counter_param.counter_ids);
    initSaiRedisCounterEmptyParameter(flex_counter_param.counter_field_name);
    initSaiRedisCounterEmptyParameter(flex_counter_param.stats_mode);

    sai_switch_api->set_switch_attribute(switch_oid, &attr);
}

/*
    Use metadata info of the SAI object to infer all the available stats
    Syncd already has logic to filter out the supported stats
*/
std::vector<sai_stat_id_t> queryAvailableCounterStats(const sai_object_type_t object_type)
{
    std::vector<sai_stat_id_t> stat_list;
    auto info = sai_metadata_get_object_type_info(object_type);

    if (!info)
    {
        SWSS_LOG_ERROR("Metadata info query failed, invalid object: %d", object_type);
        return stat_list;
    }

    SWSS_LOG_NOTICE("SAI object %s supports stat type %s",
            sai_serialize_object_type(object_type).c_str(),
            info->statenum->name);

    auto statenumlist = info->statenum->values;
    auto statnumcount = (uint32_t)info->statenum->valuescount;
    stat_list.reserve(statnumcount);

    for (uint32_t i = 0; i < statnumcount; i++)
    {
        stat_list.push_back(static_cast<sai_stat_id_t>(statenumlist[i]));
    }
    return stat_list;
}