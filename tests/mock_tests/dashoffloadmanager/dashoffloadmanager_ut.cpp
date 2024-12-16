#include "gtest/gtest.h"
#include "mock_table.h"

#include "dpuinfoprovider.h"

#include <unordered_map>

#define private public
#include "dashoffloadmanager.h"
#include "dashoffloadpavalidation.h"
#undef private

#define ACL_OFFLOAD_TABLE_TYPE "DASH_PA_VALIDATION"

using namespace std;
using namespace swss;

namespace dashoffloadmanager_test {
    struct DashOffloadManagerTest : public ::testing::Test
    {
        using VniAddresses = vector<string>;
        using PaOffloadAclConfig = unordered_map<string, VniAddresses>;

        virtual void SetUp() override
        {
            ::testing_db::reset();

            DBConnector db("CONFIG_DB", 0);
            auto bridge_table = Table(&db, CFG_MID_PLANE_BRIDGE_TABLE_NAME);
            auto dpus_table = Table(&db, CFG_DPUS_TABLE_NAME);
            auto dhcp_table = Table(&db, CFG_DHCP_SERVER_IPV4_TABLE_NAME);

            const string bridge = "test-bridge";
            bridge_table.hset("GLOBAL", "bridge", bridge);

            const vector<string> dpus = {"dpu0", "dpu1"};
            for (const auto& dpu : dpus) {
                const string dpu_midplane = dpu + "midplane";
                const string dpu_interface = "{\"" + dpu + "interface\" : \"Ethenet0\"}";
                const string dpu_key = bridge + "|" + dpu_midplane;
                const string dpu_ips = "1.2.3.4";

                dpus_table.hset(dpu, "midplane_interface", dpu + "midplane");
                dpus_table.hset(dpu, "interface", dpu_interface);
                dhcp_table.hset(dpu_key, "ips@", dpu_ips);
            }
        }

        void validateTableValue(Table *table, const string& key, const string& field, const string& exp)
        {
            string value;
            ASSERT_TRUE(table->hget(key, field, value)) << "Get for " << key << " field:" << field;
            ASSERT_EQ(value, exp);
        }

        void addPaValidationEntry(DashPaVlidationOffloadOrch *orch, const string& vni, const vector<string> &ips)
        {
            PaValidationEntry entry { vni, {}};
            for (const auto& addr : ips) {
                entry.addresses.push_back(IpAddress(addr));
            }

            orch->addPaValidationEntry(entry);
        }

        void validatePaAclConfig(DashPaVlidationOffloadOrch *orch, const DpuInfo &info, const PaOffloadAclConfig& config)
        {
            DBConnector db("APPL_DB", 0);
            auto acl_tables = Table(&db, APP_ACL_TABLE_TABLE_NAME);
            auto acl_rules = Table(&db, APP_ACL_RULE_TABLE_NAME);
            auto table_name = orch->m_offloadAclTable;

            ASSERT_FALSE(table_name.empty());

            validateTableValue(&acl_tables, table_name, "type", ACL_OFFLOAD_TABLE_TYPE);
            validateTableValue(&acl_tables, table_name, "stage", "egress");
            validateTableValue(&acl_tables, table_name, "ports", info.interfaces);

            size_t rules_count = 0;

            for (const auto& v : config)
            {
                auto vni = v.first;
                auto addresses = v.second;
                uint32_t rule_idx = 0;

                for (const auto& addr : addresses)
                {
                    auto rule_key = orch->makeAclRuleKey(vni, rule_idx++);
                    auto ip_match = (addr.find("/32") != string::npos) ? "SRC_IP" : "SRC_IPV6";

                    validateTableValue(&acl_rules, rule_key, "priority", "10");
                    validateTableValue(&acl_rules, rule_key, "PACKET_ACTION", "FORWARD");
                    validateTableValue(&acl_rules, rule_key, "TUNNEL_VNI", vni);
                    validateTableValue(&acl_rules, rule_key, ip_match, addr);
                }

                auto drop_rule_key = orch->makeAclDropRuleKey(vni);

                validateTableValue(&acl_rules, drop_rule_key, "priority", "1");
                validateTableValue(&acl_rules, drop_rule_key, "PACKET_ACTION", "DROP");
                validateTableValue(&acl_rules, drop_rule_key, "TUNNEL_VNI", vni);

                rules_count += addresses.size() + 1;
            }

            vector<string> all_rules;
            acl_rules.getKeys(all_rules);
            ASSERT_EQ(all_rules.size(), rules_count);
        }

        void validatePaAclConfigEmpty()
        {
            DBConnector db("APPL_DB", 0);
            auto acl_tables = Table(&db, APP_ACL_TABLE_TABLE_NAME);
            auto acl_rules = Table(&db, APP_ACL_RULE_TABLE_NAME);

            vector<string> tables;
            acl_tables.getKeys(tables);
            ASSERT_TRUE(tables.empty());

            vector<string> rules;
            acl_rules.getKeys(rules);
            ASSERT_TRUE(rules.empty());
        }
    };

