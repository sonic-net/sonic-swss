#include "ut_helper.h"
#include "flowcounterrouteorch.h"

extern sai_object_id_t gSwitchId;

extern SwitchOrch *gSwitchOrch;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;
extern RouteOrch *gRouteOrch;
extern FlowCounterRouteOrch *gFlowCounterRouteOrch;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern FgNhgOrch *gFgNhgOrch;
extern Srv6Orch  *gSrv6Orch;

extern FdbOrch *gFdbOrch;
extern MirrorOrch *gMirrorOrch;
extern VRFOrch *gVrfOrch;

extern sai_acl_api_t *sai_acl_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_hash_api_t *sai_hash_api;
extern sai_port_api_t *sai_port_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_bridge_api_t *sai_bridge_api;
extern sai_route_api_t *sai_route_api;
extern sai_route_api_t *sai_neighbor_api;
extern sai_mpls_api_t *sai_mpls_api;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern string gMySwitchType;

using namespace saimeta;

namespace aclorch_test
{
    using namespace std;

    struct AclTestBase : public ::testing::Test
    {
        vector<int32_t *> m_s32list_pool;

        virtual ~AclTestBase()
        {
            for (auto p : m_s32list_pool)
            {
                free(p);
            }
        }
    };

    struct AclTest : public AclTestBase
    {

        struct CreateAclResult
        {
            bool ret_val;

            vector<sai_attribute_t> attr_list;
        };

        shared_ptr<swss::DBConnector> m_config_db;

        AclTest()
        {
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
        }

        void SetUp() override
        {
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            ASSERT_EQ(sai_acl_api, nullptr);
            sai_acl_api = new sai_acl_api_t();
        }

        void TearDown() override
        {
            delete gCrmOrch;
            gCrmOrch = nullptr;

            delete sai_acl_api;
            sai_acl_api = nullptr;
        }

        shared_ptr<CreateAclResult> createAclTable(AclTable &acl)
        {
            auto ret = make_shared<CreateAclResult>();

            auto spy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE>(&sai_acl_api->create_acl_table);
            spy->callFake([&](sai_object_id_t *oid, sai_object_id_t, uint32_t attr_count, const sai_attribute_t *attr_list) -> sai_status_t {
                for (uint32_t i = 0; i < attr_count; ++i)
                {
                    ret->attr_list.emplace_back(attr_list[i]);
                }
                return SAI_STATUS_SUCCESS;
            });

            ret->ret_val = acl.create();
            return ret;
        }
    };

    TEST_F(AclTest, Create_L3_Acl_Table)
    {
        AclTable acltable; /* this test shouldn't trigger a call to gAclOrch because it's nullptr */
        AclTableTypeBuilder builder;
        auto l3TableType = builder
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
            .withMatch(make_shared<AclTableRangeMatch>(set<sai_acl_range_type_t>({SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE})))
            .build();
        acltable.type = l3TableType;
        auto res = createAclTable(acltable);

        ASSERT_TRUE(res->ret_val);

        auto v = vector<swss::FieldValueTuple>(
            { { "SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST", "2:SAI_ACL_BIND_POINT_TYPE_PORT,SAI_ACL_BIND_POINT_TYPE_LAG" },
              { "SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_SRC_IP", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_DST_IP", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS", "true" },
              { "SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE", "2:SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE,SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE" },
              { "SAI_ACL_TABLE_ATTR_ACL_STAGE", "SAI_ACL_STAGE_INGRESS" }});
        SaiAttributeList attr_list(SAI_OBJECT_TYPE_ACL_TABLE, v, false);

        ASSERT_TRUE(Check::AttrListEq(SAI_OBJECT_TYPE_ACL_TABLE, res->attr_list, attr_list));
    }

    struct MockAclOrch
    {
        AclOrch *m_aclOrch;
        swss::DBConnector *config_db;

        MockAclOrch(swss::DBConnector *config_db, swss::DBConnector *state_db, SwitchOrch *switchOrch,
                    PortsOrch *portsOrch, MirrorOrch *mirrorOrch, NeighOrch *neighOrch, RouteOrch *routeOrch) :
            config_db(config_db)
        {
            TableConnector confDbAclTable(config_db, CFG_ACL_TABLE_TABLE_NAME);
            TableConnector confDbAclRuleTable(config_db, CFG_ACL_RULE_TABLE_NAME);

            vector<TableConnector> acl_table_connectors = { confDbAclTable, confDbAclRuleTable };

            m_aclOrch = new AclOrch(acl_table_connectors, state_db, switchOrch, portsOrch, mirrorOrch,
                                    neighOrch, routeOrch);
        }

        ~MockAclOrch()
        {
            delete m_aclOrch;
        }

        operator const AclOrch *() const
        {
            return m_aclOrch;
        }

