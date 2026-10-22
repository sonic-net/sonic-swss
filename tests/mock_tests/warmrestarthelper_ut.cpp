#include "warmRestartHelper.h"
#include "warm_restart.h"
#include "mock_table.h"
#include "ut_helper.h"

using namespace testing_db;

namespace wrhelper_test
{
    struct WRHelperTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::RedisPipeline> m_pipeline;
        std::shared_ptr<swss::Table> m_routeTable;
        std::shared_ptr<swss::ProducerStateTable> m_routeProducerTable;
        std::shared_ptr<swss::Table> m_mysidTable;
        std::shared_ptr<swss::ProducerStateTable> m_mysidProducerTable;
        std::shared_ptr<swss::WarmStartHelper> wrHelper;

        void SetUp() override
        {
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_routeTable = std::make_shared<swss::Table>(m_app_db.get(), "ROUTE_TABLE");
            m_routeProducerTable = std::make_shared<swss::ProducerStateTable>(m_app_db.get(), "ROUTE_TABLE");
            m_mysidTable = std::make_shared<swss::Table>(m_app_db.get(), "SRV6_MY_SID_TABLE");
            m_mysidProducerTable = std::make_shared<swss::ProducerStateTable>(m_app_db.get(), "SRV6_MY_SID_TABLE");
            wrHelper = std::make_shared<swss::WarmStartHelper>(m_pipeline.get(), m_routeProducerTable.get(), "ROUTE_TABLE", "bgp", "bgp");
            wrHelper->registerTable(m_pipeline.get(), m_mysidProducerTable.get(), "SRV6_MY_SID_TABLE");
            testing_db::reset();
        }

