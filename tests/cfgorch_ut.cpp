#include <unistd.h>
#include <vector>
#include "dbconnector.h"
#include "select.h"
#include "schema.h"
#include "exec.h"
#include "cfgorch.h"
#include "gtest/gtest.h"

using namespace std;
using namespace swss;

/*
 * In the following unit test:
 * cfgOrch supports orchestrating notifications from three tables in TEST_CONFIG_DB,
 * and directs the notification to each target table in TEST_APP_DB
 */
#define TEST_CONFIG_DB        (7)
#define TEST_APP_DB           (8)

#define SELECT_TIMEOUT 1000

const string cfgTableNameA1 = "CFGTABLE_A_1";
const string cfgTableNameA2 = "CFGTABLE_A_2";
const string cfgTableNameB = "CFGTABLE_B";

const string keySuffix = "_Key";
const string keyA1 = cfgTableNameA1 + keySuffix;
const string keyA2 = cfgTableNameA2 + keySuffix;
const string keyB = cfgTableNameB + keySuffix;

static inline void clearDB(int db)
{
    DBConnector dbc(db, "localhost", 6379, 0);
    RedisReply r(&dbc, "FLUSHDB", REDIS_REPLY_STATUS);
    r.checkStatusOK();
}

class CfgAgentA : public CfgOrch
{
public:
    CfgAgentA(DBConnector *cfgDb, DBConnector *appDb, vector<string> tableNames):
        CfgOrch(cfgDb, tableNames),
        m_cfgTableA1(cfgDb, cfgTableNameA1, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgTableA2(cfgDb, cfgTableNameA2, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_appTableA1(appDb, cfgTableNameA1),
        m_appTableA2(appDb, cfgTableNameA2)
    {
    }

    void syncCfgDB()
    {
        CfgOrch::syncCfgDB(cfgTableNameA1, m_cfgTableA1);
        CfgOrch::syncCfgDB(cfgTableNameA2, m_cfgTableA2);
    }

private:
    Table m_cfgTableA1, m_cfgTableA2;
    Table m_appTableA1, m_appTableA2;

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
                    m_appTableA1.set(kfvKey(t), kfvFieldsValues(t));
                }
                else
                {
                    m_appTableA2.set(kfvKey(t), kfvFieldsValues(t));
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (table_name == cfgTableNameA1)
                {
                    m_appTableA1.del(kfvKey(t));
                }
                else
                {
                    m_appTableA2.del(kfvKey(t));
                }
            }
            it = consumer.m_toSync.erase(it);
            continue;
        }
    }
};

class CfgAgentB : public CfgOrch
{
public:
    CfgAgentB(DBConnector *cfgDb, DBConnector *appDb, vector<string> tableNames):
        CfgOrch(cfgDb, tableNames),
        m_cfgTableB(cfgDb, cfgTableNameB, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_appTableB(appDb, cfgTableNameB)
    {

    }

    void syncCfgDB()
    {
        CfgOrch::syncCfgDB(cfgTableNameB, m_cfgTableB);
    }

private:
    Table m_cfgTableB;
    Table m_appTableB;

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
                m_appTableB.set(kfvKey(t), kfvFieldsValues(t));
            }
            else if (op == DEL_COMMAND)
            {
                m_appTableB.del(kfvKey(t));
            }
            it = consumer.m_toSync.erase(it);
            continue;
        }
    }
};