        void doAclTableTypeTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(config_db, CFG_ACL_TABLE_TYPE_TABLE_NAME, 1, 1), m_aclOrch, CFG_ACL_TABLE_TYPE_TABLE_NAME));

            consumer->addToSync(entries);
            static_cast<Orch *>(m_aclOrch)->doTask(*consumer);
        }

        void doAclTableTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(config_db, CFG_ACL_TABLE_TABLE_NAME, 1, 1), m_aclOrch, CFG_ACL_TABLE_TABLE_NAME));

            consumer->addToSync(entries);
            static_cast<Orch *>(m_aclOrch)->doTask(*consumer);
        }

        void doAclRuleTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(config_db, CFG_ACL_RULE_TABLE_NAME, 1, 1), m_aclOrch, CFG_ACL_RULE_TABLE_NAME));

            consumer->addToSync(entries);
            static_cast<Orch *>(m_aclOrch)->doTask(*consumer);
        }

        sai_object_id_t getTableById(const string &table_id)
        {
            return m_aclOrch->getTableById(table_id);
        }

        const AclRule* getAclRule(string tableName, string ruleName)
        {
            return m_aclOrch->getAclRule(tableName, ruleName);
        }

        const AclTable* getTableByOid(sai_object_id_t oid)
        {
            return m_aclOrch->getTableByOid(oid);
        }

        const AclTable* getAclTable(string tableName)
        {
            auto oid = m_aclOrch->getTableById(tableName);
            return getTableByOid(oid);
        }

        const AclTableType* getAclTableType(string tableTypeName)
        {
            return m_aclOrch->getAclTableType(tableTypeName);
        }

        const map<sai_object_id_t, AclTable> &getAclTables() const
        {
            return Portal::AclOrchInternal::getAclTables(m_aclOrch);
        }
    };

    struct AclOrchTest : public AclTest
    {

        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;

        AclOrchTest()
        {
            // FIXME: move out from constructor
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            if(gMySwitchType == "voq")
                m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
        }

        static map<string, string> gProfileMap;
        static map<string, string>::iterator gProfileIter;

        static const char *profile_get_value(
            sai_switch_profile_id_t profile_id,
            const char *variable)
        {
            map<string, string>::const_iterator it = gProfileMap.find(variable);
            if (it == gProfileMap.end())
            {
                return NULL;
            }

            return it->second.c_str();
        }

        static int profile_get_next_value(
            sai_switch_profile_id_t profile_id,
            const char **variable,
            const char **value)
        {
            if (value == NULL)
            {
                gProfileIter = gProfileMap.begin();
                return 0;
            }

            if (variable == NULL)
            {
                return -1;
            }

            if (gProfileIter == gProfileMap.end())
            {
                return -1;
            }

            *variable = gProfileIter->first.c_str();
            *value = gProfileIter->second.c_str();

            gProfileIter++;

            return 0;
        }

        void SetUp() override
        {
            AclTestBase::SetUp();

            // Init switch and create dependencies

            gProfileMap.emplace("SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850");
            gProfileMap.emplace("KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00");

            sai_service_method_table_t test_services = {
                AclOrchTest::profile_get_value,
                AclOrchTest::profile_get_next_value
            };

            auto status = sai_api_initialize(0, (sai_service_method_table_t *)&test_services);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            sai_api_query(SAI_API_SWITCH, (void **)&sai_switch_api);
            sai_api_query(SAI_API_HASH, (void **)&sai_hash_api);
            sai_api_query(SAI_API_BRIDGE, (void **)&sai_bridge_api);
            sai_api_query(SAI_API_PORT, (void **)&sai_port_api);
            sai_api_query(SAI_API_VLAN, (void **)&sai_vlan_api);
            sai_api_query(SAI_API_ROUTE, (void **)&sai_route_api);
            sai_api_query(SAI_API_NEIGHBOR, (void **)&sai_neighbor_api);
            sai_api_query(SAI_API_MPLS, (void **)&sai_mpls_api);
            sai_api_query(SAI_API_ACL, (void **)&sai_acl_api);
            sai_api_query(SAI_API_NEXT_HOP_GROUP, (void **)&sai_next_hop_group_api);

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            // Get the default virtual router ID
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gVirtualRouterId = attr.value.oid;

            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(),  APP_SWITCH_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            // Create dependencies ...

            const int portsorch_base_pri = 40;

            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            static const  vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch);

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());

            ASSERT_EQ(gFgNhgOrch, nullptr);
            const int fgnhgorch_pri = 15;

            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG,                 fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX,          fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER,          fgnhgorch_pri }
            };
            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);

            ASSERT_EQ(gSrv6Orch, nullptr);
            vector<string> srv6_tables = {
                APP_SRV6_SID_LIST_TABLE_NAME,
                APP_SRV6_MY_SID_TABLE_NAME
            };
            gSrv6Orch = new Srv6Orch(m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);

            ASSERT_EQ(gRouteOrch, nullptr);
            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME,        routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME,  routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch, nullptr);

            vector<TableConnector> policer_tables = {
                TableConnector(m_config_db.get(), CFG_POLICER_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_PORT_STORM_CONTROL_TABLE_NAME)
            };
            TableConnector stateDbStorm(m_state_db.get(), "BUM_STORM_CAPABILITY");
            PolicerOrch *policer_orch = new PolicerOrch(policer_tables, gPortsOrch);

            TableConnector stateDbMirrorSession(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
            TableConnector confDbMirrorSession(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);

            ASSERT_EQ(gMirrorOrch, nullptr);
            gMirrorOrch = new MirrorOrch(stateDbMirrorSession, confDbMirrorSession,
                                         gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch, policer_orch);

            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_app_db.get(), APP_PORT_TABLE_NAME, 1, 1), gPortsOrch, APP_PORT_TABLE_NAME));

            consumer->addToSync({ { "PortInitDone", EMPTY_PREFIX, { { "", "" } } } });
            static_cast<Orch *>(gPortsOrch)->doTask(*consumer.get());
        }

        void TearDown() override
        {
            AclTestBase::TearDown();

            delete gSwitchOrch;
            gSwitchOrch = nullptr;
            delete gMirrorOrch;
            gMirrorOrch = nullptr;
            delete gRouteOrch;
            gRouteOrch = nullptr;
            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;
            delete gSrv6Orch;
            gSrv6Orch = nullptr;
            delete gNeighOrch;
            gNeighOrch = nullptr;
            delete gFdbOrch;
            gFdbOrch = nullptr;
            delete gIntfsOrch;
            gIntfsOrch = nullptr;
            delete gVrfOrch;
            gVrfOrch = nullptr;
            delete gCrmOrch;
            gCrmOrch = nullptr;
            delete gPortsOrch;
            gPortsOrch = nullptr;
            delete gFgNhgOrch;
            gFgNhgOrch = nullptr;

            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            sai_api_uninitialize();

            sai_switch_api = nullptr;
            sai_acl_api = nullptr;
            sai_port_api = nullptr;
            sai_vlan_api = nullptr;
            sai_bridge_api = nullptr;
            sai_route_api = nullptr;
            sai_neighbor_api = nullptr;
            sai_mpls_api = nullptr;
        }

        shared_ptr<MockAclOrch> createAclOrch()
        {
            return make_shared<MockAclOrch>(m_config_db.get(), m_state_db.get(), gSwitchOrch, gPortsOrch, gMirrorOrch,
                                            gNeighOrch, gRouteOrch);
        }

        shared_ptr<SaiAttributeList> getAclTableAttributeList(sai_object_type_t objecttype, const AclTable &acl_table)
        {
            vector<swss::FieldValueTuple> fields;

            fields.push_back({ "SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST", "2:SAI_ACL_BIND_POINT_TYPE_PORT,SAI_ACL_BIND_POINT_TYPE_LAG" });
            fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID", "true" });
            fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE", "true" });
            fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT", "true" });
            fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT", "true" });
            fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS", "true" });
            fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE", "2:SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE,SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE" });

            if (acl_table.type.getName() == TABLE_TYPE_L3)
            {
                fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE", "true" });
                fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_SRC_IP", "true" });
                fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_DST_IP", "true" });
                fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL", "true" });
            }
            else if (acl_table.type.getName() == TABLE_TYPE_L3V6)
            {
                fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6", "true" });
                fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6", "true" });
                fields.push_back({ "SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER", "true" });
            }
            else
            {
                // We shouldn't get here. Will continue to add more test cases ...;
            }

            if (ACL_STAGE_INGRESS == acl_table.stage)
            {
                fields.push_back({ "SAI_ACL_TABLE_ATTR_ACL_STAGE", "SAI_ACL_STAGE_INGRESS" });
            }
            else if (ACL_STAGE_EGRESS == acl_table.stage)
            {
                fields.push_back({ "SAI_ACL_TABLE_ATTR_ACL_STAGE", "SAI_ACL_STAGE_EGRESS" });
            }

            return shared_ptr<SaiAttributeList>(new SaiAttributeList(objecttype, fields, false));
        }

        shared_ptr<SaiAttributeList> getAclRuleAttributeList(sai_object_type_t objecttype, const AclRule &acl_rule, sai_object_id_t acl_table_oid, const AclTable &acl_table)
        {
            vector<swss::FieldValueTuple> fields;

            auto table_id = sai_serialize_object_id(acl_table_oid);
            auto counter_id = sai_serialize_object_id(const_cast<AclRule &>(acl_rule).getCounterOid()); // FIXME: getcounterOid() should be const

            fields.push_back({ "SAI_ACL_ENTRY_ATTR_TABLE_ID", table_id });
            fields.push_back({ "SAI_ACL_ENTRY_ATTR_PRIORITY", "0" });
            fields.push_back({ "SAI_ACL_ENTRY_ATTR_ADMIN_STATE", "true" });
            fields.push_back({ "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER", counter_id });

            if (acl_table.type.getName() == TABLE_TYPE_L3)
            {
                fields.push_back({ "SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP", "1.2.3.4&mask:255.255.255.255" });
            }
            if (acl_table.type.getName() == TABLE_TYPE_L3V6)
            {
                fields.push_back({ "SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6", "::1.2.3.4&mask:ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff" });
            }
            else
            {
                // We shouldn't get here. Will continue to add more test cases ...
            }

            return shared_ptr<SaiAttributeList>(new SaiAttributeList(objecttype, fields, false));
        }

        bool validateAclRule(const string acl_rule_sid, const AclRule &acl_rule, sai_object_id_t acl_table_oid, const AclTable &acl_table)
        {
            sai_object_type_t objecttype = SAI_OBJECT_TYPE_ACL_ENTRY;
            auto exp_attrlist_2 = getAclRuleAttributeList(objecttype, acl_rule, acl_table_oid, acl_table);

            auto acl_rule_oid = Portal::AclRuleInternal::getRuleOid(&acl_rule);

            {
                auto &exp_attrlist = *exp_attrlist_2;

                vector<sai_attribute_t> act_attr;

                for (uint32_t i = 0; i < exp_attrlist.get_attr_count(); ++i)
                {
                    const auto attr = exp_attrlist.get_attr_list()[i];
                    auto meta = sai_metadata_get_attr_metadata(objecttype, attr.id);

                    if (meta == nullptr)
                    {
                        return false;
                    }

                    sai_attribute_t new_attr = {};

                    new_attr.id = attr.id;

                    switch (meta->attrvaluetype)
                    {
                        case SAI_ATTR_VALUE_TYPE_INT32_LIST:
                            new_attr.value.s32list.list = (int32_t *)calloc(attr.value.s32list.count, sizeof(int32_t));
                            new_attr.value.s32list.count = attr.value.s32list.count;
                            m_s32list_pool.emplace_back(new_attr.value.s32list.list);
                            break;

                        default:
                            // do nothing
                            ;
                    }

                    act_attr.emplace_back(new_attr);
                }

                auto status = sai_acl_api->get_acl_entry_attribute(acl_rule_oid, (uint32_t)act_attr.size(), act_attr.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    return false;
                }

                auto b_attr_eq = Check::AttrListEq(objecttype, act_attr, exp_attrlist);
                if (!b_attr_eq)
                {
                    return false;
                }
            }

            return true;
        }

        bool validateAclTable(sai_object_id_t acl_table_oid, const AclTable &acl_table, shared_ptr<SaiAttributeList> expAttrList = nullptr)
        {
            const sai_object_type_t objecttype = SAI_OBJECT_TYPE_ACL_TABLE;
            if (!expAttrList)
            {
                expAttrList = getAclTableAttributeList(objecttype, acl_table);

            }

            {
                auto &exp_attrlist = *expAttrList;

                vector<sai_attribute_t> act_attr;

                for (uint32_t i = 0; i < exp_attrlist.get_attr_count(); ++i)
                {
                    const auto attr = exp_attrlist.get_attr_list()[i];
                    auto meta = sai_metadata_get_attr_metadata(objecttype, attr.id);

                    if (meta == nullptr)
                    {
                        return false;
                    }

                    sai_attribute_t new_attr = {};

                    new_attr.id = attr.id;

                    switch (meta->attrvaluetype)
                    {
                        case SAI_ATTR_VALUE_TYPE_INT32_LIST:
                            new_attr.value.s32list.list = (int32_t *)calloc(attr.value.s32list.count, sizeof(int32_t));
                            new_attr.value.s32list.count = attr.value.s32list.count;
                            m_s32list_pool.emplace_back(new_attr.value.s32list.list);
                            break;

                        default:
                            // do nothing
                            ;
                    }

                    act_attr.emplace_back(new_attr);
                }

                auto status = sai_acl_api->get_acl_table_attribute(acl_table_oid, (uint32_t)act_attr.size(), act_attr.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    return false;
                }

                auto b_attr_eq = Check::AttrListEq(objecttype, act_attr, exp_attrlist);
                if (!b_attr_eq)
                {
                    return false;
                }
            }

            for (const auto &sid_acl_rule : acl_table.rules)
            {
                auto b_valid = validateAclRule(sid_acl_rule.first, *sid_acl_rule.second, acl_table_oid, acl_table);
                if (!b_valid)
                {
                    return false;
                }
            }

            return true;
        }

        // Validate that ACL table resource count is consistent with CRM
        bool validateResourceCountWithCrm(const AclOrch *aclOrch, CrmOrch *crmOrch)
        {
            auto const &resourceMap = Portal::CrmOrchInternal::getResourceMap(crmOrch);

            // Verify the ACL tables
            size_t crmAclTableBindingCount = 0;
            for (auto const &kv: resourceMap.at(CrmResourceType::CRM_ACL_TABLE).countersMap)
            {
                crmAclTableBindingCount += kv.second.usedCounter;
            }

            size_t aclorchAclTableBindingCount = 0;
            for (auto const &kv: Portal::AclOrchInternal::getAclTables(aclOrch))
            {
                aclorchAclTableBindingCount += kv.second.type.getBindPointTypes().size();
            }

            if (crmAclTableBindingCount != aclorchAclTableBindingCount)
            {
                ADD_FAILURE() << "ACL table binding count is not consistent between CrmOrch ("
                        << crmAclTableBindingCount << ") and AclOrch ("
                        << aclorchAclTableBindingCount << ")";
                return false;
            }

            // Verify ACL rules and counters

            // For each CRM_ACL_ENTRY and CRM_ACL_COUNTER entry, there should be a corresponding ACL table
            for (auto aclResourceType: {CrmResourceType::CRM_ACL_ENTRY, CrmResourceType::CRM_ACL_COUNTER})
            {
                for (auto const &kv: resourceMap.at(aclResourceType).countersMap)
                {
                    auto aclOid = kv.second.id;

                    const auto &aclTables = Portal::AclOrchInternal::getAclTables(aclOrch);
                    if (aclTables.find(aclOid) == aclTables.end())
                    {
                        ADD_FAILURE() << "Can't find ACL '" << sai_serialize_object_id(aclOid)
                                << "' in AclOrch";
                        return false;
                    }

                    if (kv.second.usedCounter != aclTables.at(aclOid).rules.size())
                    {
                        ADD_FAILURE() << "CRM usedCounter (" << kv.second.usedCounter
                                << ") is not equal rule in ACL ("
                                << aclTables.at(aclOid).rules.size() << ")";
                        return false;
                    }
                }
            }

            // For each ACL table with at least one rule, there should be corresponding entries for CRM_ACL_ENTRY and CRM_ACL_COUNTER
            for (const auto &kv: Portal::AclOrchInternal::getAclTables(aclOrch))
            {
                if (kv.second.rules.size() > 0)
                {
                    auto key = Portal::CrmOrchInternal::getCrmAclTableKey(crmOrch, kv.first);
                    for (auto aclResourceType: {CrmResourceType::CRM_ACL_ENTRY, CrmResourceType::CRM_ACL_COUNTER})
                    {
                        const auto &cntMap = resourceMap.at(aclResourceType).countersMap;
                        if (cntMap.find(key) == cntMap.end())
                        {
                            ADD_FAILURE() << "Can't find ACL (" << sai_serialize_object_id(kv.first)
                                    << ") in "
                                    << (aclResourceType == CrmResourceType::CRM_ACL_ENTRY ? "CRM_ACL_ENTRY" : "CRM_ACL_COUNTER");
                            return false;
                        }
                    }
                }
            }

            return true;
        }

        // leakage check
        bool validateResourceCountWithLowerLayerDb(const AclOrch *aclOrch)
        {
        // TODO: Using the need to include "sai_vs_state.h". That will need the include path from `configure`
        //       Do this later ...
#if WITH_SAI == LIBVS
        // {
        //     auto& aclTableHash = g_switch_state_map.at(gSwitchId)->objectHash.at(SAI_OBJECT_TYPE_ACL_TABLE);
        //
        //     return aclTableHash.size() == Portal::AclOrchInternal::getAclTables(aclOrch).size();
        // }
        //
        // TODO: add rule check
#endif

            return true;
        }

        // validate consistency between aclOrch and mock data (via SAI)
        bool validateLowerLayerDb(const MockAclOrch *orch)
        {
            assert(orch != nullptr);

            if (!validateResourceCountWithCrm(orch->m_aclOrch, gCrmOrch))
            {
                return false;
            }

            if (!validateResourceCountWithLowerLayerDb(orch->m_aclOrch))
            {
                return false;
            }

            const auto &acl_tables = orch->getAclTables();

            for (const auto &id_acl_table : acl_tables)
            {
                if (!validateAclTable(id_acl_table.first, id_acl_table.second))
                {
                    return false;
                }
            }

            return true;
        }

        bool validateAclTableByConfOp(const AclTable &acl_table, const vector<swss::FieldValueTuple> &values)
        {
            for (const auto &fv : values)
            {
                if (fv.first == ACL_TABLE_TYPE)
                {
                    if (acl_table.type.getName() != fv.second)
                    {
                        return false;
                    }
                }
                else if (fv.first == ACL_TABLE_STAGE)
                {
                    if (fv.second == STAGE_INGRESS)
                    {
                        if (acl_table.stage != ACL_STAGE_INGRESS)
                        {
                            return false;
                        }
                    }
                    else if (fv.second == STAGE_EGRESS)
                    {
                        if (acl_table.stage != ACL_STAGE_EGRESS)
                        {
                            return false;
                        }
                    }
                    else
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        bool validateAclRuleAction(const AclRule &acl_rule, const string &attr_name, const string &attr_value)
        {
            const auto &rule_actions = Portal::AclRuleInternal::getActions(&acl_rule);

            if (attr_name == ACTION_PACKET_ACTION)
            {
                auto it = rule_actions.find(SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION);
                if (it == rule_actions.end())
                {
                    return false;
                }

                if (it->second.getSaiAttr().value.aclaction.enable != true)
                {
                    return false;
                }

                if (attr_value == PACKET_ACTION_FORWARD)
                {
                    if (it->second.getSaiAttr().value.aclaction.parameter.s32 != SAI_PACKET_ACTION_FORWARD)
                    {
                        return false;
                    }
                }
                else if (attr_value == PACKET_ACTION_DROP)
                {
                    if (it->second.getSaiAttr().value.aclaction.parameter.s32 != SAI_PACKET_ACTION_DROP)
                    {
                        return false;
                    }
                }
                else if (attr_value == PACKET_ACTION_COPY)
                {
                    if (it->second.getSaiAttr().value.aclaction.parameter.s32 != SAI_PACKET_ACTION_COPY)
                    {
                        return false;
                    }
                }
                else
                {
                    // unknown attr_value
                    return false;
                }
            }
            else
            {
                // unknown attr_name
                return false;
            }

            return true;
        }

        bool validateAclRuleMatch(const AclRule &acl_rule, const string &attr_name, const string &attr_value)
        {
            const auto &rule_matches = Portal::AclRuleInternal::getMatches(&acl_rule);

            if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP)
            {
                auto it_field = rule_matches.find(attr_name == MATCH_SRC_IP ? SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP : SAI_ACL_ENTRY_ATTR_FIELD_DST_IP);
                if (it_field == rule_matches.end())
                {
                    return false;
                }

                char addr[20];
                sai_serialize_ip4(addr, it_field->second.getSaiAttr().value.aclfield.data.ip4);
                if (attr_value != addr)
                {
                    return false;
                }

                char mask[20];
                sai_serialize_ip4(mask, it_field->second.getSaiAttr().value.aclfield.mask.ip4);
                if (string(mask) != "255.255.255.255")
                {
                    return false;
                }
            }
            else if (attr_name == MATCH_SRC_IPV6)
            {
                auto it_field = rule_matches.find(SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6);
                if (it_field == rule_matches.end())
                {
                    return false;
                }

                char addr[46];
                sai_serialize_ip6(addr, it_field->second.getSaiAttr().value.aclfield.data.ip6);
                if (attr_value != addr)
                {
                    return false;
                }

                char mask[46];
                sai_serialize_ip6(mask, it_field->second.getSaiAttr().value.aclfield.mask.ip6);
                if (string(mask) != "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
                {
                    return false;
                }
            }
            else
            {
                // unknown attr_name
                return false;
            }

            return true;
        }

        bool validateAclRuleByConfOp(const AclRule &acl_rule, const vector<swss::FieldValueTuple> &values)
        {
            for (const auto &fv : values)
            {
                auto attr_name = fv.first;
                auto attr_value = fv.second;

                if (attr_name == ACTION_PACKET_ACTION)
                {
                    if (!validateAclRuleAction(acl_rule, attr_name, attr_value))
                    {
                        return false;
                    }
                }
                else if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP || attr_name == MATCH_SRC_IPV6)
                {
                    if (!validateAclRuleMatch(acl_rule, attr_name, attr_value))
                    {
                        return false;
                    }
                }
                else
                {
                    // unknown attr_name
                    return false;
                }
            }
            return true;
        }

        bool validateAclRuleCounter(const AclRule &rule, bool enabled)
        {
            auto ruleOid = Portal::AclRuleInternal::getRuleOid(&rule);

            sai_attribute_t attr;
            attr.id = SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;

            auto status = sai_acl_api->get_acl_entry_attribute(ruleOid, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                return false;
            }

            auto &aclEnable = attr.value.aclaction.enable;
            auto &aclOid = attr.value.aclaction.parameter.oid;

            if (enabled)
            {
                if (aclEnable && aclOid != SAI_NULL_OBJECT_ID)
                {
                    return true;
                }

                return false;
            }

            return !aclEnable && aclOid == SAI_NULL_OBJECT_ID;
        }

        string getAclRuleSaiAttribute(const AclRule& rule, sai_acl_entry_attr_t attrId)
        {
            sai_attribute_t attr{};
            attr.id = attrId;
            auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, attrId);
            if (!meta)
            {
                SWSS_LOG_THROW("SAI BUG: Failed to get attribute metadata for SAI_OBJECT_TYPE_ACL_ENTRY attribute id %d", attrId);
            }

            auto status = sai_acl_api->get_acl_entry_attribute(rule.m_ruleOid, 1, &attr);
            EXPECT_TRUE(status == SAI_STATUS_SUCCESS);

            auto actualSaiValue = sai_serialize_attr_value(*meta, attr);

            return actualSaiValue;
        }

    };

    map<string, string> AclOrchTest::gProfileMap;
    map<string, string>::iterator AclOrchTest::gProfileIter = AclOrchTest::gProfileMap.begin();

    // When received ACL table SET_COMMAND, orchagent can create corresponding ACL.
    // When received ACL table DEL_COMMAND, orchagent can delete corresponding ACL.
    //
    // Input by type = {L3, L3V6, PFCCMD ...}, stage = {INGRESS, EGRESS}.
    //
    // Using fixed ports = {"1,2"} for now.
    // The bind operations will be another separately test cases.
    TEST_F(AclOrchTest, ACL_Creation_and_Destruction)
    {
        auto orch = createAclOrch();

        for (const auto &acl_table_type : { TABLE_TYPE_L3, TABLE_TYPE_L3V6 })
        {
            for (const auto &acl_table_stage : { STAGE_INGRESS, STAGE_EGRESS })
            {
                string acl_table_id = "acl_table_1";

                auto kvfAclTable = deque<KeyOpFieldsValuesTuple>(
                    { { acl_table_id,
                        SET_COMMAND,
                        { { ACL_TABLE_DESCRIPTION, "filter source IP" },
                          { ACL_TABLE_TYPE, acl_table_type },
                          { ACL_TABLE_STAGE, acl_table_stage },
                          { ACL_TABLE_PORTS, "1,2" } } } });
                // FIXME:                  ^^^^^^^^^^^^^ fixed port

                orch->doAclTableTask(kvfAclTable);

                auto oid = orch->getTableById(acl_table_id);
                ASSERT_NE(oid, SAI_NULL_OBJECT_ID);

                const auto &acl_tables = orch->getAclTables();

                auto it = acl_tables.find(oid);
                ASSERT_NE(it, acl_tables.end());

                const auto &acl_table = it->second;

                ASSERT_TRUE(validateAclTableByConfOp(acl_table, kfvFieldsValues(kvfAclTable.front())));
                ASSERT_TRUE(validateLowerLayerDb(orch.get()));

                // delete acl table ...

                kvfAclTable = deque<KeyOpFieldsValuesTuple>(
                    { { acl_table_id,
                        DEL_COMMAND,
                        {} } });

                orch->doAclTableTask(kvfAclTable);

                oid = orch->getTableById(acl_table_id);
                ASSERT_EQ(oid, SAI_NULL_OBJECT_ID);

                ASSERT_TRUE(validateLowerLayerDb(orch.get()));
            }
        }
    }

    // When received ACL rule SET_COMMAND, orchagent can create corresponding ACL rule.
    // When received ACL rule DEL_COMMAND, orchagent can delete corresponding ACL rule.
    //
    // Verify ACL table type = { L3 }, stage = { INGRESS, ENGRESS }
    // Input by matches = { SIP, DIP ...}, pkg:actions = { FORWARD, DROP ... }
    //
    TEST_F(AclOrchTest, L3Acl_Matches_Actions)
    {
        string acl_table_id = "acl_table_1";
        string acl_rule_id = "acl_rule_1";

        auto orch = createAclOrch();

        auto kvfAclTable = deque<KeyOpFieldsValuesTuple>(
            { { acl_table_id,
                SET_COMMAND,
                { { ACL_TABLE_DESCRIPTION, "filter source IP" },
                  { ACL_TABLE_TYPE, TABLE_TYPE_L3 },
                  //            ^^^^^^^^^^^^^ L3 ACL
                  { ACL_TABLE_STAGE, STAGE_INGRESS },
                  // FIXME:      ^^^^^^^^^^^^^ only support / test for ingress ?
                  { ACL_TABLE_PORTS, "1,2" } } } });
        // FIXME:                  ^^^^^^^^^^^^^ fixed port

        orch->doAclTableTask(kvfAclTable);

        // validate acl table ...

        auto acl_table_oid = orch->getTableById(acl_table_id);
        ASSERT_NE(acl_table_oid, SAI_NULL_OBJECT_ID);

        const auto &acl_tables = orch->getAclTables();
        auto it_table = acl_tables.find(acl_table_oid);
        ASSERT_NE(it_table, acl_tables.end());

        const auto &acl_table = it_table->second;

        ASSERT_TRUE(validateAclTableByConfOp(acl_table, kfvFieldsValues(kvfAclTable.front())));
        ASSERT_TRUE(validateLowerLayerDb(orch.get()));

        // add rule ...
        for (const auto &acl_rule_pkg_action : { PACKET_ACTION_FORWARD, PACKET_ACTION_DROP })
        {

            auto kvfAclRule = deque<KeyOpFieldsValuesTuple>({ { acl_table_id + "|" + acl_rule_id,
                                                                SET_COMMAND,
                                                                { { ACTION_PACKET_ACTION, acl_rule_pkg_action },

                                                                  // if (attr_name == ACTION_PACKET_ACTION || attr_name == ACTION_MIRROR_ACTION ||
                                                                  // attr_name == ACTION_DTEL_FLOW_OP || attr_name == ACTION_DTEL_INT_SESSION ||
                                                                  // attr_name == ACTION_DTEL_DROP_REPORT_ENABLE ||
                                                                  // attr_name == ACTION_DTEL_TAIL_DROP_REPORT_ENABLE ||
                                                                  // attr_name == ACTION_DTEL_FLOW_SAMPLE_PERCENT ||
                                                                  // attr_name == ACTION_DTEL_REPORT_ALL_PACKETS)
                                                                  //
                                                                  // TODO: required field (add new test cases for that ....)
                                                                  //

                                                                  { MATCH_SRC_IP, "1.2.3.4" },
                                                                  { MATCH_DST_IP, "4.3.2.1" } } } });

            // TODO: RULE_PRIORITY (important field)
            // TODO: MATCH_DSCP / MATCH_SRC_IPV6 || attr_name == MATCH_DST_IPV6

            orch->doAclRuleTask(kvfAclRule);

            // validate acl rule ...

            auto it_rule = acl_table.rules.find(acl_rule_id);
            ASSERT_NE(it_rule, acl_table.rules.end());

            ASSERT_TRUE(validateAclRuleByConfOp(*it_rule->second, kfvFieldsValues(kvfAclRule.front())));
            ASSERT_TRUE(validateLowerLayerDb(orch.get()));

            // delete acl rule ...

            kvfAclRule = deque<KeyOpFieldsValuesTuple>({ { acl_table_id + "|" + acl_rule_id,
                                                           DEL_COMMAND,
                                                           {} } });

            orch->doAclRuleTask(kvfAclRule);

            // validate acl rule ...

            it_rule = acl_table.rules.find(acl_rule_id);
            ASSERT_EQ(it_rule, acl_table.rules.end());
            ASSERT_TRUE(validateLowerLayerDb(orch.get()));
        }
    }

    // When received ACL rule SET_COMMAND, orchagent can create corresponding ACL rule.
    // When received ACL rule DEL_COMMAND, orchagent can delete corresponding ACL rule.
    //
    // Verify ACL table type = { L3V6 }, stage = { INGRESS, ENGRESS }
    // Input by matches = { SIP, DIP ...}, pkg:actions = { FORWARD, DROP ... }
    //
    TEST_F(AclOrchTest, L3V6Acl_Matches_Actions)
    {
        string acl_table_id = "acl_table_1";
        string acl_rule_id = "acl_rule_1";

        auto orch = createAclOrch();

        auto kvfAclTable = deque<KeyOpFieldsValuesTuple>(
            { { acl_table_id,
                SET_COMMAND,
                { { ACL_TABLE_DESCRIPTION, "filter source IP" },
                  { ACL_TABLE_TYPE, TABLE_TYPE_L3V6 },
                  //            ^^^^^^^^^^^^^ L3V6 ACL
                  { ACL_TABLE_STAGE, STAGE_INGRESS },
                  // FIXME:      ^^^^^^^^^^^^^ only support / test for ingress ?
                  { ACL_TABLE_PORTS, "1,2" } } } });
        // FIXME:                  ^^^^^^^^^^^^^ fixed port

        orch->doAclTableTask(kvfAclTable);

        // validate acl table ...

        auto acl_table_oid = orch->getTableById(acl_table_id);
        ASSERT_NE(acl_table_oid, SAI_NULL_OBJECT_ID);

        const auto &acl_tables = orch->getAclTables();
        auto it_table = acl_tables.find(acl_table_oid);
        ASSERT_NE(it_table, acl_tables.end());

        const auto &acl_table = it_table->second;

        ASSERT_TRUE(validateAclTableByConfOp(acl_table, kfvFieldsValues(kvfAclTable.front())));
        ASSERT_TRUE(validateLowerLayerDb(orch.get()));

        // add rule ...
        for (const auto &acl_rule_pkg_action : { PACKET_ACTION_FORWARD, PACKET_ACTION_DROP })
        {

            auto kvfAclRule = deque<KeyOpFieldsValuesTuple>({ { acl_table_id + "|" + acl_rule_id,
                                                                SET_COMMAND,
                                                                { { ACTION_PACKET_ACTION, acl_rule_pkg_action },

                                                                  // if (attr_name == ACTION_PACKET_ACTION || attr_name == ACTION_MIRROR_ACTION ||
                                                                  // attr_name == ACTION_DTEL_FLOW_OP || attr_name == ACTION_DTEL_INT_SESSION ||
                                                                  // attr_name == ACTION_DTEL_DROP_REPORT_ENABLE ||
                                                                  // attr_name == ACTION_DTEL_TAIL_DROP_REPORT_ENABLE ||
                                                                  // attr_name == ACTION_DTEL_FLOW_SAMPLE_PERCENT ||
                                                                  // attr_name == ACTION_DTEL_REPORT_ALL_PACKETS)
                                                                  //
                                                                  // TODO: required field (add new test cases for that ....)
                                                                  //

                                                                  { MATCH_SRC_IPV6, "::1.2.3.4" },
                                                                  /*{ MATCH_DST_IP, "4.3.2.1" }*/ } } });

            // TODO: RULE_PRIORITY (important field)
            // TODO: MATCH_DSCP / MATCH_SRC_IPV6 || attr_name == MATCH_DST_IPV6

            orch->doAclRuleTask(kvfAclRule);

            // validate acl rule ...

            auto it_rule = acl_table.rules.find(acl_rule_id);
            ASSERT_NE(it_rule, acl_table.rules.end());

            ASSERT_TRUE(validateAclRuleByConfOp(*it_rule->second, kfvFieldsValues(kvfAclRule.front())));
            ASSERT_TRUE(validateLowerLayerDb(orch.get()));

            // delete acl rule ...

            kvfAclRule = deque<KeyOpFieldsValuesTuple>({ { acl_table_id + "|" + acl_rule_id,
                                                           DEL_COMMAND,
                                                           {} } });

            orch->doAclRuleTask(kvfAclRule);

            // validate acl rule ...

            it_rule = acl_table.rules.find(acl_rule_id);
            ASSERT_EQ(it_rule, acl_table.rules.end());
            ASSERT_TRUE(validateLowerLayerDb(orch.get()));
        }
    }

    // When received ACL table/rule SET_COMMAND, orchagent can create corresponding ACL table/rule
    // When received ACL table/rule DEL_COMMAND, orchagent can delete corresponding ACL table/rule
    //
    // Verify ACL rule counter enable/disable
    //
    TEST_F(AclOrchTest, AclRule_Counter_Configuration)
    {
        string tableId = "acl_table_1";
        string ruleId = "acl_rule_1";

        auto orch = createAclOrch();

        // add acl table ...

        auto kvfAclTable = deque<KeyOpFieldsValuesTuple>({{
            tableId,
            SET_COMMAND,
            {
                { ACL_TABLE_DESCRIPTION, "L3 table" },
                { ACL_TABLE_TYPE, TABLE_TYPE_L3 },
                { ACL_TABLE_STAGE, STAGE_INGRESS },
                { ACL_TABLE_PORTS, "1,2" }
            }
        }});

        orch->doAclTableTask(kvfAclTable);

        // validate acl table add ...

        auto tableOid = orch->getTableById(tableId);
        ASSERT_NE(tableOid, SAI_NULL_OBJECT_ID);

        auto tableIt = orch->getAclTables().find(tableOid);
        ASSERT_NE(tableIt, orch->getAclTables().end());

        // add acl rule ...

        auto kvfAclRule = deque<KeyOpFieldsValuesTuple>({{
            tableId + "|" + ruleId,
            SET_COMMAND,
            {
                { ACTION_PACKET_ACTION, PACKET_ACTION_FORWARD },
                { MATCH_SRC_IP, "1.2.3.4" },
                { MATCH_DST_IP, "4.3.2.1" }
            }
        }});

        orch->doAclRuleTask(kvfAclRule);

        // validate acl rule add ...

        auto ruleIt = tableIt->second.rules.find(ruleId);
        ASSERT_NE(ruleIt, tableIt->second.rules.end());

        auto &tableObj = tableIt->second;
        auto &ruleObj = ruleIt->second;

        // validate acl counter disabled ...

        ASSERT_TRUE(ruleObj->disableCounter());
        ASSERT_TRUE(validateAclRuleCounter(*ruleObj, false));

        // validate acl counter enabled ...

        ASSERT_TRUE(ruleObj->enableCounter());
        ASSERT_TRUE(validateAclRuleCounter(*ruleObj, true));

        // delete acl rule ...

        kvfAclRule = deque<KeyOpFieldsValuesTuple>({{
            tableId + "|" + ruleId,
            DEL_COMMAND,
            {}
        }});

        orch->doAclRuleTask(kvfAclRule);

        // validate acl rule delete ...

        ruleIt = tableObj.rules.find(ruleId);
        ASSERT_EQ(ruleIt, tableObj.rules.end());

        // delete acl table ...

        kvfAclTable = deque<KeyOpFieldsValuesTuple>({{
            tableId,
            DEL_COMMAND,
            {}
        }});

        orch->doAclTableTask(kvfAclTable);

        // validate acl table delete ...

        tableIt = orch->getAclTables().find(tableOid);
        ASSERT_EQ(tableIt, orch->getAclTables().end());
    }

    TEST_F(AclOrchTest, AclTableType_Configuration)
    {
        const string aclTableTypeName = "TEST_TYPE";
        const string aclTableName = "TEST_TABLE";
        const string aclRuleName = "TEST_RULE";

        auto orch = createAclOrch();

        auto tableKofvt = deque<KeyOpFieldsValuesTuple>(
            {
                {
                    aclTableName,
                    SET_COMMAND,
                    {
                        { ACL_TABLE_DESCRIPTION, "Test table" },
                        { ACL_TABLE_TYPE, aclTableTypeName},
                        { ACL_TABLE_STAGE, STAGE_INGRESS },
                        { ACL_TABLE_PORTS, "1,2" }
                    }
                }
            }
        );

        orch->doAclTableTask(tableKofvt);

        // Table not created without table type
        ASSERT_FALSE(orch->getAclTable(aclTableName));

        orch->doAclTableTypeTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableTypeName,
                        SET_COMMAND,
                        {
                            {
                                ACL_TABLE_TYPE_MATCHES,
                                string(MATCH_SRC_IP) +  comma + MATCH_ETHER_TYPE + comma + MATCH_L4_SRC_PORT_RANGE + comma + MATCH_BTH_OPCODE + comma + MATCH_AETH_SYNDROME
                            },
                            {
                                ACL_TABLE_TYPE_BPOINT_TYPES,
                                string(BIND_POINT_TYPE_PORT) + comma + BIND_POINT_TYPE_PORTCHANNEL
                            },
                        }
                    }
                }
            )
        );

        orch->doAclTableTask(tableKofvt);

        // Table is created now
        ASSERT_TRUE(orch->getAclTable(aclTableName));

        auto fvs = vector<FieldValueTuple>{
            { "SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST", "2:SAI_ACL_BIND_POINT_TYPE_PORT,SAI_ACL_BIND_POINT_TYPE_LAG" },
            { "SAI_ACL_TABLE_ATTR_FIELD_SRC_IP", "true" },
            { "SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE", "true" },
            { "SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE", "1:SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE" },
            { "SAI_ACL_TABLE_ATTR_FIELD_BTH_OPCODE", "true" },
            { "SAI_ACL_TABLE_ATTR_FIELD_AETH_SYNDROME", "true" },
        };

        ASSERT_TRUE(validateAclTable(
            orch->getAclTable(aclTableName)->getOid(),
            *orch->getAclTable(aclTableName),
            make_shared<SaiAttributeList>(SAI_OBJECT_TYPE_ACL_TABLE, fvs, false))
        );

        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        SET_COMMAND,
                        {
                            { MATCH_SRC_IP, "1.1.1.1/32" },
                            { MATCH_L4_DST_PORT_RANGE, "80..100" },
                            { ACTION_PACKET_ACTION, PACKET_ACTION_DROP },
                        }
                    }
                }
            )
        );

        // L4_DST_PORT_RANGE is not in the table type
        ASSERT_FALSE(orch->getAclRule(aclTableName, aclRuleName));

        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        SET_COMMAND,
                        {
                            { MATCH_SRC_IP, "1.1.1.1/32" },
                            { MATCH_DST_IP, "2.2.2.2/32" },
                            { ACTION_PACKET_ACTION, PACKET_ACTION_DROP },
                        }
                    }
                }
            )
        );

        // DST_IP is not in the table type
        ASSERT_FALSE(orch->getAclRule(aclTableName, aclRuleName));

        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        SET_COMMAND,
                        {
                            { ACTION_PACKET_ACTION, PACKET_ACTION_DROP },
                            { MATCH_BTH_OPCODE, "0x60" },
                        }
                    }
                }
            )
        );

        // MATCH_BTH_OPCODE invalid format
        ASSERT_FALSE(orch->getAclRule(aclTableName, aclRuleName));

        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        SET_COMMAND,
                        {
                            { ACTION_PACKET_ACTION, PACKET_ACTION_DROP },
                            { MATCH_AETH_SYNDROME, "0x60" },
                        }
                    }
                }
            )
        );

        // MATCH_AETH_SYNDROME invalid format
        ASSERT_FALSE(orch->getAclRule(aclTableName, aclRuleName));

        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        SET_COMMAND,
                        {
                            { MATCH_SRC_IP, "1.1.1.1/32" },
                            { ACTION_PACKET_ACTION, PACKET_ACTION_DROP },
                            { MATCH_BTH_OPCODE, "0x60/0xff" },
                            { MATCH_AETH_SYNDROME, "0x60/0x60" },
                        }
                    }
                }
            )
        );

        // Now it is valid for this table.
        ASSERT_TRUE(orch->getAclRule(aclTableName, aclRuleName));

        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        DEL_COMMAND,
                        {}
                    }
                }
            )
        );

        ASSERT_FALSE(orch->getAclRule(aclTableName, aclRuleName));

        orch->doAclTableTypeTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableTypeName,
                        DEL_COMMAND,
                        {}
                    }
                }
            )
        );

        // Table still exists
        ASSERT_TRUE(orch->getAclTable(aclTableName));
        ASSERT_FALSE(orch->getAclTableType(aclTableTypeName));

        orch->doAclTableTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName,
                        DEL_COMMAND,
                        {}
                    }
                }
            )
        );

        // Table is removed
        ASSERT_FALSE(orch->getAclTable(aclTableName));
    }

    TEST_F(AclOrchTest, AclTableType_ActionValidation)
    {
        const string aclTableTypeName = "TEST_TYPE";
        const string aclTableName = "TEST_TABLE";
        const string aclRuleName = "TEST_RULE";

        auto orch = createAclOrch();

        orch->doAclTableTypeTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableTypeName,
                        SET_COMMAND,
                        {
                            {
                                ACL_TABLE_TYPE_MATCHES,
                                string(MATCH_ETHER_TYPE) + comma + MATCH_L4_SRC_PORT_RANGE + comma + MATCH_L4_DST_PORT_RANGE
                            },
                            {
                                ACL_TABLE_TYPE_BPOINT_TYPES,
                                BIND_POINT_TYPE_PORTCHANNEL
                            },
                            {
                                ACL_TABLE_TYPE_ACTIONS,
                                ACTION_MIRROR_INGRESS_ACTION
                            }
                        }
                    }
                }
            )
        );

        orch->doAclTableTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName,
                        SET_COMMAND,
                        {
                            { ACL_TABLE_DESCRIPTION, "Test table" },
                            { ACL_TABLE_TYPE, aclTableTypeName},
                            { ACL_TABLE_STAGE, STAGE_INGRESS },
                            { ACL_TABLE_PORTS, "1,2" }
                        }
                    }
                }
            )
        );

        ASSERT_TRUE(orch->getAclTable(aclTableName));

        auto fvs = vector<FieldValueTuple>{
            { "SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST", "1:SAI_ACL_BIND_POINT_TYPE_LAG" },
            { "SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE", "true" },
            { "SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE", "2:SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE,SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE" },
            { "SAI_ACL_TABLE_ATTR_ACL_ACTION_TYPE_LIST", "1:SAI_ACL_ACTION_TYPE_MIRROR_INGRESS" },
        };

        ASSERT_TRUE(validateAclTable(
            orch->getAclTable(aclTableName)->getOid(),
            *orch->getAclTable(aclTableName),
            make_shared<SaiAttributeList>(SAI_OBJECT_TYPE_ACL_TABLE, fvs, false))
        );

        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        SET_COMMAND,
                        {
                            { MATCH_ETHER_TYPE, "2048" },
                            { ACTION_PACKET_ACTION, PACKET_ACTION_DROP },
                        }
                    }
                }
            )
        );

        // Packet action is not supported on this table
        ASSERT_FALSE(orch->getAclRule(aclTableName, aclRuleName));

        const auto testSessionName = "test_session";
        gMirrorOrch->createEntry(testSessionName, {});
        orch->doAclRuleTask(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        aclTableName + "|" + aclRuleName,
                        SET_COMMAND,
                        {
                            { MATCH_ETHER_TYPE, "2048" },
                            { ACTION_MIRROR_INGRESS_ACTION, testSessionName },
                        }
                    }
                }
            )
        );

        // Mirror action is supported on this table
        ASSERT_TRUE(orch->getAclRule(aclTableName, aclRuleName));
    }

    TEST_F(AclOrchTest, AclRuleUpdate)
    {
        string acl_table_id = "acl_table_1";
        string acl_rule_id = "acl_rule_1";

        auto orch = createAclOrch();

        auto kvfAclTable = deque<KeyOpFieldsValuesTuple>(
            { { acl_table_id,
                SET_COMMAND,
                { { ACL_TABLE_DESCRIPTION, "TEST" },
                  { ACL_TABLE_TYPE, TABLE_TYPE_L3 },
                  { ACL_TABLE_STAGE, STAGE_INGRESS },
                  { ACL_TABLE_PORTS, "1,2" } } } });

        orch->doAclTableTask(kvfAclTable);

        // validate acl table ...

        auto acl_table_oid = orch->getTableById(acl_table_id);
        ASSERT_NE(acl_table_oid, SAI_NULL_OBJECT_ID);

        const auto &acl_tables = orch->getAclTables();
        auto it_table = acl_tables.find(acl_table_oid);
        ASSERT_NE(it_table, acl_tables.end());

        class AclRuleTest : public AclRulePacket
        {
        public:
            AclRuleTest(AclOrch* orch, string rule, string table):
                AclRulePacket(orch, rule, table, true)
            {}

            void setCounterEnabled(bool enabled)
            {
                m_createCounter = enabled;
            }

            void disableMatch(sai_acl_entry_attr_t attr)
            {
                m_matches.erase(attr);
            }
        };

        auto rule = make_shared<AclRuleTest>(orch->m_aclOrch, acl_rule_id, acl_table_id);
        ASSERT_TRUE(rule->validateAddPriority(RULE_PRIORITY, "800"));
        ASSERT_TRUE(rule->validateAddMatch(MATCH_SRC_IP, "1.1.1.1/32"));
        ASSERT_TRUE(rule->validateAddAction(ACTION_PACKET_ACTION, PACKET_ACTION_FORWARD));

        ASSERT_TRUE(orch->m_aclOrch->addAclRule(rule, acl_table_id));
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_PRIORITY), "800");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP), "1.1.1.1&mask:255.255.255.255");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION), "SAI_PACKET_ACTION_FORWARD");

        auto updatedRule = make_shared<AclRuleTest>(*rule);
        ASSERT_TRUE(updatedRule->validateAddPriority(RULE_PRIORITY, "900"));
        ASSERT_TRUE(updatedRule->validateAddMatch(MATCH_SRC_IP, "2.2.2.2/24"));
        ASSERT_TRUE(updatedRule->validateAddMatch(MATCH_DST_IP, "3.3.3.3/24"));
        ASSERT_TRUE(updatedRule->validateAddAction(ACTION_PACKET_ACTION, PACKET_ACTION_DROP));

        ASSERT_TRUE(orch->m_aclOrch->updateAclRule(updatedRule));
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_PRIORITY), "900");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP), "2.2.2.2&mask:255.255.255.0");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_FIELD_DST_IP), "3.3.3.3&mask:255.255.255.0");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION), "SAI_PACKET_ACTION_DROP");

        auto updatedRule2 = make_shared<AclRuleTest>(*updatedRule);
        updatedRule2->setCounterEnabled(false);
        updatedRule2->disableMatch(SAI_ACL_ENTRY_ATTR_FIELD_DST_IP);
        ASSERT_TRUE(orch->m_aclOrch->updateAclRule(updatedRule2));
        ASSERT_TRUE(validateAclRuleCounter(*orch->m_aclOrch->getAclRule(acl_table_id, acl_rule_id), false));
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_PRIORITY), "900");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP), "2.2.2.2&mask:255.255.255.0");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_FIELD_DST_IP), "disabled");
        ASSERT_EQ(getAclRuleSaiAttribute(*rule, SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION), "SAI_PACKET_ACTION_DROP");

        auto updatedRule3 = make_shared<AclRuleTest>(*updatedRule2);
        updatedRule3->setCounterEnabled(true);
        ASSERT_TRUE(orch->m_aclOrch->updateAclRule(updatedRule3));
        ASSERT_TRUE(validateAclRuleCounter(*orch->m_aclOrch->getAclRule(acl_table_id, acl_rule_id), true));

        ASSERT_TRUE(orch->m_aclOrch->removeAclRule(rule->getTableId(), rule->getId()));
    }

    TEST_F(AclOrchTest, deleteNonExistingRule)
    {
        string tableId = "acl_table";
        string ruleId = "acl_rule";

        auto orch = createAclOrch();

        // add acl table
        auto kvfAclTable = deque<KeyOpFieldsValuesTuple>({{
            tableId,
            SET_COMMAND,
            {
                { ACL_TABLE_DESCRIPTION, "L3 table" },
                { ACL_TABLE_TYPE, TABLE_TYPE_L3 },
                { ACL_TABLE_STAGE, STAGE_INGRESS },
                { ACL_TABLE_PORTS, "1,2" }
            }
        }});

        orch->doAclTableTask(kvfAclTable);

        // try to delete non existing acl rule
        ASSERT_TRUE(orch->m_aclOrch->removeAclRule(tableId, ruleId));
    }

    sai_switch_api_t *old_sai_switch_api;

    // The following function is used to override SAI API get_switch_attribute to request passing
    // mandatory ACL actions to SAI when creating mirror ACL table.
    sai_status_t getSwitchAttribute(_In_ sai_object_id_t switch_id,_In_ uint32_t attr_count,
                                    _Inout_ sai_attribute_t *attr_list)
    {
        if (attr_count == 1)
        {
            switch(attr_list[0].id)
            {
            case SAI_SWITCH_ATTR_MAX_ACL_ACTION_COUNT:
                attr_list[0].value.u32 = 2;
                return SAI_STATUS_SUCCESS;
            case SAI_SWITCH_ATTR_ACL_STAGE_INGRESS:
            case SAI_SWITCH_ATTR_ACL_STAGE_EGRESS:
                attr_list[0].value.aclcapability.action_list.count = 2;
                attr_list[0].value.aclcapability.action_list.list[0]= SAI_ACL_ACTION_TYPE_COUNTER;
                attr_list[0].value.aclcapability.action_list.list[1]=
                    attr_list[0].id == SAI_SWITCH_ATTR_ACL_STAGE_INGRESS ?
                        SAI_ACL_ACTION_TYPE_MIRROR_INGRESS : SAI_ACL_ACTION_TYPE_MIRROR_EGRESS;
                attr_list[0].value.aclcapability.is_action_list_mandatory = true;
                return SAI_STATUS_SUCCESS;
            }
        }
        return old_sai_switch_api->get_switch_attribute(switch_id, attr_count, attr_list);
    }

    TEST_F(AclOrchTest, AclTableCreationWithMandatoryActions)
    {
        // Override SAI API get_switch_attribute to request passing mandatory ACL actions to SAI
        // when creating mirror ACL table.
        old_sai_switch_api = sai_switch_api;
        sai_switch_api_t new_sai_switch_api = *sai_switch_api;
        sai_switch_api = &new_sai_switch_api;
        sai_switch_api->get_switch_attribute = getSwitchAttribute;

        // Set platform env to enable support of MIRRORV6 ACL table.
        bool unset_platform_env = false;
        if (!getenv("platform"))
        {
            setenv("platform", VS_PLATFORM_SUBSTRING, 0);
            unset_platform_env = true;
        }

        auto orch = createAclOrch();

        for (const auto &acl_table_type : { TABLE_TYPE_MIRROR, TABLE_TYPE_MIRRORV6, TABLE_TYPE_MIRROR_DSCP })
        {
            for (const auto &acl_table_stage : { STAGE_INGRESS, STAGE_EGRESS })
            {
                // Create ACL table.
                string acl_table_id = "mirror_acl_table";
                auto kvfAclTable = deque<KeyOpFieldsValuesTuple>(
                    { { acl_table_id,
                        SET_COMMAND,
                        { { ACL_TABLE_DESCRIPTION, acl_table_type },
                          { ACL_TABLE_TYPE, acl_table_type },
                          { ACL_TABLE_STAGE, acl_table_stage },
                          { ACL_TABLE_PORTS, "1,2" } } } });
                orch->doAclTableTask(kvfAclTable);
                auto acl_table = orch->getAclTable(acl_table_id);
                ASSERT_NE(acl_table, nullptr);

                // Verify mandaotry ACL actions has been added.
                auto acl_actions = acl_table->type.getActions();
                ASSERT_NE(acl_actions.find(SAI_ACL_ACTION_TYPE_COUNTER), acl_actions.end());
                sai_acl_action_type_t action = strcmp(acl_table_stage, STAGE_INGRESS) == 0 ?
                    SAI_ACL_ACTION_TYPE_MIRROR_INGRESS : SAI_ACL_ACTION_TYPE_MIRROR_EGRESS;
                ASSERT_NE(acl_actions.find(action), acl_actions.end());

                // Delete ACL table.
                kvfAclTable = deque<KeyOpFieldsValuesTuple>(
                    { { acl_table_id,
                        DEL_COMMAND,
                        {} } });
                orch->doAclTableTask(kvfAclTable);
                acl_table = orch->getAclTable(acl_table_id);
                ASSERT_EQ(acl_table, nullptr);
            }
        }

        // Unset platform env.
        if (unset_platform_env)
        {
            unsetenv("platform");
        }

        // Restore sai_switch_api.
        sai_switch_api = old_sai_switch_api;
    }
} // namespace nsAclOrchTest
