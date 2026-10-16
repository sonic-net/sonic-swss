#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
//#include "mock_orchagent_main.h"
#include "mock_table.h"
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
}
