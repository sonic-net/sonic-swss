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
        wrHelper->reconcile();
        ASSERT_EQ(wrHelper->getState(), WarmStart::RECONCILED);

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
        m_mysidTable->set("changed", {{"action", "end"}});
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
        EXPECT_FALSE(m_mysidTable->get("stale", fields));
        EXPECT_FALSE(m_mysidTable->get("deleted", fields));
        EXPECT_TRUE(m_mysidTable->get("new", fields));
        EXPECT_EQ(testing_db::getProducerSetCount("ROUTE_TABLE"), 0u);
        EXPECT_EQ(testing_db::getProducerSetCount("SRV6_MY_SID_TABLE"), 2u);
        EXPECT_EQ(testing_db::getProducerDelCount("SRV6_MY_SID_TABLE"), 3u);
        EXPECT_EQ(wrHelper->getState(), WarmStart::RECONCILED);
    }
}
