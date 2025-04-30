extern "C"
{
#include "sai.h"
}

#include <gmock/gmock.h>

#include <vector>

#include "copporch.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "directory.h"
#include "flowcounterrouteorch.h"
#include "mock_sai_virtual_router.h"
#include "p4orch.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include "switchorch.h"
#include "vrforch.h"
#include "gtest/gtest.h"

using ::testing::StrictMock;

/* Global variables */
sai_object_id_t gVirtualRouterId = SAI_NULL_OBJECT_ID;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
sai_object_id_t gVrfOid = 111;
sai_object_id_t gTrapGroupStartOid = 20;
sai_object_id_t gHostifStartOid = 30;
sai_object_id_t gUserDefinedTrapStartOid = 40;
char *gVrfName = "b4-traffic";
char *gMirrorSession1 = "mirror-session-1";
sai_object_id_t kMirrorSessionOid1 = 9001;
char *gMirrorSession2 = "mirror-session-2";
sai_object_id_t kMirrorSessionOid2 = 9002;
sai_object_id_t gUnderlayIfId;
string gMyAsicName = "";
event_handle_t g_events_handle;

#define DEFAULT_BATCH_SIZE 128
#define DEFAULT_MAX_BULK_SIZE 1000
extern int gBatchSize;
size_t gMaxBulkSize = DEFAULT_MAX_BULK_SIZE;
bool gSyncMode = false;
bool gIsNatSupported = false;
bool gTraditionalFlexCounter = false;
sai_redis_communication_mode_t gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;

PortsOrch *gPortsOrch;
CrmOrch *gCrmOrch;
P4Orch *gP4Orch;
VRFOrch *gVrfOrch;
FlowCounterRouteOrch *gFlowCounterRouteOrch;
SwitchOrch *gSwitchOrch;
Directory<Orch *> gDirectory;
swss::DBConnector *gAppDb;
swss::DBConnector *gStateDb;
swss::DBConnector *gConfigDb;
swss::DBConnector *gCountersDb;
MacAddress gVxlanMacAddress;

sai_router_interface_api_t *sai_router_intfs_api;
sai_neighbor_api_t *sai_neighbor_api;
sai_next_hop_api_t *sai_next_hop_api;
sai_next_hop_group_api_t *sai_next_hop_group_api;
sai_route_api_t *sai_route_api;
sai_acl_api_t *sai_acl_api;
sai_policer_api_t *sai_policer_api;
sai_virtual_router_api_t *sai_virtual_router_api;
sai_hostif_api_t *sai_hostif_api;
sai_hash_api_t *sai_hash_api;
sai_switch_api_t *sai_switch_api;
sai_mirror_api_t *sai_mirror_api;
sai_udf_api_t *sai_udf_api;
sai_tunnel_api_t *sai_tunnel_api;
sai_my_mac_api_t *sai_my_mac_api;
sai_counter_api_t *sai_counter_api;
sai_generic_programmable_api_t *sai_generic_programmable_api;

task_process_status handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context)
{
    return task_success;
}

task_process_status handleSaiSetStatus(sai_api_t api, sai_status_t status, void *context)
{
    return task_success;
}

task_process_status handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context)
{
    return task_success;
}

task_process_status handleSaiGetStatus(sai_api_t api, sai_status_t status, void *context)
{
    return task_success;
}

bool parseHandleSaiStatusFailure(task_process_status status)
{
    return true;
}

namespace
{

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrictMock;

void CreatePort(const std::string port_name, const uint32_t speed, const uint32_t mtu, const sai_object_id_t port_oid,
                Port::Type port_type = Port::PHY, const sai_port_oper_status_t oper_status = SAI_PORT_OPER_STATUS_DOWN,
                const sai_object_id_t vrouter_id = gVirtualRouterId, const bool admin_state_up = true)
{
    Port port(port_name, port_type);
    port.m_speed = speed;
    port.m_mtu = mtu;
    if (port_type == Port::LAG)
    {
        port.m_lag_id = port_oid;
    }
    else
    {
        port.m_port_id = port_oid;
    }
    port.m_vr_id = vrouter_id;
    port.m_admin_state_up = admin_state_up;
    port.m_oper_status = oper_status;

    gPortsOrch->setPort(port_name, port);
}

void SetupPorts()
{
    CreatePort(/*port_name=*/"Ethernet1", /*speed=*/100000,
               /*mtu=*/1500, /*port_oid=*/0x112233);
    CreatePort(/*port_name=*/"Ethernet2", /*speed=*/400000,
               /*mtu=*/4500, /*port_oid=*/0x1fed3);
    CreatePort(/*port_name=*/"Ethernet3", /*speed=*/50000,
               /*mtu=*/9100, /*port_oid=*/0xaabbccdd);
    CreatePort(/*port_name=*/"Ethernet4", /*speed=*/100000,
               /*mtu=*/1500, /*port_oid=*/0x9988);
    CreatePort(/*port_name=*/"Ethernet5", /*speed=*/400000,
               /*mtu=*/4500, /*port_oid=*/0x56789abcdef);
    CreatePort(/*port_name=*/"Ethernet6", /*speed=*/50000,
               /*mtu=*/9100, /*port_oid=*/0x56789abcdff, Port::PHY, SAI_PORT_OPER_STATUS_UP);
    CreatePort(/*port_name=*/"Ethernet7", /*speed=*/100000,
               /*mtu=*/9100, /*port_oid=*/0x1234, /*port_type*/ Port::LAG);
    CreatePort(/*port_name=*/"Ethernet8", /*speed=*/100000,
               /*mtu=*/9100, /*port_oid=*/0x5678, /*port_type*/ Port::MGMT);
    CreatePort(/*port_name=*/"Ethernet9", /*speed=*/50000,
               /*mtu=*/9100, /*port_oid=*/0x56789abcfff, Port::PHY, SAI_PORT_OPER_STATUS_UNKNOWN);
}

void AddVrf()
{
    Table app_vrf_table(gAppDb, APP_VRF_TABLE_NAME);
    std::vector<swss::FieldValueTuple> attributes;
    app_vrf_table.set(gVrfName, attributes);

    StrictMock<MockSaiVirtualRouter> mock_sai_virtual_router_;
    mock_sai_virtual_router = &mock_sai_virtual_router_;
    sai_virtual_router_api->create_virtual_router = create_virtual_router;
    sai_virtual_router_api->remove_virtual_router = remove_virtual_router;
    sai_virtual_router_api->set_virtual_router_attribute = set_virtual_router_attribute;
    EXPECT_CALL(mock_sai_virtual_router_, create_virtual_router(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(gVrfOid), Return(SAI_STATUS_SUCCESS)));
    gVrfOrch->addExistingData(&app_vrf_table);
    static_cast<Orch *>(gVrfOrch)->doTask();
}

} // namespace

