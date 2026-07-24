// chassisorch_ut.cpp
//
// Unit tests for ChassisOrch
//

#include <memory>
#include <string>
#include <vector>
#include <sstream>

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "gtest/gtest.h"

extern "C" {
    #include "sai.h"
}

#define private public
#define protected public
#include "chassisorch.h"
#undef protected
#undef private

namespace chassisorch_test
{
    using namespace std;
    using namespace swss;

    class ChassisOrchTest : public ::testing::Test
    {
    protected:
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_config_db;
        unique_ptr<ChassisOrch> m_chassisOrch;

        void SetUp() override
        {
            testing_db::reset();
            m_app_db    = make_shared<DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);

            // Init chassisorch
            vector<string> tables = { "CHASSIS_UT_TABLE" };
            m_chassisOrch.reset(new ChassisOrch(m_config_db.get(),
                                                m_app_db.get(),
                                                tables,
                                                nullptr /* VNetRouteOrch */));
        }

        void TearDown() override
        {
            m_chassisOrch.reset();
            testing_db::reset();
        }

        // Read a single entry out of the APP_PASS_THROUGH_ROUTE_TABLE_NAME
        // that ChassisOrch writes to.
        bool getPassThroughRoute(const string& key, vector<FieldValueTuple>& fvs)
        {
            Table t(m_app_db.get(), APP_PASS_THROUGH_ROUTE_TABLE_NAME);
            fvs.clear();
            return t.get(key, fvs);
        }

        static string getField(const vector<FieldValueTuple>& fvs, const string& field)
        {
            for (const auto& fv : fvs)
            {
                if (fvField(fv) == field) return fvValue(fv);
            }
            return "";
        }

        static VNetNextHopUpdate makeUpdate(const string& op,
                                            const string& vnet,
                                            const string& dst,
                                            const vector<string>& nhIps,
                                            const string& ifname)
        {
            VNetNextHopUpdate u;
            u.op          = op;
            u.vnet        = vnet;
            u.destination = IpAddress(dst);
            u.prefix      = IpPrefix(u.destination.to_string() +
                                     (u.destination.isV4() ? "/32" : "/128"));
            for (const auto& s : nhIps)
            {
                u.nexthop.ips.emplace_back(IpAddress(s));
            }
            u.nexthop.ifname = ifname;
            return u;
        }
    };

    // Single IPv4 nexthop
    TEST_F(ChassisOrchTest, AddRoute_SingleIpv4)
    {
        auto u = makeUpdate(SET_COMMAND, "Vnet1", "10.0.0.1",
                            { "192.168.1.1" }, "Ethernet0");
        m_chassisOrch->addRouteToPassThroughRouteTable(u);

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(getPassThroughRoute("10.0.0.1/32", fvs));
        EXPECT_EQ(getField(fvs, "next_hop_ip"), "192.168.1.1");
        EXPECT_EQ(getField(fvs, "next_vrf_name"), "Vnet1");
        EXPECT_EQ(getField(fvs, "ifname"), "Ethernet0");
        EXPECT_EQ(getField(fvs, "source"), "CHASSIS_ORCH");
        EXPECT_EQ(getField(fvs, "redistribute"), "true");
    }

    // ECMP IPv4: three nexthops must serialize as "a,b,c" with no
    // trailing comma. This is the primary behavior added by the PR.
    TEST_F(ChassisOrchTest, AddRoute_EcmpIpv4)
    {
        auto u = makeUpdate(SET_COMMAND, "Vnet1", "10.0.0.2",
                            { "192.168.1.1", "192.168.1.2", "192.168.1.3" },
                            "Ethernet4");
        m_chassisOrch->addRouteToPassThroughRouteTable(u);

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(getPassThroughRoute("10.0.0.2/32", fvs));
        EXPECT_EQ(getField(fvs, "next_hop_ip"),
                  "192.168.1.1,192.168.1.2,192.168.1.3");
        EXPECT_EQ(getField(fvs, "ifname"), "Ethernet4");
    }

    // ECMP IPv6
    TEST_F(ChassisOrchTest, AddRoute_EcmpIpv6LinkLocal)
    {
        auto u = makeUpdate(SET_COMMAND, "Vnet2", "2001:db8::1",
                            { "fe80::1", "fe80::2" },
                            "Ethernet8,Ethernet12");
        m_chassisOrch->addRouteToPassThroughRouteTable(u);

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(getPassThroughRoute("2001:db8::1/128", fvs));
        EXPECT_EQ(getField(fvs, "next_hop_ip"), "fe80::1,fe80::2");
        EXPECT_EQ(getField(fvs, "ifname"), "Ethernet8,Ethernet12");
    }

    // Negative case - empty nexthop vector must not crash and
    // must yield an empty "next_hop_ip" field (never a stray comma).
    TEST_F(ChassisOrchTest, AddRoute_EmptyNexthops)
    {
        auto u = makeUpdate(SET_COMMAND, "Vnet3", "10.0.0.3",
                            {}, "");
        m_chassisOrch->addRouteToPassThroughRouteTable(u);

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(getPassThroughRoute("10.0.0.3/32", fvs));
        EXPECT_EQ(getField(fvs, "next_hop_ip"), "");
        EXPECT_EQ(getField(fvs, "ifname"), "");
    }

    // Delete path: after a set + explicit delete the row must be gone.
    TEST_F(ChassisOrchTest, DeleteRoute)
    {
        auto u = makeUpdate(SET_COMMAND, "Vnet1", "10.0.0.4",
                            { "192.168.1.1" }, "Ethernet0");
        m_chassisOrch->addRouteToPassThroughRouteTable(u);

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(getPassThroughRoute("10.0.0.4/32", fvs));

        m_chassisOrch->deleteRoutePassThroughRouteTable(u);
        EXPECT_FALSE(getPassThroughRoute("10.0.0.4/32", fvs));
    }

    // Observer::update() with SET_COMMAND should reach addRoute and
    // produce comma separated form for a multi-nexthop update.
    TEST_F(ChassisOrchTest, Update_SetDispatch)
    {
        auto u = makeUpdate(SET_COMMAND, "Vnet1", "10.0.0.5",
                            { "192.168.1.10", "192.168.1.11" },
                            "Ethernet0");
        m_chassisOrch->update(SUBJECT_TYPE_NEXTHOP_CHANGE,
                              static_cast<void*>(&u));

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(getPassThroughRoute("10.0.0.5/32", fvs));
        EXPECT_EQ(getField(fvs, "next_hop_ip"),
                  "192.168.1.10,192.168.1.11");
    }

    // Observer::update() with anything other than SET_COMMAND must
    // result in delete path.
    TEST_F(ChassisOrchTest, Update_DelDispatch)
    {
        auto uAdd = makeUpdate(SET_COMMAND, "Vnet1", "10.0.0.6",
                               { "192.168.1.20" }, "Ethernet0");
        m_chassisOrch->addRouteToPassThroughRouteTable(uAdd);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(getPassThroughRoute("10.0.0.6/32", fvs));

        auto uDel = uAdd;
        uDel.op = DEL_COMMAND;
        m_chassisOrch->update(SUBJECT_TYPE_NEXTHOP_CHANGE,
                              static_cast<void*>(&uDel));

        EXPECT_FALSE(getPassThroughRoute("10.0.0.6/32", fvs));
    }
}
