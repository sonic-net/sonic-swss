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
#include <cstring>
#include <cstdlib>
#include <new>

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

        /* Accessors - friend is not inherited by TEST_F derived classes,
         * so we expose private members via fixture methods. */
        swss::RIBNHGTable* getRibNhgTable()
        {
            return m_nhgmgr->m_rib_nhg_table;
        }

        swss::SonicPICContentTable* getSonicNhgTable()
        {
            return m_nhgmgr->m_sonic_nhg_table;
        }

        swss::SonicIDMgr& getSonicIdManager()
        {
            return m_nhgmgr->m_sonic_id_manager;
        }

        std::map<swss::SonicNHGObjectKey, swss::SonicNHGObjectInfo>& getCreatedSharedNhgMap()
        {
            return m_nhgmgr->m_rib_nhg_table->m_created_shared_nhg_map;
        }

        std::vector<swss::FieldValueTuple>& getRibEntryFvVector(swss::RIBNHGEntry *entry)
        {
            return entry->m_fvVector;
        }

        std::vector<swss::FieldValueTuple>& getPicEntryFvVector(swss::SonicPICContentEntry *entry)
        {
            return entry->m_fvVector;
        }

        int callUpdateSonicPICObject(swss::RIBNHGEntry *entry, uint32_t previousSonicObjID)
        {
            return m_nhgmgr->updateSonicPICObject(entry, previousSonicObjID);
        }

        void callDumpNHGGroupFull(swss::NextHopGroupFull nhg)
        {
            m_nhgmgr->dumpNHGGroupFull(nhg);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;
    }

    /* Test add and remove a single ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, ReceivingSingleIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv6 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100");
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET6), 0);

        /* Get entry and Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        std::string nexthop, ifname;

        /* Delete entry and check the APP_DB */
        ASSERT_EQ(m_nhgmgr->delNHGFull(nhg_obj.id), 0);
        entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_EQ(entry, nullptr);
        std::vector<FieldValueTuple> fvs;

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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);

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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET6), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);

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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);

        std::string nexthop;

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET), 0);

    }

    /* Test update a single ipv6 nexthop */
    TEST_F(FpmSyncdNhgMgr, UpdateingSingleIPv6Nexthop)
    {
        /* Create a NextHopGroupFull object containing single ipv6 nexthop*/
        NextHopGroupFull nhg_obj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100", 1);
        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET6), 0);
        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(nhg_obj.id);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->getNextHopStr(), "fc00::1");
        ASSERT_EQ(entry->getInterfaceNameStr(), nhg_obj.ifname);
        std::string nexthop;
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhg_obj, AF_INET6), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);

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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjANew, AF_INET), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET6), 0);

        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjC.id);
        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB1.id);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(nhgObjB2.id);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);

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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);
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
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjANew, AF_INET6), 0);
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

        /* Create a recursive SRv6 VPN NHG A -> B1, B2 */
        vector<uint32_t> dependsA = { };
        std::vector<fib::nh_grp_full> nhgFullA(0);
        string nexthopA = "b::b";
        string vpnSid = "1::1";
        NextHopGroupFull nhgObjA = createSingleSRv6VPNNextHopNHGFull(vpnSid.c_str(), "a::a", nexthopA.c_str(), ribIDA);
        nhgObjA.depends.resize(0);
        nhgObjA.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB */
        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_NE(entryA, nullptr);

        string nexthops = "";
        string vpnsids = "";
        string weights = "";

        ASSERT_EQ(nexthopA, entryA->getNextHopStr());

        /* Check the SRv6 NHG Object */
        SonicPICContentEntry *sonicNHGEntry = m_nhgmgr->getSonicPICByRIBID(ribIDA);
        ASSERT_NE(sonicNHGEntry, nullptr);
        ASSERT_EQ(sonicNHGEntry->getType(), swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);
        uint32_t sonicPICobjIDA = sonicNHGEntry->getSonicPicContentObjId();
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjIDA), "vpn_sid", vpnsids), true);
        ASSERT_EQ(m_nhgmgr->isSonicPICIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, sonicPICobjIDA), true);
        ASSERT_EQ(nexthops, nexthopA);
        ASSERT_EQ(vpnsids, vpnSid);

        /* Remove the SRv6 NHG A */
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->isSonicPICIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, sonicPICobjIDA), false);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicPICobjIDA), fvs), false);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribIDA), nullptr);
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
        uint32_t ribIDC = 16;

        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDB);
        NextHopGroupFull nhgObjC = createSingleSRv6VPNNextHopNHGFull("2::2", "c::c", "e::e", ribIDC);
        nhgObjB.depends.resize(0);
        nhgObjC.depends.resize(0);
        nhgObjB.nh_grp_full_list.resize(0);
        nhgObjC.nh_grp_full_list.resize(0);

        /* Send the object to the NhgMgr Add function */
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);

        /* Check that fpmsyncd created the correct entries in APP_DB for B */
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        nexthops = "";
        vpnsids = "";
        nexthopResults.clear();
        weightResults.clear();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");

        /* Check the SRv6 VPN nexthop of B */
        SonicPICContentEntry *sonicNHGEntryB = m_nhgmgr->getSonicPICByRIBID(ribIDB);
        ASSERT_NE(sonicNHGEntryB, nullptr);
        ASSERT_EQ(sonicNHGEntryB->getType(), swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);
        uint32_t sonicPICobjIDB = sonicNHGEntryB->getSonicPicContentObjId();
        ASSERT_EQ(m_nhgmgr->isSonicPICIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, sonicPICobjIDB), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjIDB), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "1::1");
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");

        /* Check that fpmsyncd created the correct entries in APP_DB for C */
        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC);
        ASSERT_NE(entryC, nullptr);
        uint32_t sonicObjIDC = entryC->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);
        nexthops = "";
        nexthopResults.clear();
        weightResults.clear();
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDC), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "e::e");

        /* Check the SRv6 VPN nexthop of C */
        SonicPICContentEntry *sonicNHGEntryC = m_nhgmgr->getSonicPICByRIBID(ribIDC);
        ASSERT_NE(sonicNHGEntryC, nullptr);
        ASSERT_EQ(sonicNHGEntryC->getType(), swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);
        uint32_t sonicPICobjIDC = sonicNHGEntryC->getSonicPicContentObjId();
        ASSERT_EQ(m_nhgmgr->isSonicPICIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, sonicPICobjIDC), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjIDC), "vpn_sid", vpnsids), true);
        ASSERT_EQ(vpnsids, "2::2");
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjIDC), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "e::e");

        /* Create the NHG A, which depends on B and C */
        map<uint32_t, NextHopGroupFull> nhgFullA = { { ribIDB, nhgObjB },
                                                     { ribIDC, nhgObjC }};

        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(nhgFullA,
                                                             { { ribIDB, 12 },{ ribIDC, 10 }},
                                                             { { ribIDB, 0 }, { ribIDC, 0 } },
                                                             dependsA, {}, ribIDA);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

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
        SonicPICContentEntry *sonicNHGEntry = m_nhgmgr->getSonicPICByRIBID(ribIDA);
        uint32_t sonicPICobjID = sonicNHGEntry->getSonicPicContentObjId();
        ASSERT_NE(sonicNHGEntry, nullptr);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjID), "nexthop", nexthops), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICobjID), "vpn_sid", vpnsids), true);
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
        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribIDA), nullptr);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicPICobjID), fvs), false);
        ASSERT_EQ(m_nhgmgr->isSonicPICIDInUsed(swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, sonicPICobjID), false);
    }

    /*
     * Test: single SRv6 VPN NHG without NEXTHOP_GROUP_RECEIVED_FLAG.
     * Expect: entry is created in RIB table and marked as SRv6,
     *         but NO PIC object and NO sonic NHG object are created.
     */
    TEST_F(FpmSyncdNhgMgr, SingleSRv6VPNWithoutReceivedFlag)
    {
        uint32_t ribID = 10;

        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribID);
        nhgObj.nhg_flags = 0;
        nhgObj.depends.resize(0);
        nhgObj.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET6), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->isSRv6Nhg(), true);
        ASSERT_EQ(entry->hasSonicPICObj(), false);
        ASSERT_EQ(entry->needCreateSonicObject(), false);

        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribID), nullptr);

        uint32_t sonicObjID = entry->getSonicObjID();
        std::string nexthops;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjID), "nexthop", nexthops), false);

        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_picContextTable->get(to_string(sonicObjID), fvs), false);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribID), nullptr);
    }

    /*
     * Test: multi NHG whose SRv6 members lack the received flag.
     * The parent multi NHG inherits m_is_srv6_nhg=true from members,
     * but since members have no PIC objects, the parent should also
     * skip creating PIC and sonic NHG objects.
     */
    TEST_F(FpmSyncdNhgMgr, MultiNHGWithSRv6MembersWithoutReceivedFlag)
    {
        uint32_t ribIDA = 20;
        uint32_t ribIDB = 18;
        uint32_t ribIDC = 19;

        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDB);
        NextHopGroupFull nhgObjC = createSingleSRv6VPNNextHopNHGFull("2::2", "c::c", "e::e", ribIDC);
        nhgObjB.nhg_flags = 0;
        nhgObjC.nhg_flags = 0;
        nhgObjB.depends.resize(0);
        nhgObjC.depends.resize(0);
        nhgObjB.nh_grp_full_list.resize(0);
        nhgObjC.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);

        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC);
        ASSERT_NE(entryB, nullptr);
        ASSERT_NE(entryC, nullptr);
        ASSERT_EQ(entryB->isSRv6Nhg(), true);
        ASSERT_EQ(entryC->isSRv6Nhg(), true);
        ASSERT_EQ(entryB->hasSonicPICObj(), false);
        ASSERT_EQ(entryC->hasSonicPICObj(), false);

        map<uint32_t, NextHopGroupFull> nhgFullA = {
            { ribIDB, nhgObjB },
            { ribIDC, nhgObjC }
        };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(
            nhgFullA,
            { { ribIDB, 12 }, { ribIDC, 10 } },
            { { ribIDB, 0 }, { ribIDC, 0 } },
            dependsA, {}, ribIDA);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_EQ(entryA->isSRv6Nhg(), true);
        ASSERT_EQ(entryA->hasSonicPICObj(), false);
        ASSERT_EQ(entryA->needCreateSonicObject(), false);

        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribIDA), nullptr);

        uint32_t sonicObjIDA = entryA->getSonicObjID();
        std::string nexthops;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), false);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
    }

    /*
     * Test: mixed scenario where one SRv6 member has the received flag
     * and another does not. The parent multi NHG should still create
     * PIC/NHG objects because at least one member has a PIC object.
     */
    TEST_F(FpmSyncdNhgMgr, MultiNHGWithMixedSRv6ReceivedFlag)
    {
        uint32_t ribIDA = 30;
        uint32_t ribIDB = 28;
        uint32_t ribIDC = 29;

        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDB);
        nhgObjB.depends.resize(0);
        nhgObjB.nh_grp_full_list.resize(0);

        NextHopGroupFull nhgObjC = createSingleSRv6VPNNextHopNHGFull("2::2", "c::c", "e::e", ribIDC);
        nhgObjC.nhg_flags = 0;
        nhgObjC.depends.resize(0);
        nhgObjC.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);

        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC);
        ASSERT_NE(entryB, nullptr);
        ASSERT_NE(entryC, nullptr);
        ASSERT_EQ(entryB->hasSonicPICObj(), true);
        ASSERT_EQ(entryC->hasSonicPICObj(), false);

        map<uint32_t, NextHopGroupFull> nhgFullA = {
            { ribIDB, nhgObjB },
            { ribIDC, nhgObjC }
        };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(
            nhgFullA,
            { { ribIDB, 12 }, { ribIDC, 10 } },
            { { ribIDB, 0 }, { ribIDC, 0 } },
            dependsA, {}, ribIDA);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_EQ(entryA->isSRv6Nhg(), true);
        ASSERT_EQ(entryA->hasSonicPICObj(), true);

        SonicPICContentEntry *sonicPICEntry = m_nhgmgr->getSonicPICByRIBID(ribIDA);
        ASSERT_NE(sonicPICEntry, nullptr);
        ASSERT_EQ(sonicPICEntry->getType(), swss::SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
    }

    /*
     * Test: Two SRv6 VPN entries with different VPN SIDs but same gateway/segSrc
     * share one sonic NHG (since NHG key comparison skips vpnSid for NORMAL type),
     * while each has its own PIC object. Deleting one reduces refCount; deleting
     * all removes the shared NHG DB record.
     */
    TEST_F(FpmSyncdNhgMgr, SharedSonicNHGReuse)
    {
        uint32_t ribIDA = 40;
        uint32_t ribIDB = 41;

        NextHopGroupFull nhgObjA = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDA);
        nhgObjA.depends.resize(0);
        nhgObjA.nh_grp_full_list.resize(0);

        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("2::2", "a::a", "b::b", ribIDB);
        nhgObjB.depends.resize(0);
        nhgObjB.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);

        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryA, nullptr);
        ASSERT_NE(entryB, nullptr);

        uint32_t sonicObjIDA = entryA->getSonicObjID();
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        ASSERT_EQ(sonicObjIDA, sonicObjIDB);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);

        ASSERT_EQ(entryA->hasSonicPICObj(), true);
        ASSERT_EQ(entryB->hasSonicPICObj(), true);
        SonicPICContentEntry *picA = m_nhgmgr->getSonicPICByRIBID(ribIDA);
        SonicPICContentEntry *picB = m_nhgmgr->getSonicPICByRIBID(ribIDB);
        ASSERT_NE(picA, nullptr);
        ASSERT_NE(picB, nullptr);
        uint32_t picObjIDA = picA->getSonicPicContentObjId();
        uint32_t picObjIDB = picB->getSonicPicContentObjId();
        ASSERT_NE(picObjIDA, picObjIDB);

        std::string picNexhopsA, picVpnSidA, picNexhopsB, picVpnSidB;
        ASSERT_EQ(m_picContextTable->hget(to_string(picObjIDA), "nexthop", picNexhopsA), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(picObjIDA), "vpn_sid", picVpnSidA), true);
        ASSERT_EQ(picNexhopsA, "b::b");
        ASSERT_EQ(picVpnSidA, "1::1");
        ASSERT_EQ(m_picContextTable->hget(to_string(picObjIDB), "nexthop", picNexhopsB), true);
        ASSERT_EQ(m_picContextTable->hget(to_string(picObjIDB), "vpn_sid", picVpnSidB), true);
        ASSERT_EQ(picNexhopsB, "b::b");
        ASSERT_EQ(picVpnSidB, "2::2");

        std::string nexthops;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);
        ASSERT_EQ(nexthops, "b::b");

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA), nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDA), "nexthop", nexthops), true);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB), nullptr);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDA), false);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonicObjIDA), fvs), false);
    }


    /*
     * Test: Updating a received multi NHG by changing members triggers key change,
     * causing the old sonic NHG to be freed and a new sonic NHG created.
     * Steps:
     *   1. Create 3 received SRv6 NHGs: B, C, D
     *   2. Create received NHG A with members B, C
     *   3. Verify A's sonic NHG creation
     *   4. Update A's members to C, D
     *   5. Verify new sonic NHG created, old one freed
     */
    TEST_F(FpmSyncdNhgMgr, UpdateWithKeyChange)
    {
        uint32_t ribIDB = 61;
        uint32_t ribIDC = 62;
        uint32_t ribIDD = 63;
        uint32_t ribIDA = 64;

        NextHopGroupFull nhgObjB = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribIDB);
        nhgObjB.depends.resize(0);
        nhgObjB.nh_grp_full_list.resize(0);
        NextHopGroupFull nhgObjC = createSingleSRv6VPNNextHopNHGFull("2::2", "c::c", "d::d", ribIDC);
        nhgObjC.depends.resize(0);
        nhgObjC.nh_grp_full_list.resize(0);
        NextHopGroupFull nhgObjD = createSingleSRv6VPNNextHopNHGFull("3::3", "e::e", "f::f", ribIDD);
        nhgObjD.depends.resize(0);
        nhgObjD.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjD, AF_INET6), 0);

        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC);
        RIBNHGEntry *entryD = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDD);
        ASSERT_NE(entryB, nullptr);
        ASSERT_NE(entryC, nullptr);
        ASSERT_NE(entryD, nullptr);
        ASSERT_EQ(entryB->hasSonicPICObj(), true);
        ASSERT_EQ(entryC->hasSonicPICObj(), true);
        ASSERT_EQ(entryD->hasSonicPICObj(), true);

        map<uint32_t, NextHopGroupFull> nhgFullA = {
            { ribIDB, nhgObjB },
            { ribIDC, nhgObjC }
        };
        vector<uint32_t> dependsA = { ribIDB, ribIDC };
        NextHopGroupFull nhgObjA = createMultiNextHopNHGFull(
            nhgFullA,
            { { ribIDB, 10 }, { ribIDC, 20 } },
            { { ribIDB, 0 }, { ribIDC, 0 } },
            dependsA, {}, ribIDA);
        nhgObjA.nhg_flags = 1024;

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjA, AF_INET6), 0);

        RIBNHGEntry *entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        ASSERT_EQ(entryA->needCreateSonicObject(), true);
        uint32_t sonicObjIDOld = entryA->getSonicObjID();
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDOld), true);
        std::string nexthops;
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDOld), "nexthop", nexthops), true);

        map<uint32_t, NextHopGroupFull> nhgFullANew = {
            { ribIDC, nhgObjC },
            { ribIDD, nhgObjD }
        };
        vector<uint32_t> dependsANew = { ribIDC, ribIDD };
        NextHopGroupFull nhgObjANew = createMultiNextHopNHGFull(
            nhgFullANew,
            { { ribIDC, 20 }, { ribIDD, 30 } },
            { { ribIDC, 0 }, { ribIDD, 0 } },
            dependsANew, {}, ribIDA);
        nhgObjANew.nhg_flags = 1024;

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjANew, AF_INET6), 0);

        entryA = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDA);
        ASSERT_NE(entryA, nullptr);
        uint32_t sonicObjIDNew = entryA->getSonicObjID();
        ASSERT_NE(sonicObjIDOld, sonicObjIDNew);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDNew), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDOld), false);

        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDNew), "nexthop", nexthops), true);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonicObjIDOld), fvs), false);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDA), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDD), 0);
    }

    /*
     * Test: Shared NHG reference counting across create and update operations.
     * Steps:
     *   1. Create 4 received SRv6 NHGs: B1, B2, C1, C2
     *   2. Create non-received multi NHG B (members B1,B2), C (members C1,C2)
     *   3. Create non-received multi NHG D (members B1,B2) - shares B's sonic NHG
     *   4. Update B members to {C1,C2} - joins C's shared NHG, old {B1,B2} NHG still alive (D uses it)
     *   5. Update D members to {B1,C1} - creates new shared NHG, old {B1,B2} NHG deleted (refCount=0)
     */
    TEST_F(FpmSyncdNhgMgr, SharedNHGRefCountAcrossUpdates)
    {
        uint32_t ribIDB1 = 70;
        uint32_t ribIDB2 = 71;
        uint32_t ribIDC1 = 72;
        uint32_t ribIDC2 = 73;
        uint32_t ribIDB = 74;
        uint32_t ribIDC = 75;
        uint32_t ribIDD = 76;

        /* Step 1: Create 4 received SRv6 NHGs */
        NextHopGroupFull nhgObjB1 = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b1::b1", ribIDB1);
        nhgObjB1.depends.resize(0);
        nhgObjB1.nh_grp_full_list.resize(0);
        NextHopGroupFull nhgObjB2 = createSingleSRv6VPNNextHopNHGFull("2::2", "a::a", "b2::b2", ribIDB2);
        nhgObjB2.depends.resize(0);
        nhgObjB2.nh_grp_full_list.resize(0);
        NextHopGroupFull nhgObjC1 = createSingleSRv6VPNNextHopNHGFull("3::3", "a::a", "c1::c1", ribIDC1);
        nhgObjC1.depends.resize(0);
        nhgObjC1.nh_grp_full_list.resize(0);
        NextHopGroupFull nhgObjC2 = createSingleSRv6VPNNextHopNHGFull("4::4", "a::a", "c2::c2", ribIDC2);
        nhgObjC2.depends.resize(0);
        nhgObjC2.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB1, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB2, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC1, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC2, AF_INET6), 0);

        RIBNHGEntry *entryB1 = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB1);
        RIBNHGEntry *entryB2 = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB2);
        RIBNHGEntry *entryC1 = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC1);
        RIBNHGEntry *entryC2 = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC2);
        ASSERT_NE(entryB1, nullptr);
        ASSERT_NE(entryB2, nullptr);
        ASSERT_NE(entryC1, nullptr);
        ASSERT_NE(entryC2, nullptr);
        ASSERT_EQ(entryB1->hasSonicPICObj(), true);
        ASSERT_EQ(entryB2->hasSonicPICObj(), true);
        ASSERT_EQ(entryC1->hasSonicPICObj(), true);
        ASSERT_EQ(entryC2->hasSonicPICObj(), true);

        /* Step 2: Create non-received multi NHG B (members B1,B2) and C (members C1,C2) */
        map<uint32_t, NextHopGroupFull> nhgFullB = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        vector<uint32_t> dependsB = { ribIDB1, ribIDB2 };
        NextHopGroupFull nhgObjB = createMultiNextHopNHGFull(
            nhgFullB,
            { { ribIDB1, 1 }, { ribIDB2, 1 } },
            { { ribIDB1, 0 }, { ribIDB2, 0 } },
            dependsB, {}, ribIDB);

        map<uint32_t, NextHopGroupFull> nhgFullC = {
            { ribIDC1, nhgObjC1 },
            { ribIDC2, nhgObjC2 }
        };
        vector<uint32_t> dependsC = { ribIDC1, ribIDC2 };
        NextHopGroupFull nhgObjC = createMultiNextHopNHGFull(
            nhgFullC,
            { { ribIDC1, 1 }, { ribIDC2, 1 } },
            { { ribIDC1, 0 }, { ribIDC2, 0 } },
            dependsC, {}, ribIDC);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjB, AF_INET6), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjC, AF_INET6), 0);

        RIBNHGEntry *entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        RIBNHGEntry *entryC = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDC);
        ASSERT_NE(entryB, nullptr);
        ASSERT_NE(entryC, nullptr);
        ASSERT_EQ(entryB->needCreateSonicObject(), true);
        ASSERT_EQ(entryC->needCreateSonicObject(), true);
        uint32_t sonicObjIDB = entryB->getSonicObjID();
        uint32_t sonicObjIDC = entryC->getSonicObjID();
        ASSERT_NE(sonicObjIDB, sonicObjIDC);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDC), true);

        std::string nexthops, weights;
        vector<string> nexthopResults, weightResults;
        map<string, string> expectedNexthopB = { { "b1::b1", "1" }, { "b2::b2", "1" } };
        map<string, string> expectedNexthopC = { { "c1::c1", "1" }, { "c2::c2", "1" } };

        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), 2);
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopB.find(nexthopResults[i]), expectedNexthopB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopB.find(nexthopResults[i])->second);
        }

        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDC), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDC), "weight", weights), true);
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), 2);
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopC.find(nexthopResults[i]), expectedNexthopC.end());
            ASSERT_EQ(weightResults[i], expectedNexthopC.find(nexthopResults[i])->second);
        }

        /* Step 3: Create non-received multi NHG D (members B1,B2) - shares B's sonic NHG */
        map<uint32_t, NextHopGroupFull> nhgFullD = {
            { ribIDB1, nhgObjB1 },
            { ribIDB2, nhgObjB2 }
        };
        vector<uint32_t> dependsD = { ribIDB1, ribIDB2 };
        NextHopGroupFull nhgObjD = createMultiNextHopNHGFull(
            nhgFullD,
            { { ribIDB1, 1 }, { ribIDB2, 1 } },
            { { ribIDB1, 0 }, { ribIDB2, 0 } },
            dependsD, {}, ribIDD);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjD, AF_INET6), 0);

        RIBNHGEntry *entryD = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDD);
        ASSERT_NE(entryD, nullptr);
        ASSERT_EQ(entryD->needCreateSonicObject(), false);
        uint32_t sonicObjIDD = entryD->getSonicObjID();
        ASSERT_EQ(sonicObjIDD, sonicObjIDB);
        ASSERT_EQ(entryD->hasSonicPICObj(), true);
        uint32_t picObjIDB = m_nhgmgr->getSonicPICByRIBID(ribIDB)->getSonicPicContentObjId();
        uint32_t picObjIDD = m_nhgmgr->getSonicPICByRIBID(ribIDD)->getSonicPicContentObjId();
        ASSERT_NE(picObjIDB, picObjIDD);

        /* Step 4: Update B members to {C1,C2} - joins C's shared NHG */
        map<uint32_t, NextHopGroupFull> nhgFullBNew = {
            { ribIDC1, nhgObjC1 },
            { ribIDC2, nhgObjC2 }
        };
        vector<uint32_t> dependsBNew = { ribIDC1, ribIDC2 };
        NextHopGroupFull nhgObjBNew = createMultiNextHopNHGFull(
            nhgFullBNew,
            { { ribIDC1, 1 }, { ribIDC2, 1 } },
            { { ribIDC1, 0 }, { ribIDC2, 0 } },
            dependsBNew, {}, ribIDB);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjBNew, AF_INET6), 0);

        entryB = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDB);
        ASSERT_NE(entryB, nullptr);
        uint32_t sonicObjIDBAfter = entryB->getSonicObjID();
        ASSERT_EQ(sonicObjIDBAfter, sonicObjIDC);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), true);

        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDB), "weight", weights), true);
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), 2);
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopB.find(nexthopResults[i]), expectedNexthopB.end());
            ASSERT_EQ(weightResults[i], expectedNexthopB.find(nexthopResults[i])->second);
        }

        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDC), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDC), "weight", weights), true);
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), 2);
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopC.find(nexthopResults[i]), expectedNexthopC.end());
            ASSERT_EQ(weightResults[i], expectedNexthopC.find(nexthopResults[i])->second);
        }

        /* Step 5: Update D members to {B1,C1} - creates new shared NHG, old {B1,B2} NHG deleted */
        map<uint32_t, NextHopGroupFull> nhgFullDNew = {
            { ribIDB1, nhgObjB1 },
            { ribIDC1, nhgObjC1 }
        };
        vector<uint32_t> dependsDNew = { ribIDB1, ribIDC1 };
        NextHopGroupFull nhgObjDNew = createMultiNextHopNHGFull(
            nhgFullDNew,
            { { ribIDB1, 1 }, { ribIDC1, 1 } },
            { { ribIDB1, 0 }, { ribIDC1, 0 } },
            dependsDNew, {}, ribIDD);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjDNew, AF_INET6), 0);

        entryD = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDD);
        ASSERT_NE(entryD, nullptr);
        uint32_t sonicObjIDDAfter = entryD->getSonicObjID();
        ASSERT_NE(sonicObjIDDAfter, sonicObjIDB);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDDAfter), true);
        ASSERT_EQ(m_nhgmgr->isSonicNHGIDInUsed(sonicObjIDB), false);
        std::vector<FieldValueTuple> fvs;
        ASSERT_EQ(m_nextHopTable->get(to_string(sonicObjIDB), fvs), false);

        map<string, string> expectedNexthopD = { { "b1::b1", "1" }, { "c1::c1", "1" } };
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDDAfter), "nexthop", nexthops), true);
        ASSERT_EQ(m_nextHopTable->hget(to_string(sonicObjIDDAfter), "weight", weights), true);
        nexthopResults = splitResults(nexthops, ",");
        weightResults = splitResults(weights, ",");
        ASSERT_EQ(nexthopResults.size(), 2);
        ASSERT_EQ(nexthopResults.size(), weightResults.size());
        for (size_t i = 0; i < nexthopResults.size(); i++)
        {
            ASSERT_NE(expectedNexthopD.find(nexthopResults[i]), expectedNexthopD.end());
            ASSERT_EQ(weightResults[i], expectedNexthopD.find(nexthopResults[i])->second);
        }

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDD), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDB2), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDC2), 0);
    }

    /*
     * Test: Deleting a non-existent NHG returns 0 (not an error).
     */
    TEST_F(FpmSyncdNhgMgr, DeleteNonExistentNHG)
    {
        ASSERT_EQ(m_nhgmgr->delNHGFull(99999), 0);
    }

    /*
     * Test: seg_src field is correctly stored in PIC_CONTEXT_TABLE.
     */
    TEST_F(FpmSyncdNhgMgr, SegSrcInPICContextTable)
    {
        uint32_t ribID = 70;

        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull("1::1", "a::a", "b::b", ribID);
        nhgObj.depends.resize(0);
        nhgObj.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET6), 0);

        SonicPICContentEntry *picEntry = m_nhgmgr->getSonicPICByRIBID(ribID);
        ASSERT_NE(picEntry, nullptr);
        uint32_t sonicPICObjID = picEntry->getSonicPicContentObjId();

        std::string segSrc;
        ASSERT_EQ(m_picContextTable->hget(to_string(sonicPICObjID), "seg_src", segSrc), true);
        ASSERT_EQ(segSrc, "a::a");

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: enableNHG/disableNHG toggle the enable status of RIBNHGEntry.
     */
    TEST_F(FpmSyncdNhgMgr, EnableDisableNHG)
    {
        uint32_t ribID = 71;

        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->getNhgEnableStatus(), true);

        entry->disableNHG();
        ASSERT_EQ(entry->getNhgEnableStatus(), false);

        entry->enableNHG();
        ASSERT_EQ(entry->getNhgEnableStatus(), true);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: checkNeedUpdate detects field-level changes (weight, gateway, ifname).
     */
    TEST_F(FpmSyncdNhgMgr, CheckNeedUpdateFieldChanges)
    {
        uint32_t ribID = 72;

        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        bool updated = false;
        entry->checkNeedUpdate(nhgObj, AF_INET, updated);
        ASSERT_EQ(updated, false);

        NextHopGroupFull nhgObjNewGw = createSingleIPv4NextHopNHGFull("192.100.1.2", "120.0.0.1", ribID);
        updated = false;
        entry->checkNeedUpdate(nhgObjNewGw, AF_INET, updated);
        ASSERT_EQ(updated, true);

        updated = false;
        entry->checkNeedUpdate(nhgObj, AF_INET6, updated);
        ASSERT_EQ(updated, true);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: Adding a multi NHG that references a non-existent group member should fail.
     */
    TEST_F(FpmSyncdNhgMgr, AddWithInvalidGroupMember)
    {
        uint32_t ribIDMember = 80;
        uint32_t ribIDParent = 81;
        uint32_t ribIDGhost = 82; // never added to the RIB table

        NextHopGroupFull nhgObjMember = createSingleIPv4NextHopNHGFull("10.0.0.1", "10.0.0.100", ribIDMember);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjMember, AF_INET), 0);

        // Create a multi NHG that depends on ribIDMember (exists) and ribIDGhost (does not exist)
        map<uint32_t, NextHopGroupFull> nhgFullParent = {
            { ribIDMember, nhgObjMember },
            { ribIDGhost, nhgObjMember } // reuse struct but with ghost ID
        };
        // Fix the id in the second entry to match the ghost
        NextHopGroupFull nhgObjGhost = createSingleIPv4NextHopNHGFull("10.0.0.2", "10.0.0.200", ribIDGhost);
        nhgFullParent[ribIDGhost] = nhgObjGhost;

        vector<uint32_t> dependsParent = { ribIDMember, ribIDGhost };
        NextHopGroupFull nhgObjParent = createMultiNextHopNHGFull(
            nhgFullParent,
            { { ribIDMember, 10 }, { ribIDGhost, 20 } },
            { { ribIDMember, 0 }, { ribIDGhost, 0 } },
            dependsParent, {}, ribIDParent);

        // addNHGFull should fail because ribIDGhost is not in the RIB table
        ASSERT_NE(m_nhgmgr->addNHGFull(nhgObjParent, AF_INET), 0);

        // Parent should not exist in the table
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDParent), nullptr);

        // Cleanup
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDMember), 0);
    }

    /*
     * Test: Multi NHG with only one resolved member sets m_is_single=true
     * and does NOT create a sonic NHG object.
     */
    TEST_F(FpmSyncdNhgMgr, SingleMemberMultiNHG)
    {
        uint32_t ribIDMember1 = 85;
        uint32_t ribIDMember2 = 86;
        uint32_t ribIDParent = 87;

        // Add two singleton members
        NextHopGroupFull nhgObjMember1 = createSingleIPv4NextHopNHGFull("10.1.0.1", "10.1.0.100", ribIDMember1);
        NextHopGroupFull nhgObjMember2 = createSingleIPv4NextHopNHGFull("10.1.0.2", "10.1.0.200", ribIDMember2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjMember1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjMember2, AF_INET), 0);

        // Create parent multi NHG where member1 has num_direct=0 (resolved)
        // and member2 has num_direct=1 (not resolved)
        // So only one member resolves -> m_is_single=true
        map<uint32_t, NextHopGroupFull> nhgFullParent = {
            { ribIDMember1, nhgObjMember1 },
            { ribIDMember2, nhgObjMember2 }
        };
        vector<uint32_t> dependsParent = { ribIDMember1, ribIDMember2 };
        NextHopGroupFull nhgObjParent = createMultiNextHopNHGFull(
            nhgFullParent,
            { { ribIDMember1, 10 }, { ribIDMember2, 20 } },
            { { ribIDMember1, 0 }, { ribIDMember2, 1 } }, // member2 num_direct=1 -> not resolved
            dependsParent, {}, ribIDParent);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjParent, AF_INET), 0);

        RIBNHGEntry *entryParent = m_nhgmgr->getRIBNHGEntryByRIBID(ribIDParent);
        ASSERT_NE(entryParent, nullptr);
        // m_is_single should be true since only one member resolved
        ASSERT_EQ(entryParent->needCreateSonicObject(), false);
        // No sonic NHG ID allocated
        ASSERT_EQ(entryParent->getSonicObjID(), 0u);

        // Cleanup
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDParent), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDMember1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribIDMember2), 0);
    }

    /*
     * Test: non-SRv6 NHG with received flag should not create sonic NHG or PIC objects.
     * Covers add, update, and delete.
     */
    TEST_F(FpmSyncdNhgMgr, NonSRv6ReceivedFlagNHG)
    {
        uint32_t ribID = 50;

        NextHopGroupFull nhgObj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100", ribID);
        nhgObj.nhg_flags = NEXTHOP_GROUP_RECEIVED_FLAG;
        nhgObj.depends.resize(0);
        nhgObj.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET6), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->isSRv6Nhg(), false);
        ASSERT_EQ(entry->hasSonicPICObj(), false);
        ASSERT_EQ(entry->needCreateSonicObject(), false);
        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribID), nullptr);

        // Update: re-add with different gateway, should still not create sonic objects
        NextHopGroupFull nhgObjUpdated = createSingleIPv6NextHopNHGFull("fc00::2", "fc00::200", ribID);
        nhgObjUpdated.nhg_flags = NEXTHOP_GROUP_RECEIVED_FLAG;
        nhgObjUpdated.depends.resize(0);
        nhgObjUpdated.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObjUpdated, AF_INET6), 0);

        entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->isSRv6Nhg(), false);
        ASSERT_EQ(entry->hasSonicPICObj(), false);
        ASSERT_EQ(entry->needCreateSonicObject(), false);
        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribID), nullptr);

        // Delete
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribID), nullptr);
    }

    /*
     * Test: RIBNHGTable::addEntry returns error when NHG ID already exists.
     */
    TEST_F(FpmSyncdNhgMgr, DuplicateAddEntry)
    {
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 100);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        // Second add with same ID should fail (returns non-zero from addNHGFull
        // which internally triggers updateExistingNHGFull, but direct addEntry would fail)
        // Here the NHGMgr::addNHGFull detects existing ID and calls update instead,
        // so let's test the RIBNHGTable::addEntry directly via the internal table
        ASSERT_EQ(getRibNhgTable()->addEntry(nhgObj, AF_INET), -1);

        ASSERT_EQ(m_nhgmgr->delNHGFull(100), 0);
    }

    /*
     * Test: RIBNHGTable::updateEntry returns error when NHG ID does not exist.
     */
    TEST_F(FpmSyncdNhgMgr, UpdateNonExistentEntry)
    {
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 999);
        bool updated = false;
        ASSERT_EQ(getRibNhgTable()->updateEntry(nhgObj, AF_INET, updated), -1);
        ASSERT_EQ(updated, false);
    }

    /*
     * Test: delEntry for an entry that has no sonic NHG ID (sonicNHGID == 0).
     * Covers the early-return branch in RIBNHGTable::delEntry.
     */
    TEST_F(FpmSyncdNhgMgr, DeleteEntryWithNoSonicNHGID)
    {
        uint32_t ribID = 60;
        NextHopGroupFull nhgObj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100", ribID);
        nhgObj.nhg_flags = NEXTHOP_GROUP_RECEIVED_FLAG;
        nhgObj.depends.resize(0);
        nhgObj.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET6), 0);
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->getSonicObjID(), 0u);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribID), nullptr);
    }

    /*
     * Test: delNHGFull for a non-existent NHG ID returns 0 (not an error).
     */
    TEST_F(FpmSyncdNhgMgr, DeleteNonExistentNHGReturnsZero)
    {
        ASSERT_EQ(m_nhgmgr->delNHGFull(99999), 0);
    }

    /*
     * Test: SonicPICContentTable::addEntry returns error for duplicate entry.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentTableDuplicateAdd)
    {
        // First, add a valid SRv6 VPN NHG to create a PIC content entry
        uint32_t ribID = 70;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(entry->hasSonicPICObj());

        uint32_t picID = entry->getSonicPICObjID();
        ASSERT_NE(picID, 0u);

        // Try to add a duplicate SonicPICContentObject with the same type and id
        SonicPICContentObject dupObj;
        dupObj.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
        dupObj.id = picID;
        dupObj.nexthop = "10.0.0.2";
        dupObj.vpnSid = "fc00::2";
        dupObj.segSrc = "fc00::200";
        ASSERT_EQ(getSonicNhgTable()->addEntry(dupObj), -1);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: SonicPICContentTable::updateEntry returns error when entry does not exist.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentTableUpdateNonExistent)
    {
        SonicPICContentObject obj;
        obj.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
        obj.id = 9999;
        obj.nexthop = "10.0.0.1";
        obj.vpnSid = "fc00::1";
        obj.segSrc = "fc00::100";
        ASSERT_EQ(getSonicNhgTable()->updateEntry(obj), -1);
    }

    /*
     * Test: SonicPICContentTable::delEntry with non-existent entry (by type and id).
     * Should not crash, just log a warning.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentTableDeleteNonExistent)
    {
        // Delete by type and id - entry not found
        getSonicNhgTable()->delEntry(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, 9999);

        // Delete by SonicPICContentObject - entry not found
        SonicPICContentObject obj;
        obj.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
        obj.id = 9999;
        getSonicNhgTable()->delEntry(obj);
    }

    /*
     * Test: SonicPICContentTable::delEntry with default (unsupported) type.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentTableDeleteUnsupportedType)
    {
        getSonicNhgTable()->delEntry(static_cast<sonicNhgObjType>(99), 1);
    }

    /*
     * Test: SonicPICContentTable::getEntry with default (unsupported) type returns nullptr.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentTableGetEntryUnsupportedType)
    {
        SonicPICContentEntry *entry = getSonicNhgTable()->getEntry(
            static_cast<sonicNhgObjType>(99), 1);
        ASSERT_EQ(entry, nullptr);
    }

    /*
     * Test: SonicIDMgr::allocateID and freeID with unsupported type.
     * getAllocator returns nullptr for unknown types.
     */
    TEST_F(FpmSyncdNhgMgr, SonicIDMgrUnsupportedType)
    {
        uint32_t id = getSonicIdManager().allocateID(static_cast<sonicNhgObjType>(99));
        ASSERT_EQ(id, 0u);

        // freeID with unsupported type should not crash
        getSonicIdManager().freeID(static_cast<sonicNhgObjType>(99), 1);
    }

    /*
     * Test: SonicIDMgr::init with unsupported type returns error.
     */
    TEST_F(FpmSyncdNhgMgr, SonicIDMgrInitUnsupportedType)
    {
        SonicIDMgr idMgr;
        int ret = idMgr.init({static_cast<sonicNhgObjType>(99)});
        ASSERT_EQ(ret, -1);
    }

    /*
     * Test: SonicNHGObjectKey comparison operators.
     */
    TEST_F(FpmSyncdNhgMgr, SonicNHGObjectKeyComparison)
    {
        SonicNHGObjectKey key1;
        key1.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        key1.nexthop = "10.0.0.1";
        key1.ifName = "eth0";
        key1.segSrc = "";
        key1.vpnSid = "";

        SonicNHGObjectKey key2 = key1;
        ASSERT_TRUE(key1 == key2);
        ASSERT_FALSE(key1 != key2);

        // Different type
        key2.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
        ASSERT_TRUE(key1 != key2);
        ASSERT_TRUE(key1 < key2);

        // Same type, different nexthop
        key2 = key1;
        key2.nexthop = "10.0.0.2";
        ASSERT_TRUE(key1 != key2);

        // SRv6 PIC type, compare vpnSid
        key1.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
        key1.vpnSid = "fc00::1";
        key2 = key1;
        key2.vpnSid = "fc00::2";
        ASSERT_TRUE(key1 != key2);

        // Different ifName
        key2 = key1;
        key2.ifName = "eth1";
        ASSERT_TRUE(key1 != key2);

        // Different segSrc
        key2 = key1;
        key2.segSrc = "fc00::100";
        ASSERT_TRUE(key1 != key2);

        // Different groupMember size
        key2 = key1;
        key2.groupMember.push_back(std::make_pair(1u, 10u));
        ASSERT_TRUE(key1 != key2);

        // Same groupMember content but different order (should be equal after sorting)
        key1.groupMember.clear();
        key1.groupMember.push_back(std::make_pair(2u, 20u));
        key1.groupMember.push_back(std::make_pair(1u, 10u));
        key2.groupMember.clear();
        key2.groupMember.push_back(std::make_pair(1u, 10u));
        key2.groupMember.push_back(std::make_pair(2u, 20u));
        ASSERT_TRUE(key1 == key2);
    }

    /*
     * Test: RIBNHGTable::subSonicNHGObjectRef with non-existent key (early return).
     */
    TEST_F(FpmSyncdNhgMgr, SubSonicNHGObjectRefNonExistentKey)
    {
        SonicNHGObjectKey key;
        key.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        key.nexthop = "10.0.0.99";
        // Should not crash, just return
        getRibNhgTable()->subSonicNHGObjectRef(key);
    }

    /*
     * Test: RIBNHGTable::subSonicNHGObjectRef with refCount already at 0 (underflow protection).
     */
    TEST_F(FpmSyncdNhgMgr, SubSonicNHGObjectRefUnderflow)
    {
        SonicNHGObjectKey key;
        key.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        key.nexthop = "10.0.0.1";
        key.ifName = "eth0";

        // Manually insert with refCount = 0
        getCreatedSharedNhgMap().insert(
            std::make_pair(key, SonicNHGObjectInfo(1, 0)));

        // Should log error and return without decrementing
        getRibNhgTable()->subSonicNHGObjectRef(key);

        // Entry should still exist since underflow was prevented
        ASSERT_NE(getCreatedSharedNhgMap().find(key),
                  getCreatedSharedNhgMap().end());

        // Clean up
        getCreatedSharedNhgMap().erase(key);
    }

    /*
     * Test: RIBNHGTable::getCreatedSharedNHGObjectID returns 0 for non-existent key.
     */
    TEST_F(FpmSyncdNhgMgr, GetCreatedSharedNHGObjectIDNotFound)
    {
        SonicNHGObjectKey key;
        key.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        key.nexthop = "nonexistent";
        ASSERT_EQ(getRibNhgTable()->getCreatedSharedNHGObjectID(key), 0);
    }

    /*
     * Test: RIBNHGTable::writeToDB returns error when fvVector is empty.
     * We create a received-flag NHG (no sonic obj created), then manipulate
     * the entry to have empty fvVector.
     */
    TEST_F(FpmSyncdNhgMgr, WriteToDBEmptyFvVector)
    {
        uint32_t ribID = 80;
        NextHopGroupFull nhgObj = createSingleIPv6NextHopNHGFull("fc00::1", "fc00::100", ribID);
        nhgObj.nhg_flags = NEXTHOP_GROUP_RECEIVED_FLAG;
        nhgObj.depends.resize(0);
        nhgObj.nh_grp_full_list.resize(0);

        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET6), 0);
        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        // Clear fvVector to trigger the empty check
        getRibEntryFvVector(entry).clear();
        ASSERT_EQ(getRibNhgTable()->writeToDB(entry), -1);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: updateSonicPICObject with previousSonicPICObjID == 0 returns error.
     */
    TEST_F(FpmSyncdNhgMgr, UpdateSonicPICObjectInvalidPreviousID)
    {
        uint32_t ribID = 90;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        // Call updateSonicPICObject with previousSonicPICObjID = 0
        ASSERT_EQ(callUpdateSonicPICObject(entry, 0), -1);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: SonicPICContentTable::writeToDB returns error when fvVector is empty.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentWriteToDBEmptyFvVector)
    {
        uint32_t ribID = 91;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        uint32_t picID = entry->getSonicPICObjID();
        ASSERT_NE(picID, 0u);

        SonicPICContentEntry *picEntry = getSonicNhgTable()->getEntry(
            SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, picID);
        ASSERT_NE(picEntry, nullptr);

        // Clear fvVector to trigger empty check
        getPicEntryFvVector(picEntry).clear();
        ASSERT_EQ(getSonicNhgTable()->writeToDB(picEntry), -1);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: SonicPICContentEntry::setEntry with unsupported type returns error.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentEntrySetEntryUnsupportedType)
    {
        SonicPICContentObject obj;
        obj.type = static_cast<sonicNhgObjType>(99);
        obj.id = 1;
        // addEntry internally calls setEntry, which should fail
        ASSERT_EQ(getSonicNhgTable()->addEntry(obj), -1);
    }

    /*
     * Test: RIBNHGEntry::setEntry fails when group member does not exist in table.
     * Covers the "NextHop id in group not found" error path.
     */
    TEST_F(FpmSyncdNhgMgr, SetEntryWithNonExistentGroupMember)
    {
        uint32_t ribIDMember = 200;
        uint32_t ribIDParent = 201;

        // Build a fake member NHG object to populate nh_grp_full_list,
        // but do NOT add it to the manager — so isNHGExist(ribIDMember) returns false.
        NextHopGroupFull fakeMember = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribIDMember);
        std::map<uint32_t, NextHopGroupFull> nhgFullParent;
        nhgFullParent[ribIDMember] = fakeMember;

        NextHopGroupFull nhgObjParent = createMultiNextHopNHGFull(
            nhgFullParent,
            { { ribIDMember, 10 } },
            { { ribIDMember, 0 } },
            { ribIDMember }, {}, ribIDParent);

        // This should fail because member ribIDMember doesn't exist in the table
        ASSERT_NE(m_nhgmgr->addNHGFull(nhgObjParent, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(ribIDParent), nullptr);
    }

    /*
     * Test: SonicIDAllocator allocateID and freeID basic operations.
     */
    TEST_F(FpmSyncdNhgMgr, SonicIDAllocatorBasicOps)
    {
        // Allocate two IDs for NHG_NORMAL type
        uint32_t id1 = getSonicIdManager().allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        ASSERT_NE(id1, 0u);

        uint32_t id2 = getSonicIdManager().allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        ASSERT_NE(id2, 0u);
        ASSERT_NE(id1, id2);

        // Free first ID
        getSonicIdManager().freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, id1);

        // Free second ID
        getSonicIdManager().freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, id2);

        // Allocate for PIC type
        uint32_t picId = getSonicIdManager().allocateID(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);
        ASSERT_NE(picId, 0u);
        getSonicIdManager().freeID(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, picId);
    }

    /*
     * Test: RIBNHGTable::cleanUp properly clears all maps.
     */
    TEST_F(FpmSyncdNhgMgr, RIBNHGTableCleanUp)
    {
        NextHopGroupFull nhgObj1 = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", 300);
        NextHopGroupFull nhgObj2 = createSingleIPv4NextHopNHGFull("192.100.1.2", "120.0.0.2", 301);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj2, AF_INET), 0);

        getRibNhgTable()->cleanUp();

        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(300), nullptr);
        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByRIBID(301), nullptr);
    }

    /*
     * Test: SonicPICContentTable::cleanUp properly clears all entries.
     */
    TEST_F(FpmSyncdNhgMgr, SonicPICContentTableCleanUp)
    {
        uint32_t ribID = 92;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        uint32_t picID = entry->getSonicPICObjID();
        ASSERT_NE(picID, 0u);

        getSonicNhgTable()->cleanUp();

        ASSERT_EQ(getSonicNhgTable()->getEntry(
            SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, picID), nullptr);

        // Clean up RIB side too
        getRibNhgTable()->cleanUp();
    }

    /*
     * Test: updateSonicPICObject succeeds for valid SRv6 PIC entry.
     * Covers the normal path of updateSonicPICObject.
     */
    TEST_F(FpmSyncdNhgMgr, UpdateSonicPICObjectSuccess)
    {
        uint32_t ribID = 93;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(entry->hasSonicPICObj());
        uint32_t picID = entry->getSonicPICObjID();
        ASSERT_NE(picID, 0u);

        // Update with same entry and valid previous ID
        ASSERT_EQ(callUpdateSonicPICObject(entry, picID), 0);

        // Verify PIC entry still exists after update
        SonicPICContentEntry *picEntry = getSonicNhgTable()->getEntry(
            SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, picID);
        ASSERT_NE(picEntry, nullptr);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: isSonicNHGIDInUsed and isSonicPICIDInUsed checks.
     */
    TEST_F(FpmSyncdNhgMgr, SonicIDInUsedChecks)
    {
        // Initially no IDs in use (beyond what NHGMgr ctor set up)
        ASSERT_FALSE(m_nhgmgr->isSonicNHGIDInUsed(99999));
        ASSERT_FALSE(m_nhgmgr->isSonicPICIDInUsed(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, 99999));

        // Add an entry and check its sonic IDs are tracked
        uint32_t ribID = 94;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        uint32_t sonicID = entry->getSonicObjID();
        if (sonicID != 0) {
            ASSERT_TRUE(m_nhgmgr->isSonicNHGIDInUsed(sonicID));
        }

        uint32_t picID = entry->getSonicPICObjID();
        if (picID != 0) {
            ASSERT_TRUE(m_nhgmgr->isSonicPICIDInUsed(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC, picID));
        }

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: SonicNHGObjectKey::createSonicPICContentObjectKey from SonicPICContentObject
     * covers the static creation function.
     */
    TEST_F(FpmSyncdNhgMgr, CreateSonicPICContentObjectKey)
    {
        SonicPICContentObject obj;
        obj.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC;
        obj.nexthop = "10.0.0.1";
        obj.vpnSid = "fc00::1";
        obj.segSrc = "fc00::100";
        obj.ifName = "eth0";
        obj.groupMember.push_back(std::make_pair(1u, 10u));

        SonicNHGObjectKey key = SonicNHGObjectKey::createSonicPICContentObjectKey(obj);
        ASSERT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);
        ASSERT_EQ(key.nexthop, "10.0.0.1");
        ASSERT_EQ(key.vpnSid, "fc00::1");
        ASSERT_EQ(key.segSrc, "fc00::100");
        ASSERT_EQ(key.ifName, "eth0");
        ASSERT_EQ(key.groupMember.size(), 1u);

        // Non-SRv6 type should not copy vpnSid
        SonicPICContentObject obj2;
        obj2.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        obj2.nexthop = "10.0.0.1";
        obj2.vpnSid = "fc00::1";
        SonicNHGObjectKey key2 = SonicNHGObjectKey::createSonicPICContentObjectKey(obj2);
        ASSERT_EQ(key2.type, SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        ASSERT_TRUE(key2.vpnSid.empty());
    }

    /*
     * Test: RIBNHGEntry enable/disable NHG operations.
     */
    TEST_F(FpmSyncdNhgMgr, EnableDisableNHGEntry)
    {
        uint32_t ribID = 95;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(entry->getNhgEnableStatus());

        entry->disableNHG();
        ASSERT_FALSE(entry->getNhgEnableStatus());

        entry->enableNHG();
        ASSERT_TRUE(entry->getNhgEnableStatus());

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: RIBNHGEntry getter functions for SRv6 fields.
     */
    TEST_F(FpmSyncdNhgMgr, RIBNHGEntrySRv6FieldGetters)
    {
        uint32_t ribID = 96;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(entry->isSRv6Nhg());
        ASSERT_EQ(entry->getSonicObjType(), SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);
        ASSERT_FALSE(entry->getVPNSIDStr().empty());
        ASSERT_FALSE(entry->getSegSrcStr().empty());
        ASSERT_FALSE(entry->getNextHopStr().empty());

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: getSonicPICByRIBID returns nullptr for non-SRv6 entry.
     */
    TEST_F(FpmSyncdNhgMgr, GetSonicPICByRIBIDNonSRv6)
    {
        uint32_t ribID = 97;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("192.100.1.1", "120.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(ribID), nullptr);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: getSonicPICByRIBID returns nullptr for non-existent RIB ID.
     */
    TEST_F(FpmSyncdNhgMgr, GetSonicPICByRIBIDNonExistent)
    {
        ASSERT_EQ(m_nhgmgr->getSonicPICByRIBID(88888), nullptr);
    }

    /*
     * Test: checkNeedUpdate detects changes in depends vector.
     * Exercises compareDependsAndDependents returning false on depends mismatch.
     */
    TEST_F(FpmSyncdNhgMgr, CheckNeedUpdateDependsChange)
    {
        uint32_t ribID = 200;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("10.1.1.1", "10.1.1.100", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        // Same object -> no update
        bool updated = false;
        entry->checkNeedUpdate(nhgObj, AF_INET, updated);
        ASSERT_FALSE(updated);

        // Modify depends
        NextHopGroupFull nhgModified = nhgObj;
        nhgModified.depends.push_back(999);
        updated = false;
        entry->checkNeedUpdate(nhgModified, AF_INET, updated);
        ASSERT_TRUE(updated);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: checkNeedUpdate detects changes in dependents vector.
     */
    TEST_F(FpmSyncdNhgMgr, CheckNeedUpdateDependentsChange)
    {
        uint32_t ribID = 201;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("10.2.1.1", "10.2.1.100", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        NextHopGroupFull nhgModified = nhgObj;
        nhgModified.dependents.push_back(888);
        bool updated = false;
        entry->checkNeedUpdate(nhgModified, AF_INET, updated);
        ASSERT_TRUE(updated);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: checkNeedUpdate detects changes in nh_grp_full_list.
     * Exercises compareNHGFullList branches: size mismatch, id/weight/num_direct mismatch.
     */
    TEST_F(FpmSyncdNhgMgr, CheckNeedUpdateNHGFullListChange)
    {
        // Setup member NHGs
        uint32_t memberID1 = 210;
        uint32_t memberID2 = 211;
        uint32_t parentID = 212;

        NextHopGroupFull mem1 = createSingleIPv4NextHopNHGFull("10.3.1.1", "10.3.1.100", memberID1);
        NextHopGroupFull mem2 = createSingleIPv4NextHopNHGFull("10.3.1.2", "10.3.1.101", memberID2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(mem1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(mem2, AF_INET), 0);

        map<uint32_t, NextHopGroupFull> members = {{memberID1, mem1}, {memberID2, mem2}};
        map<uint32_t, uint32_t> weights = {{memberID1, 1}, {memberID2, 1}};
        map<uint32_t, uint32_t> numDirects = {{memberID1, 1}, {memberID2, 1}};
        vector<uint32_t> depends = {memberID1, memberID2};
        vector<uint32_t> dependents;

        NextHopGroupFull parentNhg = createMultiNextHopNHGFull(members, weights, numDirects, depends, dependents, parentID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(parentNhg, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(parentID);
        ASSERT_NE(entry, nullptr);

        // Size mismatch: remove one member from the list
        NextHopGroupFull modified = parentNhg;
        modified.nh_grp_full_list.pop_back();
        bool updated = false;
        entry->checkNeedUpdate(modified, AF_INET, updated);
        ASSERT_TRUE(updated);

        // Weight mismatch
        NextHopGroupFull modifiedWeight = parentNhg;
        modifiedWeight.nh_grp_full_list[0].weight = 99;
        updated = false;
        entry->checkNeedUpdate(modifiedWeight, AF_INET, updated);
        ASSERT_TRUE(updated);

        // num_direct mismatch
        NextHopGroupFull modifiedNumDirect = parentNhg;
        modifiedNumDirect.nh_grp_full_list[0].num_direct = 99;
        updated = false;
        entry->checkNeedUpdate(modifiedNumDirect, AF_INET, updated);
        ASSERT_TRUE(updated);

        // id mismatch
        NextHopGroupFull modifiedId = parentNhg;
        modifiedId.nh_grp_full_list[0].id = 9999;
        updated = false;
        entry->checkNeedUpdate(modifiedId, AF_INET, updated);
        ASSERT_TRUE(updated);

        // Clean up
        ASSERT_EQ(m_nhgmgr->delNHGFull(parentID), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(memberID1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(memberID2), 0);
    }

    /*
     * Test: checkNeedUpdate detects SRv6 field changes.
     * Exercises compareNHGSRv6Fields branches.
     */
    TEST_F(FpmSyncdNhgMgr, CheckNeedUpdateSRv6FieldsChange)
    {
        uint32_t ribID = 220;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        // Same object -> no update
        bool updated = false;
        entry->checkNeedUpdate(nhgObj, AF_INET, updated);
        ASSERT_FALSE(updated);

        // One has srv6, other doesn't (set nh_srv6 to nullptr)
        NextHopGroupFull noSrv6 = nhgObj;
        if (noSrv6.nh_srv6) {
            if (noSrv6.nh_srv6->seg6_segs) free(noSrv6.nh_srv6->seg6_segs);
            free(noSrv6.nh_srv6);
        }
        noSrv6.nh_srv6 = nullptr;
        updated = false;
        entry->checkNeedUpdate(noSrv6, AF_INET, updated);
        ASSERT_TRUE(updated);

        // Different VPN SID content
        NextHopGroupFull diffSid = nhgObj;
        if (diffSid.nh_srv6) {
            if (diffSid.nh_srv6->seg6_segs) free(diffSid.nh_srv6->seg6_segs);
            free(diffSid.nh_srv6);
        }
        fib::nexthop_srv6 *newSrv6 = static_cast<fib::nexthop_srv6*>(malloc(sizeof(fib::nexthop_srv6)));
        new (newSrv6) fib::nexthop_srv6();
        newSrv6->seg6local_action = nhgObj.nh_srv6->seg6local_action;
        newSrv6->seg6local_ctx = nhgObj.nh_srv6->seg6local_ctx;
        newSrv6->seg6_src = nhgObj.nh_srv6->seg6_src;
        size_t segs_size = sizeof(fib::seg6_seg_stack) + 1 * sizeof(struct in6_addr);
        fib::seg6_seg_stack *newSegs = static_cast<fib::seg6_seg_stack*>(malloc(segs_size));
        memset(newSegs, 0, segs_size);
        newSegs->num_segs = 1;
        inet_pton(AF_INET6, "fc00::99", &newSegs->seg[0]);
        newSrv6->seg6_segs = newSegs;
        diffSid.nh_srv6 = newSrv6;
        updated = false;
        entry->checkNeedUpdate(diffSid, AF_INET, updated);
        ASSERT_TRUE(updated);
        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: checkNeedUpdate detects weight, vrf_id, ifindex, ifname, type changes.
     */
    TEST_F(FpmSyncdNhgMgr, CheckNeedUpdateWeightVrfIfnameType)
    {
        uint32_t ribID = 230;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("10.5.1.1", "10.5.1.100", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        // weight change
        NextHopGroupFull modWeight = nhgObj;
        modWeight.weight = nhgObj.weight + 5;
        bool updated = false;
        entry->checkNeedUpdate(modWeight, AF_INET, updated);
        ASSERT_TRUE(updated);

        // vrf_id change
        NextHopGroupFull modVrf = nhgObj;
        modVrf.vrf_id = nhgObj.vrf_id + 1;
        updated = false;
        entry->checkNeedUpdate(modVrf, AF_INET, updated);
        ASSERT_TRUE(updated);

        // ifindex change
        NextHopGroupFull modIfindex = nhgObj;
        modIfindex.ifindex = nhgObj.ifindex + 1;
        updated = false;
        entry->checkNeedUpdate(modIfindex, AF_INET, updated);
        ASSERT_TRUE(updated);

        // ifname change
        NextHopGroupFull modIfname = nhgObj;
        modIfname.ifname = "EthernetChanged";
        updated = false;
        entry->checkNeedUpdate(modIfname, AF_INET, updated);
        ASSERT_TRUE(updated);

        // type change
        NextHopGroupFull modType = nhgObj;
        modType.type = fib::NEXTHOP_TYPE_IPV6;
        updated = false;
        entry->checkNeedUpdate(modType, AF_INET, updated);
        ASSERT_TRUE(updated);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: checkNeedUpdate detects blackhole type changes.
     */
    TEST_F(FpmSyncdNhgMgr, CheckNeedUpdateBlackholeType)
    {
        uint32_t ribID = 231;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("10.6.1.1", "10.6.1.100", ribID);
        // Make it a blackhole type
        nhgObj.type = fib::NEXTHOP_TYPE_BLACKHOLE;
        nhgObj.bh_type = fib::BLACKHOLE_UNSPEC;
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        // Same -> no update
        bool updated = false;
        entry->checkNeedUpdate(nhgObj, AF_INET, updated);
        ASSERT_FALSE(updated);

        // Change bh_type
        NextHopGroupFull modBh = nhgObj;
        modBh.bh_type = fib::BLACKHOLE_REJECT;
        updated = false;
        entry->checkNeedUpdate(modBh, AF_INET, updated);
        ASSERT_TRUE(updated);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: createSonicNormalNHGObjectKey for single nexthop entry.
     */
    TEST_F(FpmSyncdNhgMgr, CreateSonicNormalNHGObjectKeySingleNexthop)
    {
        uint32_t ribID = 240;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("10.7.1.1", "10.7.1.100", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(entry->isSingleNexthop());

        SonicNHGObjectKey key;
        SonicNHGObjectKey::createSonicNormalNHGObjectKey(entry, key);
        ASSERT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        ASSERT_FALSE(key.nexthop.empty());
        ASSERT_FALSE(key.ifName.empty());
        ASSERT_TRUE(key.groupMember.empty());

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: createSonicNormalNHGObjectKey for multi nexthop entry.
     */
    TEST_F(FpmSyncdNhgMgr, CreateSonicNormalNHGObjectKeyMultiNexthop)
    {
        uint32_t memberID1 = 250;
        uint32_t memberID2 = 251;
        uint32_t parentID = 252;

        NextHopGroupFull mem1 = createSingleIPv4NextHopNHGFull("10.8.1.1", "10.8.1.100", memberID1);
        NextHopGroupFull mem2 = createSingleIPv4NextHopNHGFull("10.8.1.2", "10.8.1.101", memberID2);
        ASSERT_EQ(m_nhgmgr->addNHGFull(mem1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(mem2, AF_INET), 0);

        map<uint32_t, NextHopGroupFull> members = {{memberID1, mem1}, {memberID2, mem2}};
        map<uint32_t, uint32_t> weights = {{memberID1, 1}, {memberID2, 1}};
        map<uint32_t, uint32_t> numDirects = {{memberID1, 0}, {memberID2, 0}};
        vector<uint32_t> depends = {memberID1, memberID2};
        vector<uint32_t> dependents;

        NextHopGroupFull parentNhg = createMultiNextHopNHGFull(members, weights, numDirects, depends, dependents, parentID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(parentNhg, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(parentID);
        ASSERT_NE(entry, nullptr);
        ASSERT_FALSE(entry->isSingleNexthop());

        SonicNHGObjectKey key;
        SonicNHGObjectKey::createSonicNormalNHGObjectKey(entry, key);
        ASSERT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        ASSERT_TRUE(key.nexthop.empty());
        ASSERT_FALSE(key.groupMember.empty());

        ASSERT_EQ(m_nhgmgr->delNHGFull(parentID), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(memberID1), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(memberID2), 0);
    }

    /*
     * Test: createSonicPICContentObjectKey from RIBNHGEntry overload.
     */
    TEST_F(FpmSyncdNhgMgr, CreateSonicPICContentObjectKeyFromEntry)
    {
        uint32_t ribID = 260;
        NextHopGroupFull nhgObj = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        RIBNHGEntry *entry = m_nhgmgr->getRIBNHGEntryByRIBID(ribID);
        ASSERT_NE(entry, nullptr);

        SonicNHGObjectKey key;
        int ret = SonicNHGObjectKey::createSonicPICContentObjectKey(entry, key);
        ASSERT_EQ(ret, 0);
        ASSERT_EQ(key.type, SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC);
        ASSERT_FALSE(key.vpnSid.empty());
        ASSERT_FALSE(key.segSrc.empty());

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: getRIBNHGEntryByKey always returns NULL (not implemented).
     */
    TEST_F(FpmSyncdNhgMgr, GetRIBNHGEntryByKeyReturnsNull)
    {
        uint32_t ribID = 270;
        NextHopGroupFull nhgObj = createSingleIPv4NextHopNHGFull("10.9.1.1", "10.9.1.100", ribID);
        ASSERT_EQ(m_nhgmgr->addNHGFull(nhgObj, AF_INET), 0);

        ASSERT_EQ(m_nhgmgr->getRIBNHGEntryByKey("any_key"), nullptr);

        ASSERT_EQ(m_nhgmgr->delNHGFull(ribID), 0);
    }

    /*
     * Test: dumpNHGGroupFull does not crash on various NHG types.
     */
    TEST_F(FpmSyncdNhgMgr, DumpNHGGroupFullDoesNotCrash)
    {
        // IPv4 nexthop
        NextHopGroupFull nhgIPv4 = createSingleIPv4NextHopNHGFull("10.10.1.1", "10.10.1.100", 280);
        ASSERT_NO_FATAL_FAILURE(callDumpNHGGroupFull(nhgIPv4));

        // IPv6 nexthop
        NextHopGroupFull nhgIPv6 = createSingleIPv6NextHopNHGFull("fc00::10", "fc00::200", 281);
        ASSERT_NO_FATAL_FAILURE(callDumpNHGGroupFull(nhgIPv6));

        // SRv6 nexthop
        NextHopGroupFull nhgSRv6 = createSingleSRv6VPNNextHopNHGFull(
            "fc00::1", "fc00::100", "10.0.0.1", 282);
        ASSERT_NO_FATAL_FAILURE(callDumpNHGGroupFull(nhgSRv6));

        // Multi nexthop
        NextHopGroupFull mem1 = createSingleIPv4NextHopNHGFull("10.10.2.1", "10.10.2.100", 283);
        NextHopGroupFull mem2 = createSingleIPv4NextHopNHGFull("10.10.2.2", "10.10.2.101", 284);
        ASSERT_EQ(m_nhgmgr->addNHGFull(mem1, AF_INET), 0);
        ASSERT_EQ(m_nhgmgr->addNHGFull(mem2, AF_INET), 0);
        map<uint32_t, NextHopGroupFull> members = {{283, mem1}, {284, mem2}};
        map<uint32_t, uint32_t> weights = {{283, 1}, {284, 1}};
        map<uint32_t, uint32_t> numDirects = {{283, 1}, {284, 1}};
        vector<uint32_t> depends = {283, 284};
        vector<uint32_t> dependents;
        NextHopGroupFull nhgMulti = createMultiNextHopNHGFull(members, weights, numDirects, depends, dependents, 285);
        ASSERT_NO_FATAL_FAILURE(callDumpNHGGroupFull(nhgMulti));

        ASSERT_EQ(m_nhgmgr->delNHGFull(283), 0);
        ASSERT_EQ(m_nhgmgr->delNHGFull(284), 0);
    }
}