        void TearDown() override {
        }
    };

    TEST_F(WRHelperTest, testReconciliation)
    {
        /* Initialize WR */
        wrHelper->setState(WarmStart::INITIALIZED);
        ASSERT_EQ(wrHelper->getState(), WarmStart::INITIALIZED);

        /* Old-life entries */
        m_routeTable->set("1.0.0.0/24",
                        {
                            {"ifname", "eth1"},
                            {"nexthop", "2.0.0.0"}
                        });
        m_routeTable->set("1.1.0.0/24",
                        {
                            {"ifname", "eth2"},
                            {"nexthop", "2.1.0.0"},
                            {"weight", "1"},
                        });
        m_routeTable->set("1.2.0.0/24",
                        {
                            {"ifname", "eth2"},
                            {"nexthop", "2.2.0.0"},
                            {"weight", "1"},
                            {"random_attrib", "random_val"},
                        });
        wrHelper->runRestoration();
        ASSERT_EQ(wrHelper->getState(), WarmStart::RESTORED);

        /* Insert new life entries */
        wrHelper->insertRefreshMap({
                                    "1.0.0.0/24",
                                    "SET",
                                    {
                                        {"ifname", "eth1"},
                                        {"nexthop", "2.0.0.0"},
                                        {"protocol", "kernel"}
                                    }
                                });
        wrHelper->insertRefreshMap({
                                    "1.1.0.0/24",
                                    "SET",
                                    {
                                        {"ifname", "eth2"},
                                        {"nexthop", "2.1.0.0,2.5.0.0"},
                                        {"weight", "4"},
                                        {"protocol", "kernel"}
                                    }
                                });
        wrHelper->insertRefreshMap({
                                    "1.2.0.0/24",
                                    "SET",
                                    {
                                        {"ifname", "eth2"},
                                        {"nexthop", "2.2.0.0"},
                                        {"weight", "1"},
                                        {"protocol", "kernel"}
                                    }
                                });
        testing_db::resetOperationCounters();
        wrHelper->reconcile();
        ASSERT_EQ(wrHelper->getState(), WarmStart::RECONCILED);
        EXPECT_EQ(testing_db::getProducerDelCount("ROUTE_TABLE"), 3u);
        EXPECT_EQ(testing_db::getProducerSetCount("ROUTE_TABLE"), 3u);

        std::string val;
        ASSERT_TRUE(m_routeTable->hget("1.0.0.0/24", "protocol", val));
        ASSERT_EQ(val, "kernel");

        m_routeTable->hget("1.1.0.0/24", "protocol", val);
        ASSERT_EQ(val, "kernel");

        m_routeTable->hget("1.1.0.0/24", "weight", val);
        ASSERT_EQ(val, "4");

        m_routeTable->hget("1.2.0.0/24", "protocol", val);
        ASSERT_EQ(val, "kernel");
        ASSERT_FALSE(m_routeTable->hget("1.2.0.0/24", "random_attrib", val));
    }

    TEST_F(WRHelperTest, testBatchedSetIgnoresOperationTags)
    {
        const std::string key = "32:16:16:0:fc00:0:1::";
        m_mysidTable->set(key, {{"action", "end.t"}, {"vrf", "VrfOld"}});
        testing_db::resetOperationCounters();

        m_mysidProducerTable->set({
            {key, DEL_COMMAND, {}},
            {key, SET_COMMAND, {{"action", "end"}}},
        });

        std::string value;
        ASSERT_TRUE(m_mysidTable->hget(key, "action", value));
        EXPECT_EQ(value, "end");
        ASSERT_TRUE(m_mysidTable->hget(key, "vrf", value));
        EXPECT_EQ(value, "VrfOld");
        EXPECT_EQ(testing_db::getProducerDelCount("SRV6_MY_SID_TABLE"), 0u);
        EXPECT_EQ(testing_db::getProducerSetCount("SRV6_MY_SID_TABLE"), 2u);
    }

    TEST_F(WRHelperTest, testEmptyRouteTableWithRetainedMySid)
    {
        const std::string key = "32:16:16:0:fc00:0:1::";
        const std::vector<swss::FieldValueTuple> fields = {{"action", "end"}};

        m_mysidTable->set(key, fields);
        wrHelper->setState(WarmStart::INITIALIZED);
        ASSERT_TRUE(wrHelper->runRestoration());
        ASSERT_EQ(wrHelper->getState(), WarmStart::RESTORED);

        wrHelper->insertRefreshMap("SRV6_MY_SID_TABLE", {key, "SET", fields});
        testing_db::resetOperationCounters();
        wrHelper->reconcile();

        EXPECT_EQ(wrHelper->getState(), WarmStart::RECONCILED);
        EXPECT_EQ(testing_db::getProducerSetCount("SRV6_MY_SID_TABLE"), 0u);
        EXPECT_EQ(testing_db::getProducerDelCount("SRV6_MY_SID_TABLE"), 0u);
    }

    TEST_F(WRHelperTest, testSameKeyInRegisteredTablesDoesNotCollide)
    {
        const std::string key = "shared-key";
        const std::vector<swss::FieldValueTuple> routeFields = {{"nexthop", "10.0.0.1"}};
        const std::vector<swss::FieldValueTuple> mysidFields = {{"action", "end"}};

        m_routeTable->set(key, routeFields);
        m_mysidTable->set(key, mysidFields);
        wrHelper->setState(WarmStart::INITIALIZED);
        ASSERT_TRUE(wrHelper->runRestoration());

        wrHelper->insertRefreshMap({key, "SET", routeFields});
        wrHelper->insertRefreshMap("SRV6_MY_SID_TABLE", {key, "SET", mysidFields});
        testing_db::resetOperationCounters();
        wrHelper->reconcile();

        EXPECT_EQ(testing_db::getProducerSetCount("ROUTE_TABLE"), 0u);
        EXPECT_EQ(testing_db::getProducerDelCount("ROUTE_TABLE"), 0u);
        EXPECT_EQ(testing_db::getProducerSetCount("SRV6_MY_SID_TABLE"), 0u);
        EXPECT_EQ(testing_db::getProducerDelCount("SRV6_MY_SID_TABLE"), 0u);
    }

    TEST_F(WRHelperTest, testReconcilesAllEntryOutcomesAcrossTables)
    {
        m_routeTable->set("unchanged", {{"nexthop", "10.0.0.1"}});
        m_mysidTable->set("changed", {{"action", "end.t"}, {"vrf", "VrfOld"}});
        m_mysidTable->set("stale", {{"action", "end"}});
        m_mysidTable->set("deleted", {{"action", "end"}});

        wrHelper->setState(WarmStart::INITIALIZED);
        ASSERT_TRUE(wrHelper->runRestoration());

        wrHelper->insertRefreshMap({"unchanged", "SET", {{"nexthop", "10.0.0.1"}}});
        wrHelper->insertRefreshMap("SRV6_MY_SID_TABLE", {"changed", "SET", {{"action", "end.x"}, {"adj", "2001:db8::1"}}});
        wrHelper->insertRefreshMap("SRV6_MY_SID_TABLE", {"deleted", "DEL", {}});
        wrHelper->insertRefreshMap("SRV6_MY_SID_TABLE", {"new", "SET", {{"action", "end"}}});

        testing_db::resetOperationCounters();
        wrHelper->reconcile();

        std::vector<swss::FieldValueTuple> fields;
        EXPECT_TRUE(m_routeTable->get("unchanged", fields));
        EXPECT_TRUE(m_mysidTable->get("changed", fields));
        std::string value;
        EXPECT_FALSE(m_mysidTable->hget("changed", "vrf", value));
        ASSERT_TRUE(m_mysidTable->hget("changed", "action", value));
        EXPECT_EQ(value, "end.x");
        ASSERT_TRUE(m_mysidTable->hget("changed", "adj", value));
        EXPECT_EQ(value, "2001:db8::1");
        EXPECT_FALSE(m_mysidTable->get("stale", fields));
        EXPECT_FALSE(m_mysidTable->get("deleted", fields));
        EXPECT_TRUE(m_mysidTable->get("new", fields));
        EXPECT_EQ(testing_db::getProducerSetCount("ROUTE_TABLE"), 0u);
        EXPECT_EQ(testing_db::getProducerSetCount("SRV6_MY_SID_TABLE"), 2u);
        EXPECT_EQ(testing_db::getProducerDelCount("SRV6_MY_SID_TABLE"), 3u);
        EXPECT_EQ(wrHelper->getState(), WarmStart::RECONCILED);
    }

    /*
     * reconcile() resolves each key into one of six outcomes. testReconciliation above
     * only exercises "updated"; this covers the other five. The two delete outcomes and
     * the "discarded" one all end with the key absent, so getDelCallCount() is what
     * separates a real delete from a suppressed one.
     */
    TEST_F(WRHelperTest, testReconciliationOutcomes)
    {
        /* getDelCallCount() is keyed by the real db id, so take it from the connector
         * rather than assuming APPL_DB is 0. */
        const int APPL_DB_ID = m_app_db->getDbId();

        wrHelper->setState(WarmStart::INITIALIZED);

        /* Old-life entries */
        m_routeTable->set("10.0.0.0/24",                    /* -> stale deleted */
                        {
                            {"ifname", "eth1"},
                            {"nexthop", "2.0.0.1"}
                        });
        m_routeTable->set("10.1.0.0/24",                    /* -> deleted */
                        {
                            {"ifname", "eth1"},
                            {"nexthop", "2.0.0.2"}
                        });
        m_routeTable->set("10.2.0.0/24",                    /* -> unchanged */
                        {
                            {"ifname", "eth1"},
                            {"nexthop", "2.0.0.3"}
                        });

        ASSERT_TRUE(wrHelper->runRestoration());
        ASSERT_EQ(wrHelper->getState(), WarmStart::RESTORED);

        /*
         * 10.0.0.0/24 is deliberately absent from the refresh map: the routing stack
         * did not re-advertise it, so it must be deleted as stale.
         */

        /* An explicit withdraw for a route that does exist in AppDB */
        wrHelper->insertRefreshMap({
                                    "10.1.0.0/24",
                                    "DEL",
                                    {}
                                });

        /* Field-for-field identical to what was restored -> no AppDB write needed */
        wrHelper->insertRefreshMap({
                                    "10.2.0.0/24",
                                    "SET",
                                    {
                                        {"ifname", "eth1"},
                                        {"nexthop", "2.0.0.3"}
                                    }
                                });

        /* Never seen before -> created */
        wrHelper->insertRefreshMap({
                                    "10.3.0.0/24",
                                    "SET",
                                    {
                                        {"ifname", "eth2"},
                                        {"nexthop", "2.0.0.4"}
                                    }
                                });

        /*
         * A withdraw for a route AppDB never had. Pushing this down would delete an
         * entry that does not exist, so reconcile() must swallow it.
         */
        wrHelper->insertRefreshMap({
                                    "10.4.0.0/24",
                                    "DEL",
                                    {}
                                });

        wrHelper->reconcile();
        ASSERT_EQ(wrHelper->getState(), WarmStart::RECONCILED);

        std::string val;
        std::vector<FieldValueTuple> fvs;

        /* stale deleted: gone, and gone because del() was called */
        ASSERT_FALSE(m_routeTable->get("10.0.0.0/24", fvs));
        ASSERT_EQ(testing_db::getDelCallCount(APPL_DB_ID, "ROUTE_TABLE", "10.0.0.0/24"), 1);

        /* deleted: same, via the explicit DEL op */
        ASSERT_FALSE(m_routeTable->get("10.1.0.0/24", fvs));
        ASSERT_EQ(testing_db::getDelCallCount(APPL_DB_ID, "ROUTE_TABLE", "10.1.0.0/24"), 1);

        /* unchanged: still there, untouched, and never deleted */
        ASSERT_TRUE(m_routeTable->hget("10.2.0.0/24", "nexthop", val));
        ASSERT_EQ(val, "2.0.0.3");
        ASSERT_EQ(testing_db::getDelCallCount(APPL_DB_ID, "ROUTE_TABLE", "10.2.0.0/24"), 0);

        /* created: pushed down with the new values */
        ASSERT_TRUE(m_routeTable->hget("10.3.0.0/24", "nexthop", val));
        ASSERT_EQ(val, "2.0.0.4");
        ASSERT_TRUE(m_routeTable->hget("10.3.0.0/24", "ifname", val));
        ASSERT_EQ(val, "eth2");

        /*
         * discarded: absent like the deleted ones, but reached that way without a
         * del() ever being issued. This is the only assertion that tells the two apart.
         */
        ASSERT_FALSE(m_routeTable->get("10.4.0.0/24", fvs));
        ASSERT_EQ(testing_db::getDelCallCount(APPL_DB_ID, "ROUTE_TABLE", "10.4.0.0/24"), 0);
    }
}
