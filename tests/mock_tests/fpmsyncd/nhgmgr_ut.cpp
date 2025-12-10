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
        std::shared_ptr<NHGMgr> m_nhgmgr;
        std::shared_ptr<swss::Table> m_nextHopTable;
        std::shared_ptr<swss::Table> m_picContextTable;

        virtual void SetUp() override
        {
            testing_db::reset();

            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);

            /* Construct dependencies */

            /*  1) RouteSync */
            pipeline = std::make_shared<swss::RedisPipeline>(m_app_db.get());
            m_nhgmgr = std::make_shared<NHGMgr>(pipeline.get(), APP_NEXTHOP_GROUP_TABLE_NAME, APP_PIC_CONTEXT_TABLE_NAME, true);

            /* 2) NEXTHOP_GROUP_TABLE in APP_DB */
            m_nextHopTable = std::make_shared<swss::Table>(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

            /* 3) PIC_CONTEXT_TABLE in APP_DB */
            m_picContextTable = std::make_shared<swss::Table>(m_app_db.get(), APP_PIC_CONTEXT_TABLE_NAME);
        }

        virtual void TearDown() override
        {
        }
    };
}

namespace ut_fpmsyncd
{
    /* Test add and remove a single ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingSingleNexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv4 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 123);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;
        // uint32_t sonic_obj_id = entry->getSonicObjID();
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "192.100.1.1");
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "ifname", ifname), true);
        // ASSERT_EQ(ifname, nhg_obj.ifname);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), true);

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;
        // ASSERT_EQ(m_nextHopTable->get(to_string(sonic_obj_id), fvs), false);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), false);
    }

    /* Test add and remove a single ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingSingleIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv6 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100");
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;
        // uint32_t sonic_obj_id = entry->getSonicObjID();
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "fc00::1");
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonic_obj_id), "ifname", ifname), true);
        // ASSERT_EQ(ifname, nhg_obj.ifname);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), true);

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;
        // ASSERT_EQ(m_nextHopTable->get(to_string(sonic_obj_id), fvs), false);
        //  ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonic_obj_id), false);
    }

    /* Test add and remove a multi ipv4 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingMultiNexthop)
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
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

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
        map<string, string> expectedNexthopofB = { { "192.100.2.1", "131" }, { "192.100.2.2", "212" } };
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
            { "192.100.2.1", "11" },
            { "192.100.2.2", "11" },
            { "192.100.1.1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
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
        uint32_t sonicObjIDA = entryA->getSonicObjID();
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
    TEST_F(FpmSyncdNhgMgr, ReceivingMultiIPv6Nexthop)
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
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

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
        map<string, string> expectedNexthopofB = { { "fc00:2::1", "131" }, { "fc00:2::2", "212" } };
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
            { "fc00:2::1", "11" },
            { "fc00:2::2", "11" },
            { "fc00:1::1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
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
        uint32_t sonicObjIDA = entryA->getSonicObjID();
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
        // uint32_t sonicObjID = entry->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
        std::string nexthop;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "192.100.1.1");
        // inet_pton(AF_INET, "122.0.0.1", &nhg_obj.gate.ipv4);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "122.0.0.1");
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
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
        // uint32_t sonicObjID = entry->getSonicObjID();
        ASSERT_EQ(entry->getNextHopStr(), "fc00::1");
        ASSERT_EQ(entry->getInterfaceNameStr(), nhg_obj.ifname);
        std::string nexthop;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "fc00::1");
        // inet_pton(AF_INET6, "fc00::2", &nhg_obj.gate.ipv6);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj), 0);
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthop), true);
        // ASSERT_EQ(nexthop, "fc00::2");
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjID), true);
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
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB, { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { "192.100.2.1", "131" }, { "192.100.2.2", "212" } };
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
            { "192.100.2.1", "11" },
            { "192.100.2.2", "11" },
            { "192.100.1.1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
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
        uint32_t sonicObjIDA = entryA->getSonicObjID();
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
            { "192.100.2.1", "12" },
            { "192.100.2.2", "12" },
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
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependentsB = { ribIDA };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(nhgFullB, { { ribIDB1, 131 }, { ribIDB2, 212 } },
                                                             { { ribIDB1, 0 }, { ribIDB2, 0 } }, dependsB, dependentsB, ribIDB);
        map<string, string> expectedNexthopofB = { { "fc00:2::1", "131" }, { "fc00:2::2", "212" } };
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
            { "fc00:2::1", "11" },
            { "fc00:2::2", "11" },
            { "fc00:1::1", "12" },
        };

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
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
        uint32_t sonicObjIDA = entryA->getSonicObjID();
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
            { "fc00:2::1", "12" },
            { "fc00:2::2", "12" },
        };
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopofA.find(nexthopResults[i]), expectedNexthopofA.end());
            ASSERT_EQ(weightResults[i], expectedNexthopofA.find(nexthopResults[i])->second);
        }
    }

    /* Test add and remove a single unresolved SRv6 VPN nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingSingleSRv6VPNNexthop)
    {
        /* Create two non-recursive nexthops which will be used as dependency */
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 4;
        uint32_t ribIDB1 = 2;
        uint32_t ribIDB2 = 3;
        string vpnSid = "1::1";
        string segSrcB1 = "fc00:200::1", segSrcB2 = "fc00:200::2";
        string nexthopB1 = "fdee::1", nexthopB2 = "fdff::1";