    TEST_F(DashOffloadManagerTest, DPUInfo) {
        vector<DpuInfo> info;
        ASSERT_TRUE(getDpuInfo(info));
        ASSERT_EQ(info.size(), 2);

        for (size_t i = 0; i < info.size(); i++) {
            const auto& dpu = info[i];
            ASSERT_EQ(dpu.dpuId, i);
            ASSERT_EQ(dpu.mgmtAddr, "1.2.3.4");
            ASSERT_EQ(dpu.interfaces, "dpu" + to_string(i) + "interface");
        }
    }

    TEST_F(DashOffloadManagerTest, OffloadState) {
        vector<DpuInfo> info;
        ASSERT_TRUE(getDpuInfo(info));

        DashOffloadManager om(info[0], "", "");
        // DPU is down
        ASSERT_FALSE(om.getOffloadState().pa_validation);
        ASSERT_TRUE(om.m_offloadOrchList.empty());

        // DPU is up
        DBConnector db("CHASSIS_STATE_DB", 0);
        auto dpu_state = Table(&db, CHASSIS_STATE_DPU_STATE_TABLE_NAME);
        dpu_state.hset("DPU0", "dpu_control_plane_state", "up");

        SubscriberStateTable dpu_sub(&db, CHASSIS_STATE_DPU_STATE_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0);
        om.processDpuStateTableUpdate(&dpu_sub);
        ASSERT_TRUE(om.getOffloadState().pa_validation);
        ASSERT_EQ(om.m_offloadOrchList.size(), 1);

        // check double event
        dpu_state.hset("DPU0", "dpu_control_plane_state", "up");
        om.processDpuStateTableUpdate(&dpu_sub);
        ASSERT_TRUE(om.getOffloadState().pa_validation);
        ASSERT_EQ(om.m_offloadOrchList.size(), 1);

        // DPU is down
        dpu_state.hset("DPU0", "dpu_control_plane_state", "down");
        om.processDpuStateTableUpdate(&dpu_sub);
        ASSERT_FALSE(om.getOffloadState().pa_validation);
        ASSERT_TRUE(om.m_offloadOrchList.empty());
    }

    void validateAclTableType(const DpuInfo &info)
    {
        DBConnector db("APPL_DB", 0);
        auto acl_table_type = Table(&db, APP_ACL_TABLE_TYPE_TABLE_NAME);

        string value;
        ASSERT_TRUE(acl_table_type.hget(ACL_OFFLOAD_TABLE_TYPE, "MATCHES", value));
        ASSERT_EQ(value, "TUNNEL_VNI,SRC_IP,SRC_IPV6");

        ASSERT_TRUE(acl_table_type.hget(ACL_OFFLOAD_TABLE_TYPE, "ACTIONS", value));
        ASSERT_EQ(value, "PACKET_ACTION");

        ASSERT_TRUE(acl_table_type.hget(ACL_OFFLOAD_TABLE_TYPE, "BIND_POINTS", value));
        ASSERT_EQ(value, "PORT");
    }

    TEST_F(DashOffloadManagerTest, PaValidation)
    {
        DBConnector db("APPL_DB", 0);

        vector<DpuInfo> info;
        ASSERT_TRUE(getDpuInfo(info));

        DashPaVlidationOffloadOrch pa_orch(info[0], &db, nullptr);

        pa_orch.createAclTableType();
        validateAclTableType(info[0]);

        // add rules
        addPaValidationEntry(&pa_orch, "1000", {"10.1.1.1", "10.2.2.2", "ff01::10"});
        addPaValidationEntry(&pa_orch, "2000", {"20.1.1.1", "20.2.2.2", "ff01::20"});

        auto acl_config = PaOffloadAclConfig {
            {"1000", {"10.1.1.1/32", "10.2.2.2/32", "ff01::10/128"}},
            {"2000", {"20.1.1.1/32", "20.2.2.2/32", "ff01::20/128"}}
        };
        validatePaAclConfig(&pa_orch, info[0], acl_config);

        // add more rules
        addPaValidationEntry(&pa_orch, "1000", {"10.2.2.2", "10.3.3.3"}); // 10.2.2.2 already added
        addPaValidationEntry(&pa_orch, "2000", {"ff01::20", "ff02::20"}); // ff01::20 already added

        acl_config = {
            {"1000", {"10.1.1.1/32", "10.2.2.2/32", "ff01::10/128", "10.3.3.3/32"}},
            {"2000", {"20.1.1.1/32", "20.2.2.2/32", "ff01::20/128", "ff02::20/128"}}
        };
        validatePaAclConfig(&pa_orch, info[0], acl_config);

        // remove vni 1000
        pa_orch.removePaValidationEntry("1000");

        acl_config = {
            {"2000", {"20.1.1.1/32", "20.2.2.2/32", "ff01::20/128", "ff02::20/128"}}
        };
        validatePaAclConfig(&pa_orch, info[0], acl_config);

        pa_orch.cleanup();
        validatePaAclConfigEmpty();
    }
}
