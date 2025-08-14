#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include <gmock/gmock.h>
#include "mock_table.h"
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "ipaddress.h"
#include "fpmsyncd/nhgmgr.h"

#define private public // Need to modify internal cache
#include "fpmlink.h"
#include "routesync.h"
#include "nhgmgr.h"
#undef private

using namespace swss;
using namespace testing;

#define MY_NEXTHOP_GROUP_KEY_DELIMITER ':'

/*
Test Fixture
*/
namespace ut_fpmsyncd
{
    struct FpmSyncdNhgMgr : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::RedisPipeline> pipeline;
        std::shared_ptr<swss::Table> m_srv6MySidTable;
        std::shared_ptr<NHGMgr> m_nhgmgr;
        std::shared_ptr<swss::Table> m_nextHopTable;

        virtual void SetUp() override
        {
            testing_db::reset();

            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);

            /* Construct dependencies */

            /*  1) RouteSync */
            pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_nhgmgr = std::make_shared<NHGMgr>(pipeline.get(), APP_NEXTHOP_GROUP_TABLE_NAME, true);

            /* 2) NEXTHOP_GROUP_TABLE in APP_DB */
            m_nextHopTable = std::make_shared<swss::Table>(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);
        }

        virtual void TearDown() override
        {
        }
    };
}

namespace ut_fpmsyncd
{
    /* Test add and remove a single ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, RecevingSingleNexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv4 nexthop*/
        struct NextHopGroupFull nhg_obj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1");
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        std::string nexthop;
        ASSERT_EQ(m_nextHopTable->hget(to_string(nhg_obj.id), "nexthop", nexthop), true);
        ASSERT_EQ(nexthop, "192.100.1.1");
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(nhg_obj.id), fvs), false);
    }

}
