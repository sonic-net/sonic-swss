#include "vlanmgr.h"
#include "gtest/gtest.h"
#include "mock_table.h"

#include <cstring>

extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;

namespace vlanmgr_ut
{
    using namespace swss;
    using namespace std;

    struct VlanMgrTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<VlanMgr> m_vlanMgr;

        VlanMgrTest()
        {
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();
            mockCallArgs.clear();
            /* doVlanTask() bails via isVlanMacOk() until the switch MAC is set. */
            gMacAddress = MacAddress("00:11:22:33:44:aa");
            vector<string> cfg_tables = {
                CFG_VLAN_TABLE_NAME,
                CFG_VLAN_MEMBER_TABLE_NAME,
                CFG_FDB_TABLE_NAME,
            };
            vector<string> state_tables = {};
            m_vlanMgr.reset(new VlanMgr(m_config_db.get(), m_app_db.get(),
                                        m_state_db.get(), cfg_tables, state_tables));
        }

        /* Register a VLAN in m_vlans (bridge/ip commands are faked). */
        void createVlan(const string &vlan)
        {
            Table cfgVlanTable(m_config_db.get(), CFG_VLAN_TABLE_NAME);
            cfgVlanTable.set(vlan, { {"vlanid", vlan.substr(strlen(VLAN_PREFIX))} });
            m_vlanMgr->addExistingData(&cfgVlanTable);
            m_vlanMgr->doTask();
        }

        /* Feed one CONFIG_DB FDB entry through doFdbTask. */
        void setFdb(const string &key, const vector<FieldValueTuple> &fvs)
        {
            Table cfgFdbTable(m_config_db.get(), CFG_FDB_TABLE_NAME);
            cfgFdbTable.set(key, fvs);
            m_vlanMgr->addExistingData(&cfgFdbTable);
            m_vlanMgr->doTask();
        }

        bool appFdbHas(const string &key, vector<FieldValueTuple> &values)
        {
            Table appFdbTable(m_app_db.get(), APP_FDB_TABLE_NAME);
            return appFdbTable.get(key, values);
        }

        static string fvGet(const vector<FieldValueTuple> &fvs, const string &field)
        {
            for (const auto &fv : fvs)
            {
                if (fvField(fv) == field)
                {
                    return fvValue(fv);
                }
            }
            return "";
        }
    };

    /* A key that is not <Vlan>|<mac> is skipped, not crashed. */
    TEST_F(VlanMgrTest, FdbInvalidKeySkipped)
    {
        setFdb("NoDelimiterKey", { {"port", "Ethernet0"} });
        setFdb("Foo|00:11:22:33:44:55", { {"port", "Ethernet0"} });

        vector<FieldValueTuple> values;
        ASSERT_FALSE(appFdbHas("Vlan0:00:11:22:33:44:55", values));
    }

    /* A malformed MAC is caught and skipped, not aborted. */
    TEST_F(VlanMgrTest, FdbMalformedMacSkipped)
    {
        setFdb("Vlan10|NOT_A_MAC", { {"port", "Ethernet0"} });

        vector<FieldValueTuple> values;
        ASSERT_FALSE(appFdbHas("Vlan10:NOT_A_MAC", values));
    }

    /* A static FDB configured before its VLAN is deferred, not applied. */
    TEST_F(VlanMgrTest, FdbDeferredUntilVlanExists)
    {
        setFdb("Vlan99|00:11:22:33:44:66", { {"port", "Ethernet0"} });

        vector<FieldValueTuple> values;
        ASSERT_FALSE(appFdbHas("Vlan99:00:11:22:33:44:66", values));
    }

    /* A valid static FDB (port + type) is programmed to APPL_DB. */
    TEST_F(VlanMgrTest, FdbSetWithType)
    {
        createVlan("Vlan10");

        setFdb("Vlan10|00:11:22:33:44:55",
               { {"port", "Ethernet0"}, {"type", "static"} });

        vector<FieldValueTuple> values;
        ASSERT_TRUE(appFdbHas("Vlan10:00:11:22:33:44:55", values));
        ASSERT_EQ(fvGet(values, "type"), "static");
        ASSERT_EQ(fvGet(values, "port"), "Ethernet0");
    }

    /* A static FDB with no port is skipped. */
    TEST_F(VlanMgrTest, FdbNoPortSkipped)
    {
        createVlan("Vlan10");

        setFdb("Vlan10|00:11:22:33:44:77", { {"type", "static"} });

        vector<FieldValueTuple> values;
        ASSERT_FALSE(appFdbHas("Vlan10:00:11:22:33:44:77", values));
    }
}
