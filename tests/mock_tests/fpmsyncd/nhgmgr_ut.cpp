#include "ut_helpers_fpmsyncd.h"
#include "gtest/gtest.h"
#include <gmock/gmock.h>
#include "mock_table.h"
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "ipaddress.h"
#include "fpmsyncd/nhgmgr.h"
#include <iostream>

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
        NextHopGroupFull nhg_obj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 123);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;
        uint32_t sonic_obj_id = entry->getSoincObjID();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "nexthop", nexthop), true);
        ASSERT_EQ(nexthop, "192.100.1.1");
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "ifname", ifname), true);
        ASSERT_EQ(ifname, nhg_obj.ifname);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), true);

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonic_obj_id), fvs), false);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), false);
    }

    /* Test add and remove a single ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, RecevingSingleIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv6 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100");
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;
        uint32_t sonic_obj_id = entry->getSoincObjID();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "nexthop", nexthop), true);
        ASSERT_EQ(nexthop, "fc00::1");
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "ifname", ifname), true);
        ASSERT_EQ(ifname, nhg_obj.ifname);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), true);

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonic_obj_id), fvs), false);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), false);
    }

    /* Test add and remove a multi ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, RecevingMultiNexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv4 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv4NextHopNHGFull("192.100.2.1", "120.0.2.1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv4NextHopNHGFull("192.100.2.2", "120.0.2.2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        uint32_t sonicObjIDC = entryC->getSoincObjID();
        uint32_t sonicObjIDB1 = entryB1->getSoincObjID();
        uint32_t sonicObjIDB2 = entryB2->getSoincObjID();

        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB,
                                                             { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } },
                                                             dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { to_string(entryB1->getSoincObjID()), "131" }, { to_string(entryB2->getSoincObjID()), "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { to_string(entryB1->getSoincObjID()), "11" },
            { to_string(entryB2->getSoincObjID()), "11" },
            { to_string(entryC->getSoincObjID()), "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSoincObjID();
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonicObjIDA), fvs), false);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), false);
    }

    /* Test add and remove a multi ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, RecevingMultiIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv6 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv6NextHopNHGFull("fc00:1::1", "fc00:100::1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv6NextHopNHGFull("fc00:2::1", "fc00:200::1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv6NextHopNHGFull("fc00:2::2", "fc00:200::2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        uint32_t sonicObjIDC = entryC->getSoincObjID();
        uint32_t sonicObjIDB1 = entryB1->getSoincObjID();
        uint32_t sonicObjIDB2 = entryB2->getSoincObjID();

        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB,
                                                             { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } },
                                                             dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { to_string(entryB1->getSoincObjID()), "131" }, { to_string(entryB2->getSoincObjID()), "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { to_string(entryB1->getSoincObjID()), "11" },
            { to_string(entryB2->getSoincObjID()), "11" },
            { to_string(entryC->getSoincObjID()), "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonicObjIDA), fvs), false);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), false);
    }

    /* Test update a single ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdateingSingleNexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv4 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 1);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        uint32_t sonicObjID = entry->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
        std::string nexthop;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        ASSERT_EQ(nexthop, "192.100.1.1");
        inet_pton(AF_INET, "122.0.0.1", &nhg_obj.gate.ipv4);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        ASSERT_EQ(nexthop, "122.0.0.1");
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
    }

    /* Test update a single ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdateingSingleIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv6 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100", 1);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        uint32_t sonicObjID = entry->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
        std::string nexthop;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        ASSERT_EQ(nexthop, "fc00::1");
        inet_pton(AF_INET6, "fc00::2", &nhg_obj.gate.ipv6);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        ASSERT_EQ(nexthop, "fc00::2");
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
    }

    /* Test update a multi ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdatingMultiNexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv4 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv4NextHopNHGFull("192.100.2.1", "120.0.2.1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv4NextHopNHGFull("192.100.2.2", "120.0.2.2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        uint32_t sonicObjIDC = entryC->getSoincObjID();
        uint32_t sonicObjIDB1 = entryB1->getSoincObjID();
        uint32_t sonicObjIDB2 = entryB2->getSoincObjID();

        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB, { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { to_string(entryB1->getSoincObjID()), "131" }, { to_string(entryB2->getSoincObjID()), "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { to_string(entryB1->getSoincObjID()), "11" },
            { to_string(entryB2->getSoincObjID()), "11" },
            { to_string(entryC->getSoincObjID()), "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        /* Update the NHG A -> B, B1, B2 */

        dependsA = { ribIDB };
        nhgFullA = {
            { ribIDB, nhgObjB },
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjANew = createMultiNextHopNHGFull(nhgFullA, { { ribIDB, 12 }, { ribIDB1, 12 }, { ribIDB2, 12 } },
                                                                { { ribIDB, 2 }, { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjANew), 0);
        RIBNHGEntry *entryANew = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryANew, nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        expectedNexthopofA.clear();
        expectedNexthopofA = {
            { to_string(entryB1->getSoincObjID()), "12" },
            { to_string(entryB2->getSoincObjID()), "12" }
        };
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }
    }

    /* Test update a multi ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdatingMultiIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv6 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB = 5;
        uint32_t ribIDC = 1;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;

        NextHopGroupFull nhgObjC = createSingleIPv6NextHopNHGFull("fc00:1::1", "fc00:100::1", ribIDC);
        NextHopGroupFull nhgObjB1 = createSingleIPv6NextHopNHGFull("fc00:2::1", "fc00:200::1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleIPv6NextHopNHGFull("fc00:2::2", "fc00:200::2", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        uint32_t sonicObjIDC = entryC->getSoincObjID();
        uint32_t sonicObjIDB1 = entryB1->getSoincObjID();
        uint32_t sonicObjIDB2 = entryB2->getSoincObjID();

        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB, { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { to_string(entryB1->getSoincObjID()), "131" }, { to_string(entryB2->getSoincObjID()), "212" } };
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC } };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 11 },
                                                               { ribIDB1, 11 },
                                                               { ribIDB2, 11 },
                                                               { ribIDC, 12 } },
                                                             { { ribIDB, 2 },
                                                               { ribIDB1, 0 },
                                                               { ribIDB2, 0 },
                                                               { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);
        map<string, string> expectedNexthopofA = {
            { to_string(entryB1->getSoincObjID()), "11" },
            { to_string(entryB2->getSoincObjID()), "11" },
            { to_string(entryC->getSoincObjID()), "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        std::string nexthops, weights;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        vector<string> nexthopResults, weightResults;
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofB.find(nexthopResults[i]), expectedNexthopofB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofB.find(nexthopResults[i])->second);
        }

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSoincObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }

        /* Update the NHG A -> B, B1, B2 */

        dependsA = { ribIDB };
        nhgFullA = {
            { ribIDB, nhgObjB },
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjANew = createMultiNextHopNHGFull(nhgFullA, { { ribIDB, 12 }, { ribIDB1, 12 }, { ribIDB2, 12 } },
                                                                { { ribIDB, 2 }, { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsA, {}, ribIDA);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjANew), 0);
        RIBNHGEntry *entryANew = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryANew, nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "weight", weights), true);
        nexthopResults.clear();
        weightResults.clear();
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        expectedNexthopofA.clear();
        expectedNexthopofA = {
            { to_string(entryB1->getSoincObjID()), "12" },
            { to_string(entryB2->getSoincObjID()), "12" }
        };
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }
    }
}