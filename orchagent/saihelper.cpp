extern "C" {

#include "sai.h"
#include "saistatus.h"
#include "saiextensions.h"
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

extern sai_object_id_t gSwitchId;
extern bool gSairedisRecord;
extern bool gSwssRecord;
extern ofstream gRecordOfs;
extern string gRecordFile;

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
}

void initSaiRedis(const string &record_location, const std::string &record_filename)
{
    /**
     * NOTE: Notice that all Redis attributes here are using SAI_NULL_OBJECT_ID
     * as the switch ID, because those operations don't require actual switch
     * to be performed, and they should be executed before creating switch.
     */

    sai_attribute_t attr;
    sai_status_t status;

    /* set recording dir before enable recording */

    if (gSairedisRecord)
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
    attr.value.booldata = gSairedisRecord;

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to %s SAI Redis recording, rv:%d",
            gSairedisRecord ? "enable" : "disable", status);
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
    if (platform && strstr(platform, MLNX_PLATFORM_SUBSTRING))
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

    if (platform && strstr(platform, MLNX_PLATFORM_SUBSTRING))
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
    char hwinfoIntf[IFNAMSIZ + 1];
    unsigned int hwinfoPhyid;
    int ret;

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

    ret = sscanf(phy->hwinfo.c_str(), "%" STR(IFNAMSIZ) "[^/]/%u", hwinfoIntf, &hwinfoPhyid);
    if (ret != 2) {
        SWSS_LOG_ERROR("BOX: hardware info doesn't match the 'interface_name/phyid' "
                       "format");
        return SAI_STATUS_FAILURE;
    }

    if (hwinfoPhyid > std::numeric_limits<uint16_t>::max()) {
        SWSS_LOG_ERROR("BOX: phyid is bigger than maximum limit");
        return SAI_STATUS_FAILURE;
    }

    strcpy(hwinfo, phy->hwinfo.c_str());

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

    attr.id = SAI_SWITCH_ATTR_FIRMWARE_MAJOR_VERSION;
    status = sai_switch_api->get_switch_attribute(phyOid, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("BOX: Failed to get firmware major version:%d rtn:%d", phy->phy_id, status);
        return status;
    }
    else
    {
        phy->firmware_major_version = string(attr.value.chardata);
    }

    return status;
}
