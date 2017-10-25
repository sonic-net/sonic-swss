#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include <vector>
#include "dbconnector.h"
#include "select.h"
#include "schema.h"
#include "exec.h"
#include "orchbase.h"
#include "producerstatetable.h"

using namespace std;
using namespace swss;

/*
 * In the following unit test:
 * OrchBase supports orchestrating notifications from three tables in TEST_CONFIG_DB,
 * CFGTABLE_A_1, CFGTABLE_A_2 & CFGTABLE_B with client type of keyspace SubscriberStateTable,
 * and one table in TEST_APP_DB,  APPTABLE_C with client type of ConsumerStateTable,
 * and directs the notification to each result table in TEST_RESULT_DB
 */
#define TEST_CONFIG_DB        (CONFIG_DB)
#define TEST_APP_DB              (7)
#define TEST_RESULT_DB           (8)

#define SELECT_TIMEOUT 1000

const string cfgTableNameA1 = "CFGTABLE_A_1";
const string cfgTableNameA2 = "CFGTABLE_A_2";
const string cfgTableNameB = "CFGTABLE_B";
const string appTableNameC = "APPTABLE_C";

const string keySuffix = "_Key";
const string keyA1 = cfgTableNameA1 + keySuffix;
const string keyA2 = cfgTableNameA2 + keySuffix;
const string keyB = cfgTableNameB + keySuffix;
const string keyC = appTableNameC + keySuffix;

static inline void clearDB(int db)
{
    DBConnector dbc(db, "localhost", 6379, 0);
    RedisReply r(&dbc, "FLUSHDB", REDIS_REPLY_STATUS);
    r.checkStatusOK();
}