        NextHopGroupFull nhgObjB1 = createSingleSRv6VPNNextHopNHGFull(vpnSid.c_str(), segSrcB1.c_str(), nexthopB1.c_str(), ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleSRv6VPNNextHopNHGFull(vpnSid.c_str(), segSrcB2.c_str(), nexthopB2.c_str(), ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2), 0);

        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);

        /* Check the creation of the NHG entry, these two nexthop should not create related SonicGatewayNHG */
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        ASSERT_EQ(entryB1->hasSonicObj(), false);
        ASSERT_EQ(entryB2->hasSonicObj(), false);

        ASSERT_EQ(entryB1->getSonicObjType(), swss::SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        ASSERT_EQ(entryB2->getSonicObjType(), swss::SONIC_NHG_OBJ_TYPE_NHG_NORMAL);

        /* Create a recursive SRv6 VPN NHG A -> B1, B2 */
        vector<uint32_t> dependsA = { ribIDB1, ribIDB2 };
        std::vector<fib::nh_grp_full> nhgFullA(2);
        fib::nh_grp_full B1 = {
            .id = nhgObjB1.id,
            .weight = 11,
            .num_direct = 0
        };
        fib::nh_grp_full B2 = {
            .id = nhgObjB2.id,
            .weight = 11,
            .num_direct = 0
        };

        nhgFullA[0] = B1;
        nhgFullA[1] = B2;
        string nexthopA = "b::b";
        NextHopGroupFull nhgObjA = createSingleSRv6VPNNextHopNHGFull(vpnSid.c_str(), "a::a", nexthopA.c_str(), ribIDA);
        nhgObjA.depends.resize(2);
        nhgObjA.depends[0] = ribIDB1;
        nhgObjA.depends[1] = ribIDB2;
        nhgObjA.nh_grp_full_list.resize(2);
        nhgObjA.nh_grp_full_list[0] = B1;
        nhgObjA.nh_grp_full_list[1] = B2;
        map<string, string> expectedNexthopofA = {
            { nexthopB1, "11" },
            { nexthopB2, "11" },
        };
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_NE(entryA, nullptr);
        // uint32_t sonicObjIDA = entryA->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        string nexthops = "";
        string vpnsids = "";
        string weights = "";
        // vector<string> nexthopResults, weightResults;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(nexthopA, entryA->getNextHopStr());

        /* Check the SRv6 NHG Object */
        SonicGateWayNHGEntry *sonicNHGEntry = m_nhgmgr->getSonicNHGByRIBID(ribIDA);
        ASSERT_NE(sonicNHGEntry, nullptr);
        ASSERT_EQ(sonicNHGEntry->getSonicGateWayObjType(), swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY);
        uint32_t sonicGatewayObjIDA = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDA), "vpn_sid", vpnsids), true);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDA), true);
        ASSERT_EQ(nexthops, nexthopA);
        ASSERT_EQ(vpnsids, vpnSid);

        /* Remove the SRv6 NHG A */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), false);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDA), false);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicGatewayObjIDA), fvs), false);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->getSonicNHGByRIBID(ribIDA), nullptr);
    }

    /* Test update SRv6 VPN nexthop with new vpn_sid */
    TEST_F(FpmSyncdNhgMgr, UpdatingSingleSRv6VPNNexthopVpnSid)
    {
        /* Create a NextHopGroupFull object containing single srv6 vpn nexthop */
        std::map<uint32_t, NextHopGroupFull> dependsList;
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 9;
        uint32_t ribIDB1 = 8;
        uint32_t ribIDB2 = 7;
        NextHopGroupFull nhgObjB1 = createSingleSRv6VPNNextHopNHGFull("1::1", "fc00:200::1", "fdee::1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleSRv6VPNNextHopNHGFull("1::1", "fcee:200::1", "fdff::1", ribIDB2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2), 0);

        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();

        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);

        /* Create a recursive SRv6 VPN NHG A -> B1, B2 */
        vector<uint32_t> dependsA = { ribIDB1, ribIDB2 };
        std::vector<fib::nh_grp_full> nhgFullA(2);
        fib::nh_grp_full B1 = {
            .id = nhgObjB1.id,
            .weight = 11,
            .num_direct = 0
        };
        fib::nh_grp_full B2 = {
            .id = nhgObjB2.id,
            .weight = 11,
            .num_direct = 0
        };

        nhgFullA[0] = B1;
        nhgFullA[1] = B2;
        NextHopGroupFull nhgObjA = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDA);
        nhgObjA.depends.resize(2);
        nhgObjA.depends[0] = ribIDB1;
        nhgObjA.depends[1] = ribIDB2;
        nhgObjA.nh_grp_full_list.resize(2);
        nhgObjA.nh_grp_full_list[0] = B1;
        nhgObjA.nh_grp_full_list[1] = B2;
        map<string, string> expectedNexthopofA = {
            { "fdee::1", "11" },
            { "fdff::1", "11" },
        };

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        // uint32_t sonicObjIDA = entryA->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        string nexthops = "";
        string vpnsids = "";
        string weights = "";
        vector<string> nexthopResults, weightResults, vpnSidsResults;
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        // ASSERT_EQ("b::b", nexthops);

        /* Check the SRv6 NHG Object */
        SonicGateWayNHGEntry *sonicNHGEntry = m_nhgmgr->getSonicNHGByRIBID(ribIDA);
        ASSERT_NE(sonicNHGEntry, nullptr);
        uint32_t sonicGatewayObjID = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "1::1");
        ASSERT_EQ(nexthops, "b::b");

        /* Update the NHG object with a new vpn_sid */
        NextHopGroupFull nhgObjAUpdated = createSingleSRv6VPNNextHopNHGFull("2::2", "a::a", "b::b", ribIDA); // Changed vpn_sid from "1::1" to "2::2"
        nhgObjAUpdated.depends.resize(2);
        nhgObjAUpdated.depends[0] = ribIDB1;
        nhgObjAUpdated.depends[1] = ribIDB2;
        nhgObjAUpdated.nh_grp_full_list.resize(2);
        nhgObjAUpdated.nh_grp_full_list[0] = B1;
        nhgObjAUpdated.nh_grp_full_list[1] = B2;

        /* Send the updated object to the NhgMgr Add function (this should update the existing entry) */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjAUpdated), 0);

        /* Check that the vpn_sid field has been updated in the APP_DB */
        sonicNHGEntry = m_nhgmgr->getSonicNHGByRIBID(ribIDA);
        sonicGatewayObjID = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);
        ASSERT_EQ(nexthops, "b::b"); // nexthop should remain the same
        ASSERT_EQ(vpnsids, "2::2");  // vpn_sid should be updated to the new value

        /* Remove the NHG object */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicGatewayObjID), fvs), false);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjID), false);
    }

    /* Test add and remove a multi unresolved SRv6 VPN nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingMultiSRv6VPNNexthop)
    {
        /* Create a NextHopGroupFull object list containing single ipv6 nexthop*/
        std::map<uint32_t, NextHopGroupFull> dependsList;
        vector<string> nexthopResults, weightResults, vpnSidsResults;
        string nexthops = "", vpnsids = "", weights = "";
        map<uint32_t, uint32_t> nexthopList;
        uint32_t ribIDA = 17;
        uint32_t ribIDB = 15;
        uint32_t ribIDB1 = 11;
        uint32_t ribIDB2 = 12;
        uint32_t ribIDC = 16;
        uint32_t ribIDC1 = 13;
        uint32_t ribIDC2 = 14;

        NextHopGroupFull nhgObjB1 = createSingleSRv6VPNNextHopNHGFull("1::1", "fc00:100::1", "fc00:100::1", ribIDB1);
        NextHopGroupFull nhgObjB2 = createSingleSRv6VPNNextHopNHGFull("1::1", "fc00:100::1", "fc00:100::1", ribIDB2);
        NextHopGroupFull nhgObjC1 = createSingleSRv6VPNNextHopNHGFull("2::2", "fc00:200::1", "fc00:200::1", ribIDC1);
        NextHopGroupFull nhgObjC2 = createSingleSRv6VPNNextHopNHGFull("2::2", "fc00:200::1", "fc00:200::1", ribIDC2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC1), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC2), 0);

        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        RIBNHGEntry *entryC1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC1.id);
        RIBNHGEntry *entryC2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC2.id);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        ASSERT_NE(entryC1, nullptr);
        ASSERT_NE(entryC2, nullptr);
        // uint32_t sonicObjIDB1 = entryB1->getSonicObjID();
        // uint32_t sonicObjIDB2 = entryB2->getSonicObjID();
        // uint32_t sonicObjIDC1 = entryC1->getSonicObjID();
        // uint32_t sonicObjIDC2 = entryC2->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB2), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC1), true);
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC2), true);

        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        vector<uint32_t> dependsC = { ribIDC1, ribIDC2 };
        std::vector<fib::nh_grp_full> nhgFullB(2);
        std::vector<fib::nh_grp_full> nhgFullC(2);
        fib::nh_grp_full B1 = {
            .id = nhgObjB1.id,
            .weight = 11,
            .num_direct = 1
        };
        fib::nh_grp_full B2 = {
            .id = nhgObjB2.id,
            .weight = 11,
            .num_direct = 1
        };
        fib::nh_grp_full C1 = {
            .id = nhgObjC1.id,
            .weight = 11,
            .num_direct = 1
        };
        fib::nh_grp_full C2 = {
            .id = nhgObjC2.id,
            .weight = 11,
            .num_direct = 1
        };

        nhgFullB[0] = B1;
        nhgFullB[1] = B2;
        nhgFullC[0] = C1;
        nhgFullC[1] = C2;
        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDB);
        NextHopGroupFull nhgObjC = createSingleSRv6VPNNextHopNHGFull("2::2", "c::c", "e::e", ribIDC);
        nhgObjB.depends.resize(2);
        nhgObjC.depends.resize(2);
        nhgObjB.depends[0] = ribIDB1;
        nhgObjB.depends[1] = ribIDB2;
        nhgObjC.depends[0] = ribIDC1;
        nhgObjC.depends[1] = ribIDC2;
        nhgObjB.nh_grp_full_list.resize(2);
        nhgObjC.nh_grp_full_list.resize(2);
        nhgObjB.nh_grp_full_list[0] = B1;
        nhgObjB.nh_grp_full_list[1] = B2;
        nhgObjC.nh_grp_full_list[0] = C1;
        nhgObjC.nh_grp_full_list[1] = C2;

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB for B */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        // uint32_t sonicObjIDB = entryB->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        nexthops = "";
        vpnsids = "";
        nexthopResults.clear();
        weightResults.clear();
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        // ASSERT_EQ(nexthops, "b::b");

        /* Check the SRv6 VPN nexthop of B */
        SonicGateWayNHGEntry *sonicNHGEntryB = m_nhgmgr->getSonicNHGByRIBID(ribIDB);
        ASSERT_NE(sonicNHGEntryB, nullptr);
        ASSERT_EQ(sonicNHGEntryB->getSonicGateWayObjType(), swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY);
        uint32_t sonicGatewayObjIDB = sonicNHGEntryB->getSonicGateWayObjID();
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDB), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDB), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "1::1");
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");

        /* Check that fpmsyncd created the correct entries in APP_DB for C */
        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC);
        ASSERT_NE(entryC, nullptr);
        // uint32_t sonicObjIDC = entryC->getSonicObjID();
        // ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        nexthops = "";
        nexthopResults.clear();
        weightResults.clear();
        // ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDC), "nexthop", nexthops), true);
        // ASSERT_EQ(nexthops, "e::e");

        /* Check the SRv6 VPN nexthop of C */
        SonicGateWayNHGEntry *sonicNHGEntryC = m_nhgmgr->getSonicNHGByRIBID(ribIDC);
        ASSERT_NE(sonicNHGEntryC, nullptr);
        ASSERT_EQ(sonicNHGEntryC->getSonicGateWayObjType(), swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY);
        uint32_t sonicGatewayObjIDC = sonicNHGEntryC->getSonicGateWayObjID();
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjIDC), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDC), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "2::2");
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjIDC), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "e::e");

        /* Create the NHG A, which depends on B and C */
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDB1, nhgObjB1 },
                                                     { ribIDB2, nhgObjB2 },
                                                     { ribIDC, nhgObjC },
                                                     { ribIDC1, nhgObjC1 },
                                                     { ribIDC2, nhgObjC2 } };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };

        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 12 }, { ribIDB1, 12 }, { ribIDB2, 12 }, { ribIDC, 10 }, { ribIDC1, 10 }, { ribIDC2, 10 } },
                                                             { { ribIDB, 2 }, { ribIDB1, 0 }, { ribIDB2, 0 }, { ribIDC, 2 }, { ribIDC1, 0 }, { ribIDC2, 0 } },
                                                             dependsA, {}, ribIDA);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB for NHG A */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDA = entryA->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        nexthops = "";
        weights = "";
        vpnsids = "";
        map<string, string> expectedNexthopofA = {
            { "b::b", "12" },
            { "e::e", "10" },
        };
        nexthopResults.clear();
        weightResults.clear();
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

        /* Check the SRv6 NHG Object for NHG A */
        map<string, string> expectedVpnSidofA = {
            { "b::b", "1::1" },
            { "e::e", "2::2" },
        };
        SonicGateWayNHGEntry *sonicNHGEntry = m_nhgmgr->getSonicNHGByRIBID(ribIDA);
        uint32_t sonicGatewayObjID = sonicNHGEntry->getSonicGateWayObjID();
        ASSERT_NE(sonicNHGEntry, nullptr);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicGatewayObjID), "vpn_sid", vpnsids), true);
        ASSERT_NE(nexthops, "");
        nexthopResults = splitResults(nexthops, ",");
        vpnSidsResults = splitResults(vpnsids, ",");
        for (size_t i = 0; i < vpnSidsResults.size(); i++)
        {
            ASSERT_NE(expectedVpnSidofA.find(nexthopResults[i]), expectedVpnSidofA.end());
            ASSERT_EQ(vpnSidsResults[i], expectedVpnSidofA.find(nexthopResults[i])->second);
        }

        /* Remove the SRv6 NHG Object */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->getSonicNHGByRIBID(ribIDA), nullptr);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicGatewayObjID), fvs), false);
        ASSERT_EQ(m_nhgmgr->isSonicGatewayNHGIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, sonicGatewayObjID), false);
    }
}