int main(int argc, char *argv[])
{
    gBatchSize = DEFAULT_BATCH_SIZE;
    testing::InitGoogleTest(&argc, argv);

    sai_router_interface_api_t router_intfs_api;
    sai_neighbor_api_t neighbor_api;
    sai_next_hop_api_t next_hop_api;
    sai_next_hop_group_api_t next_hop_group_api;
    sai_route_api_t route_api;
    sai_acl_api_t acl_api;
    sai_policer_api_t policer_api;
    sai_virtual_router_api_t virtual_router_api;
    sai_hostif_api_t hostif_api;
    sai_hash_api_t hash_api;
    sai_switch_api_t switch_api;
    sai_mirror_api_t mirror_api;
    sai_udf_api_t udf_api;
    sai_my_mac_api_t my_mac_api;
    sai_tunnel_api_t tunnel_api;
    sai_counter_api_t counter_api;
    sai_generic_programmable_api_t generic_programmable_api;
    sai_router_intfs_api = &router_intfs_api;
    sai_neighbor_api = &neighbor_api;
    sai_next_hop_api = &next_hop_api;
    sai_next_hop_group_api = &next_hop_group_api;
    sai_route_api = &route_api;
    sai_acl_api = &acl_api;
    sai_policer_api = &policer_api;
    sai_virtual_router_api = &virtual_router_api;
    sai_hostif_api = &hostif_api;
    sai_hash_api = &hash_api;
    sai_switch_api = &switch_api;
    sai_mirror_api = &mirror_api;
    sai_udf_api = &udf_api;
    sai_my_mac_api = &my_mac_api;
    sai_tunnel_api = &tunnel_api;
    sai_counter_api = &counter_api;
    sai_generic_programmable_api = &generic_programmable_api;

    swss::DBConnector appl_db("APPL_DB", 0);
    swss::DBConnector state_db("STATE_DB", 0);
    swss::DBConnector config_db("CONFIG_DB", 0);
    swss::DBConnector counters_db("COUNTERS_DB", 0);
    gAppDb = &appl_db;
    gStateDb = &state_db;
    gConfigDb = &config_db;
    gCountersDb = &counters_db;
    std::vector<table_name_with_pri_t> ports_tables;
    PortsOrch ports_orch(gAppDb, gStateDb, ports_tables, gAppDb);
    gPortsOrch = &ports_orch;
    CrmOrch crm_orch(gConfigDb, CFG_CRM_TABLE_NAME);

    gCrmOrch = &crm_orch;
    VRFOrch vrf_orch(gAppDb, APP_VRF_TABLE_NAME, gStateDb, STATE_VRF_OBJECT_TABLE_NAME);
    gVrfOrch = &vrf_orch;
    gDirectory.set(static_cast<VRFOrch *>(&vrf_orch));

    FlowCounterRouteOrch flow_counter_route_orch(gConfigDb, std::vector<std::string>{});
    gFlowCounterRouteOrch = &flow_counter_route_orch;
    gDirectory.set(static_cast<FlowCounterRouteOrch *>(&flow_counter_route_orch));

    // Setup ports for all tests.
    SetupPorts();
    AddVrf();

    return RUN_ALL_TESTS();
}

void setFlexCounterGroupParameter(const std::string &group,
                                  const std::string &poll_interval,
                                  const std::string &stats_mode,
                                  const std::string &plugin_name,
                                  const std::string &plugins,
                                  const std::string &operation,
                                  bool is_gearbox)
{
    return;
}

void setFlexCounterGroupPollInterval(const std::string &group,
                                     const std::string &poll_interval,
                                     bool is_gearbox)
{
    return;
}

void setFlexCounterGroupOperation(const std::string &group,
                                  const std::string &operation,
                                  bool is_gearbox)
{
    return;
}

void setFlexCounterGroupStatsMode(const std::string &group,
                                  const std::string &stats_mode,
                                  bool is_gearbox)
{
    return;
}

void delFlexCounterGroup(const std::string &group,
                         bool is_gearbox)
{
    return;
}

void startFlexCounterPolling(sai_object_id_t switch_oid,
                             const std::string &key,
                             const std::string &counter_ids,
                             const std::string &counter_field_name,
                             const std::string &stats_mode)
{
    return;
}

void stopFlexCounterPolling(sai_object_id_t switch_oid,
                            const std::string &key)
{
    return;
}