class CfgAgentA : public OrchBase
{
public:
    CfgAgentA(DBConnector *cfgDb, DBConnector *resultDb, vector<string> tableNames):
        OrchBase(cfgDb, tableNames),
        m_cfgTableA1(cfgDb, cfgTableNameA1, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgTableA2(cfgDb, cfgTableNameA2, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_resultTableA1(resultDb, cfgTableNameA1),
        m_resultTableA2(resultDb, cfgTableNameA2)
    {
    }

    void syncCfgDB()
    {
        OrchBase::syncDB(cfgTableNameA1, m_cfgTableA1);
        OrchBase::syncDB(cfgTableNameA2, m_cfgTableA2);
    }

private:
    Table m_cfgTableA1, m_cfgTableA2;
    Table m_resultTableA1, m_resultTableA2;

    void doTask(Consumer &consumer)
    {
        string table_name = consumer.m_consumer->getTableName();
        string key_expected = table_name + keySuffix;

        EXPECT_TRUE(table_name == cfgTableNameA1 || table_name == cfgTableNameA2);

        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple t = it->second;

            string key = kfvKey(t);
            ASSERT_STREQ(key.c_str(), key_expected.c_str());

            string op = kfvOp(t);
            if (op == SET_COMMAND)
            {
                if (table_name == cfgTableNameA1)
                {
                    m_resultTableA1.set(kfvKey(t), kfvFieldsValues(t));
                }
                else
                {
                    m_resultTableA2.set(kfvKey(t), kfvFieldsValues(t));
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (table_name == cfgTableNameA1)
                {
                    m_resultTableA1.del(kfvKey(t));
                }
                else
                {
                    m_resultTableA2.del(kfvKey(t));
                }
            }
            it = consumer.m_toSync.erase(it);
            continue;
        }
    }
};

class CfgAgentB : public OrchBase
{
public:
    CfgAgentB(DBConnector *cfgDb, DBConnector *resultDb, vector<string> tableNames):
        OrchBase(cfgDb, tableNames),
        m_cfgTableB(cfgDb, cfgTableNameB, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_resultTableB(resultDb, cfgTableNameB)
    {

    }

    void syncCfgDB()
    {
        OrchBase::syncDB(cfgTableNameB, m_cfgTableB);
    }

private:
    Table m_cfgTableB;
    Table m_resultTableB;

    void doTask(Consumer &consumer)
    {
        string table_name = consumer.m_consumer->getTableName();
        string key_expected = table_name + keySuffix;

        EXPECT_TRUE(table_name == cfgTableNameB);

        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple t = it->second;

            string key = kfvKey(t);
            ASSERT_STREQ(key.c_str(), key_expected.c_str());

            string op = kfvOp(t);
            if (op == SET_COMMAND)
            {
                m_resultTableB.set(kfvKey(t), kfvFieldsValues(t));
            }
            else if (op == DEL_COMMAND)
            {
                m_resultTableB.del(kfvKey(t));
            }
            it = consumer.m_toSync.erase(it);
            continue;
        }
    }
};

class AppAgentC : public OrchBase
{
public:
    AppAgentC(DBConnector *appDb, DBConnector *resultDb, vector<string> tableNames):
        OrchBase(appDb, tableNames),
        m_appTableC(appDb, appTableNameC),
        m_resultTableC(resultDb, appTableNameC)
    {

    }

    void syncAppDB()
    {
        OrchBase::syncDB(appTableNameC, m_appTableC);
    }

private:
    Table m_appTableC;
    Table m_resultTableC;

    void doTask(Consumer &consumer)
    {
        string table_name = consumer.m_consumer->getTableName();
        string key_expected = table_name + keySuffix;

        EXPECT_TRUE(table_name == appTableNameC);

        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple t = it->second;

            string key = kfvKey(t);
            ASSERT_STREQ(key.c_str(), key_expected.c_str());

            string op = kfvOp(t);
            if (op == SET_COMMAND)
            {
                m_resultTableC.set(kfvKey(t), kfvFieldsValues(t));
            }
            else if (op == DEL_COMMAND)
            {
                m_resultTableC.del(kfvKey(t));
            }
            it = consumer.m_toSync.erase(it);
            continue;
        }
    }
};

// Thread for both publisher and producer
static void publisherProducerWorkerSet()
{
    // To trigger keyspace notification
    DBConnector cfgDb(TEST_CONFIG_DB, "localhost", 6379, 0);
    Table tableA1(&cfgDb, cfgTableNameA1, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableA2(&cfgDb, cfgTableNameA2, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableB(&cfgDb, cfgTableNameB, CONFIGDB_TABLE_NAME_SEPARATOR);

    // TO trigger producer state notifiation
    DBConnector appDb(TEST_APP_DB, "localhost", 6379, 0);
    ProducerStateTable tableC(&appDb, appTableNameC);

    vector<FieldValueTuple> fields;
    FieldValueTuple t("field", "value");
    fields.push_back(t);

    tableA1.set(keyA1, fields);
    tableA2.set(keyA2, fields);
    tableB.set(keyB, fields);
    tableC.set(keyC, fields);
}

static void publisherProducerWorkerDel()
{
    DBConnector cfgDb(TEST_CONFIG_DB, "localhost", 6379, 0);
    Table tableA1(&cfgDb, cfgTableNameA1, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableA2(&cfgDb, cfgTableNameA2, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableB(&cfgDb, cfgTableNameB, CONFIGDB_TABLE_NAME_SEPARATOR);

    DBConnector appDb(TEST_APP_DB, "localhost", 6379, 0);
    ProducerStateTable tableC(&appDb, appTableNameC);

    tableA1.del(keyA1);
    tableA2.del(keyA2);
    tableB.del(keyB);
    tableC.del(keyC);
}

// worker thread for both keyspace subscriber and state consumer
static void subscriberConsumerWorker(std::vector<OrchBase *> cfgOrchList)
{
    swss::Select s;
    for (OrchBase *o : cfgOrchList)
    {
        s.addSelectables(o->getSelectables());
    }

    while (true)
    {
        Selectable *sel;
        int fd, ret;

        ret = s.select(&sel, &fd, SELECT_TIMEOUT);
        if (ret == Select::TIMEOUT)
        {
            static int maxWait = 10;
            maxWait--;
            if (maxWait < 0)
            {
                // This unit testing should finish in 10 seconds!
                break;;
            }
            continue;
        }
        EXPECT_EQ(ret, Select::OBJECT);

        for (OrchBase *o : cfgOrchList)
        {
            TableConsumable *c = (TableConsumable *)sel;
            if (o->hasSelectable(c))
            {
                o->execute(c->getTableName());
            }
        }
    }
}

TEST(OrchBase, test)
{
    thread *publisherThread;
    thread *subscriberThread;
    vector<FieldValueTuple> values;

    vector<string> agent_a_tables = {
        cfgTableNameA1,
        cfgTableNameA2,
    };
    vector<string> agent_b_tables = {
        cfgTableNameB,
    };

    vector<string> agent_c_tables = {
        appTableNameC,
    };

    cout << "Starting OrchBase testing" << endl;
    clearDB(TEST_CONFIG_DB);
    clearDB(TEST_APP_DB);
    clearDB(TEST_RESULT_DB);

    string result;
    string kea_cmd = "redis-cli config set notify-keyspace-events KEA";
    int ret = exec(kea_cmd, result);
    EXPECT_TRUE(ret == 0);

    // Connection to configDB will work as keysapce SubscriberStateTable client in OrchBase.
    // Note that TEST_CONFIG_DB must be defined as CONFIG_DB
    DBConnector cfgDb(TEST_CONFIG_DB, "localhost", 6379, 0);

    // Connection to other DB will work as ConsumerStateTable client in OrchBase.
    DBConnector appDb(TEST_APP_DB, "localhost", 6379, 0);

    // connection to result DB is just for checking the processing result.
    DBConnector resultDb(TEST_RESULT_DB, "localhost", 6379, 0);

    CfgAgentA agent_a(&cfgDb, &resultDb, agent_a_tables);
    CfgAgentB agent_b(&cfgDb, &resultDb, agent_b_tables);
    AppAgentC agent_c(&appDb, &resultDb, agent_c_tables);

    std::vector<OrchBase *> cfgOrchList = {&agent_a, &agent_b, &agent_c};

    cout << "- Step 1. Provision TEST_CONFIG_DB and TEST_APP_DB" << endl;

    subscriberThread = new thread(subscriberConsumerWorker, cfgOrchList);

    publisherThread = new thread(publisherProducerWorkerSet);
    publisherThread->join();
    delete publisherThread;

    sleep(1);

    cout << "- Step 2. Verify TEST_RESULT_DB content" << endl;
    Table resultTableA1(&resultDb, cfgTableNameA1);
    Table resultTableA2(&resultDb, cfgTableNameA2);
    Table resultTableB(&resultDb, cfgTableNameB);
    Table resultTableC(&resultDb, appTableNameC);
    ASSERT_EQ(resultTableA1.get(keyA1, values), true);
    EXPECT_EQ(resultTableA2.get(keyA2, values), true);
    EXPECT_EQ(resultTableB.get(keyB, values), true);
    EXPECT_EQ(resultTableC.get(keyC, values), true);

    cout << "- Step 3. Flush TEST_RESULT_DB" << endl;
    clearDB(TEST_RESULT_DB);
    EXPECT_EQ(resultTableA1.get(keyA1, values), false);
    EXPECT_EQ(resultTableA2.get(keyA2, values), false);
    EXPECT_EQ(resultTableB.get(keyB, values), false);
    EXPECT_EQ(resultTableC.get(keyC, values), false);

    cout << "- Step 4. Sync from TEST_CONFIG_DB and TEST_APP_DB" << endl;
    agent_a.syncCfgDB();
    agent_b.syncCfgDB();
    agent_c.syncAppDB();

    cout << "- Step 5. Verify TEST_RESULT_DB content" << endl;
    EXPECT_EQ(resultTableA1.get(keyA1, values), true);
    EXPECT_EQ(resultTableA2.get(keyA2, values), true);
    EXPECT_EQ(resultTableB.get(keyB, values), true);
    EXPECT_EQ(resultTableC.get(keyC, values), true);

    cout << "- Step 6. Clean TEST_CONFIG_DB and TEST_APP_DB" << endl;
    publisherThread = new thread(publisherProducerWorkerDel);
    publisherThread->join();
    delete publisherThread;
    sleep(1);
    cout << "- Step 7. Verify TEST_RESULT_DB content is empty" << endl;
    EXPECT_EQ(resultTableA1.get(keyA1, values), false);
    EXPECT_EQ(resultTableA2.get(keyA2, values), false);
    EXPECT_EQ(resultTableB.get(keyB, values), false);
    EXPECT_EQ(resultTableC.get(keyC, values), false);

    subscriberThread->join();
    delete subscriberThread;
    cout << "Done." << endl;
}