static void publisherWorkerSet()
{
    DBConnector cfgDb(TEST_CONFIG_DB, "localhost", 6379, 0);
    Table tableA1(&cfgDb, cfgTableNameA1, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableA2(&cfgDb, cfgTableNameA2, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableB(&cfgDb, cfgTableNameB, CONFIGDB_TABLE_NAME_SEPARATOR);

    vector<FieldValueTuple> fields;
    FieldValueTuple t("field", "value");
    fields.push_back(t);

    tableA1.set(keyA1, fields);
    tableA2.set(keyA2, fields);
    tableB.set(keyB, fields);
}

static void publisherWorkerDel()
{
    DBConnector cfgDb(TEST_CONFIG_DB, "localhost", 6379, 0);
    Table tableA1(&cfgDb, cfgTableNameA1, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableA2(&cfgDb, cfgTableNameA2, CONFIGDB_TABLE_NAME_SEPARATOR);
    Table tableB(&cfgDb, cfgTableNameB, CONFIGDB_TABLE_NAME_SEPARATOR);

    tableA1.del(keyA1);
    tableA2.del(keyA2);
    tableB.del(keyB);
}


static void subscriberWorker(std::vector<CfgOrch *> cfgOrchList)
{
    swss::Select s;
    for (CfgOrch *o : cfgOrchList)
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

        for (CfgOrch *o : cfgOrchList)
        {
            TableConsumable *c = (TableConsumable *)sel;
            if (o->hasSelectable(c))
            {
                o->execute(c->getTableName());
            }
        }
    }
}

TEST(CfgOrch, test)
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

    cout << "Starting cfgOrch testing" << endl;
    clearDB(TEST_CONFIG_DB);
    clearDB(TEST_APP_DB);

    string result;
    string kea_cmd = "redis-cli config set notify-keyspace-events KEA";
    int ret = exec(kea_cmd, result);
    EXPECT_TRUE(ret == 0);

    DBConnector cfgDb(TEST_CONFIG_DB, "localhost", 6379, 0);
    DBConnector appDb(TEST_APP_DB, "localhost", 6379, 0);

    CfgAgentA agent_a(&cfgDb, &appDb, agent_a_tables);
    CfgAgentB agent_b(&cfgDb, &appDb, agent_b_tables);

    std::vector<CfgOrch *> cfgOrchList = {&agent_a, &agent_b};

    cout << "- Step 1. Provision TEST_CONFIG_DB" << endl;

    subscriberThread = new thread(subscriberWorker, cfgOrchList);

    publisherThread = new thread(publisherWorkerSet);
    publisherThread->join();
    delete publisherThread;

    sleep(1);

    cout << "- Step 2. Verify TEST_APP_DB content" << endl;
    Table appTableA1(&appDb, cfgTableNameA1);
    Table appTableA2(&appDb, cfgTableNameA2);
    Table appTableB(&appDb, cfgTableNameB);
    ASSERT_EQ(appTableA1.get(keyA1, values), true);
    EXPECT_EQ(appTableA2.get(keyA2, values), true);
    EXPECT_EQ(appTableB.get(keyB, values), true);

    cout << "- Step 3. Flush TEST_APP_DB" << endl;
    clearDB(TEST_APP_DB);
    EXPECT_EQ(appTableA1.get(keyA1, values), false);
    EXPECT_EQ(appTableA2.get(keyA2, values), false);
    EXPECT_EQ(appTableB.get(keyB, values), false);

    cout << "- Step 4. Sync from TEST_CONFIG_DB" << endl;
    agent_a.syncCfgDB();
    agent_b.syncCfgDB();

    cout << "- Step 5. Verify TEST_APP_DB content" << endl;
    EXPECT_EQ(appTableA1.get(keyA1, values), true);
    EXPECT_EQ(appTableA2.get(keyA2, values), true);
    EXPECT_EQ(appTableB.get(keyB, values), true);

    cout << "- Step 6. Clean TEST_CONFIG_DB" << endl;
    publisherThread = new thread(publisherWorkerDel);
    publisherThread->join();
    delete publisherThread;
    sleep(1);
    cout << "- Step 7. Verify TEST_APP_DB content is empty" << endl;
    EXPECT_EQ(appTableA1.get(keyA1, values), false);
    EXPECT_EQ(appTableA2.get(keyA2, values), false);
    EXPECT_EQ(appTableB.get(keyB, values), false);

    subscriberThread->join();
    delete subscriberThread;
    cout << "Done." << endl;
}
