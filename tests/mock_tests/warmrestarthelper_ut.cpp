#include "warmrestart/warmRestartHelper.h"
#include "warm_restart.h"
#include "tests/mock_tests/mock_table.h"
#include "tests/mock_tests/ut_helper.h"

using namespace testing_db;

namespace wrhelper_test
{
    struct WRHelperTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::RedisPipeline> m_pipeline;
        std::shared_ptr<swss::Table> m_routeTable;
        std::shared_ptr<swss::ProducerStateTable> m_routeProducerTable;
        std::shared_ptr<swss::WarmStartHelper> wrHelper;

        void SetUp() override
        {
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_routeTable = std::make_shared<swss::Table>(m_app_db.get(), "ROUTE_TABLE");
            m_routeProducerTable = std::make_shared<swss::ProducerStateTable>(m_app_db.get(), "ROUTE_TABLE");
            wrHelper = std::make_shared<swss::WarmStartHelper>(m_pipeline.get(), m_routeProducerTable.get(), "ROUTE_TABLE", "bgp", "bgp");
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
    }
}
