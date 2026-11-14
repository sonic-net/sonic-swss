#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
//#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "redisutility.h"
#include "warm_restart.h"
#define private public
#include "warmRestartAssist.h"
#undef private

#define APP_WRA_TEST_TABLE_NAME "TEST_TABLE"

namespace warmrestartassist_test
{
    using namespace std;

    shared_ptr<swss::DBConnector> m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    shared_ptr<swss::RedisPipeline> m_app_db_pipeline = make_shared<swss::RedisPipeline>(m_app_db.get());
    shared_ptr<swss::ProducerStateTable> m_wra_test_table = make_shared<swss::ProducerStateTable>(m_app_db.get(), APP_WRA_TEST_TABLE_NAME);

    AppRestartAssist *appRestartAssist;

    struct WarmrestartassistTest : public ::testing::Test
    {
        WarmrestartassistTest()
        {
            appRestartAssist = new AppRestartAssist(m_app_db_pipeline.get(), "testsyncd", "swss", 0);
            appRestartAssist->m_warmStartInProgress = true;
            appRestartAssist->registerAppTable(APP_WRA_TEST_TABLE_NAME, m_wra_test_table.get());
        }

        void SetUp() override
        {
            testing_db::reset();

            Table testTable = Table(m_app_db.get(), APP_WRA_TEST_TABLE_NAME);
            testTable.set("key",
                          {
                              {"field", "value0"},
                          });
        }

        void TearDown() override
        {
        }
    };

    TEST_F(WarmrestartassistTest, warmRestartAssistTest)
    {
        appRestartAssist->readTablesToMap();
        vector<FieldValueTuple> fvVector;
        fvVector.emplace_back("field", "value1");
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key", fvVector, false);
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key", fvVector, false);
        appRestartAssist->reconcile();

        fvVector.clear();
        Table testTable = Table(m_app_db.get(), APP_WRA_TEST_TABLE_NAME);
        ASSERT_TRUE(testTable.get("key", fvVector));
        ASSERT_EQ(fvField(fvVector[0]), "field");
        ASSERT_EQ(fvValue(fvVector[0]), "value1");
    }

    /*
     * keepStaleEntries() keeps the entries nothing refreshed and leaves updates and deletes
     * alone, whether they come before or after it.
     */
    TEST_F(WarmrestartassistTest, keepStaleEntriesTest)
    {
        Table testTable = Table(m_app_db.get(), APP_WRA_TEST_TABLE_NAME);
        testTable.set("key2", { {"field", "value2"} });
        testTable.set("key3", { {"field", "value3"} });
        testTable.set("key4", { {"field", "value4"} });
        testTable.set("key5", { {"field", "value5"} });

        appRestartAssist->readTablesToMap();
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key", { {"field", "value1"} }, false);
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key4", {}, true);
        appRestartAssist->keepStaleEntries();
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key3", {}, true);
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key5", { {"field", "value5b"} }, false);
        appRestartAssist->reconcile();

        vector<FieldValueTuple> fvVector;
        ASSERT_TRUE(testTable.get("key", fvVector));
        ASSERT_EQ(fvValue(fvVector[0]), "value1");
        ASSERT_TRUE(testTable.get("key2", fvVector));
        ASSERT_EQ(fvValue(fvVector[0]), "value2");
        ASSERT_FALSE(testTable.get("key3", fvVector));
        ASSERT_FALSE(testTable.get("key4", fvVector));
        ASSERT_TRUE(testTable.get("key5", fvVector));
        ASSERT_EQ(fvValue(fvVector[0]), "value5b");
    }

    /* keepStaleEntries() reports each entry it keeps, with its fields and without the cache state. */
    TEST_F(WarmrestartassistTest, keepStaleEntriesReportsKeptEntriesTest)
    {
        Table testTable = Table(m_app_db.get(), APP_WRA_TEST_TABLE_NAME);
        testTable.set("key2", { {"field", "value2"} });

        appRestartAssist->readTablesToMap();
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key", { {"field", "value0"} }, false);

        vector<string> kept;
        appRestartAssist->keepStaleEntries(
            [&kept](const string &tableName, const string &key, const vector<FieldValueTuple> &fvVector)
            {
                ASSERT_EQ(tableName, APP_WRA_TEST_TABLE_NAME);
                ASSERT_EQ(fvVector.size(), 1u);
                ASSERT_EQ(fvField(fvVector[0]), "field");
                ASSERT_EQ(fvValue(fvVector[0]), "value2");
                kept.push_back(key);
            });
        ASSERT_EQ(kept, vector<string>({ "key2" }));
        appRestartAssist->reconcile();
    }

    /*
     * A table registered with replaceFields gets a NEW entry that drops a field of its
     * APPL_DB entry deleted before it is set, so the old field does not stay in the hash.
     * Other NEW entries, and other tables, are only set.
     */
    TEST_F(WarmrestartassistTest, reconcileReplacesChangedFieldsTest)
    {
        const string replaceTableName = "TEST_REPLACE_TABLE";
        ProducerStateTable replaceTable(m_app_db.get(), replaceTableName);
        appRestartAssist->registerAppTable(replaceTableName, &replaceTable, true);

        Table appTable = Table(m_app_db.get(), replaceTableName);
        appTable.set("moved", { {"remote_vtep", "1.1.1.1"}, {"vni", "100"} });
        appTable.set("changed", { {"remote_vtep", "1.1.1.1"}, {"vni", "100"} });
        Table testTable = Table(m_app_db.get(), APP_WRA_TEST_TABLE_NAME);
        testTable.set("merged", { {"field", "value0"}, {"other", "value0"} });

        appRestartAssist->readTablesToMap();
        appRestartAssist->insertToMap(replaceTableName, "moved", { {"nexthop_group", "536870913"}, {"vni", "100"} }, false);
        appRestartAssist->insertToMap(replaceTableName, "changed", { {"remote_vtep", "2.2.2.2"}, {"vni", "100"} }, false);
        appRestartAssist->insertToMap(replaceTableName, "added", { {"remote_vtep", "3.3.3.3"}, {"vni", "100"} }, false);
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "key", { {"field", "value0"} }, false);
        appRestartAssist->insertToMap(APP_WRA_TEST_TABLE_NAME, "merged", { {"field", "value1"} }, false);
        testing_db::resetOperationCounters();
        appRestartAssist->reconcile();

        vector<FieldValueTuple> fvVector;
        ASSERT_TRUE(appTable.get("moved", fvVector));
        ASSERT_EQ(fvVector.size(), 2u);
        ASSERT_EQ(swss::fvsGetValue(fvVector, "nexthop_group", true).get(), "536870913");
        ASSERT_FALSE(swss::fvsGetValue(fvVector, "remote_vtep", true));
        ASSERT_TRUE(appTable.get("changed", fvVector));
        ASSERT_EQ(swss::fvsGetValue(fvVector, "remote_vtep", true).get(), "2.2.2.2");
        ASSERT_TRUE(appTable.get("added", fvVector));
        ASSERT_EQ(swss::fvsGetValue(fvVector, "remote_vtep", true).get(), "3.3.3.3");
        ASSERT_EQ(testing_db::getProducerDelCount(replaceTableName), 1u);

        ASSERT_TRUE(testTable.get("merged", fvVector));
        ASSERT_EQ(swss::fvsGetValue(fvVector, "field", true).get(), "value1");
        ASSERT_EQ(swss::fvsGetValue(fvVector, "other", true).get(), "value0");
        ASSERT_EQ(testing_db::getProducerDelCount(APP_WRA_TEST_TABLE_NAME), 0u);
    }
}
