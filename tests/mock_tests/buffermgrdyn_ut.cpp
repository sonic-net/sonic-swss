#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#define private public
#include "buffermgrdyn.h"
#undef private
#include "warm_restart.h"

extern string gMySwitchType;


namespace buffermgrdyn_test
{
    using namespace std;

    shared_ptr<swss::DBConnector> m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    shared_ptr<swss::DBConnector> m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
    shared_ptr<swss::DBConnector> m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

    BufferMgrDynamic *m_dynamicBuffer;
    SelectableTimer m_selectableTable(timespec({ .tv_sec = BUFFERMGR_TIMER_PERIOD, .tv_nsec = 0 }), 0);
    Table portTable(m_config_db.get(), CFG_PORT_TABLE_NAME);
    Table cableLengthTable(m_config_db.get(), CFG_PORT_CABLE_LEN_TABLE_NAME);
    Table bufferPoolTable(m_config_db.get(), CFG_BUFFER_POOL_TABLE_NAME);
    Table bufferProfileTable(m_config_db.get(), CFG_BUFFER_PROFILE_TABLE_NAME);
    Table bufferPgTable(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME);
    Table bufferQueueTable(m_config_db.get(), CFG_BUFFER_QUEUE_TABLE_NAME);
    Table bufferIngProfileListTable(m_config_db.get(), CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
    Table bufferEgrProfileListTable(m_config_db.get(), CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME);
    Table defaultLosslessParameterTable(m_config_db.get(), CFG_DEFAULT_LOSSLESS_BUFFER_PARAMETER);
    Table appPortTable(m_app_db.get(), APP_PORT_TABLE_NAME);
    Table appBufferPoolTable(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
    Table appBufferProfileTable(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
    Table appBufferPgTable(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
    Table appBufferQueueTable(m_app_db.get(), APP_BUFFER_QUEUE_TABLE_NAME);
    Table appBufferIngProfileListTable(m_app_db.get(), APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
    Table appBufferEgrProfileListTable(m_app_db.get(), APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME);
    Table bufferMaxParamTable(m_state_db.get(), STATE_BUFFER_MAXIMUM_VALUE_TABLE);
    Table statePortTable(m_state_db.get(), STATE_PORT_TABLE_NAME);
    Table stateBufferTable(m_state_db.get(), STATE_BUFFER_MAXIMUM_VALUE_TABLE);

    map<string, vector<FieldValueTuple>> zeroProfileMap;
    vector<KeyOpFieldsValuesTuple> zeroProfile;

    struct BufferMgrDynTest : public ::testing::Test
    {
        map<string, vector<FieldValueTuple>> testBufferProfile;
        map<string, vector<FieldValueTuple>> testBufferPool;

        void SetUpReclaimingBuffer()
        {
            zeroProfileMap["ingress_zero_pool"] = {
                {"mode", "static"},
                {"type", "ingress"},
                {"size", "0"}
            };
            zeroProfileMap["ingress_lossy_pg_zero_profile"] = {
                {"pool", "ingress_zero_pool"},
                {"size", "0"},
                {"static_th", "0"}
            };
            zeroProfileMap["ingress_lossless_zero_profile"] = {
                {"pool", "ingress_lossless_pool"},
                {"size", "0"},
                {"dynamic_th", "-8"}
            };
            zeroProfileMap["egress_lossy_zero_profile"] = {
                {"pool", "egress_lossy_pool"},
                {"size", "0"},
                {"dynamic_th", "-8"}
            };
            zeroProfileMap["egress_lossless_zero_profile"] = {
                {"pool", "egress_lossless_pool"},
                {"size", "0"},
                {"dynamic_th", "-8"}
            };

            zeroProfile = {
                {
                    "BUFFER_POOL_TABLE:ingress_zero_pool",
                    "SET",
                    zeroProfileMap["ingress_zero_pool"]
                },
                {
                    "BUFFER_PROFILE_TABLE:ingress_lossy_pg_zero_profile",
                    "SET",
                    zeroProfileMap["ingress_lossy_pg_zero_profile"]
                },
                {
                    "BUFFER_PROFILE_TABLE:ingress_lossless_zero_profile",
                    "SET",
                    zeroProfileMap["ingress_lossless_zero_profile"]
                },
                {
                    "BUFFER_PROFILE_TABLE:egress_lossy_zero_profile",
                    "SET",
                    zeroProfileMap["egress_lossy_zero_profile"]
                },
                {
                    "BUFFER_PROFILE_TABLE:egress_lossless_zero_profile",
                    "SET",
                    zeroProfileMap["egress_lossless_zero_profile"]
                },
                {
                    "control_fields",
                    "SET",
                    {
                        {"pgs_to_apply_zero_profile", "0"},
                        {"ingress_zero_profile", "ingress_lossy_pg_zero_profile"}
                    }
                }
            };
        }

        BufferMgrDynTest()
        {
            testBufferPool["ingress_lossless_pool"] = {
                {"mode", "dynamic"},
                {"type", "ingress"},
                {"size", "1024000"}
            };
            testBufferPool["egress_lossless_pool"] = {
                {"mode", "dynamic"},
                {"type", "egress"},
                {"size", "1024000"}
            };
            testBufferPool["egress_lossy_pool"] = {
                {"mode", "dynamic"},
                {"type", "egress"},
                {"size", "1024000"}
            };

            testBufferProfile["ingress_lossless_profile"] = {
                {"dynamic_th", "7"},
                {"pool", "ingress_lossless_pool"},
                {"size", "0"}
            };
            testBufferProfile["egress_lossless_profile"] = {
                {"dynamic_th", "7"},
                {"pool", "egress_lossless_pool"},
                {"size", "0"}
            };
            testBufferProfile["egress_lossy_profile"] = {
                {"dynamic_th", "3"},
                {"pool", "egress_lossy_pool"},
                {"size", "0"}
            };
        }

        void SetUp() override
        {
            setenv("ASIC_VENDOR", "mock_test", 1);

            testing_db::reset();

            WarmStart::initialize("buffermgrd", "swss");
            WarmStart::checkWarmStart("buffermgrd", "swss");
        }

        void StartBufferManager(shared_ptr<vector<KeyOpFieldsValuesTuple>> zero_profile=nullptr)
        {
            // Init switch and create dependencies
            vector<TableConnector> buffer_table_connectors = {
                TableConnector(m_config_db.get(), CFG_PORT_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_PORT_CABLE_LEN_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_BUFFER_POOL_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_BUFFER_PROFILE_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_BUFFER_QUEUE_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME),
                TableConnector(m_config_db.get(), CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME),
                TableConnector(m_config_db.get(), CFG_DEFAULT_LOSSLESS_BUFFER_PARAMETER),
                TableConnector(m_state_db.get(), STATE_BUFFER_MAXIMUM_VALUE_TABLE),
                TableConnector(m_state_db.get(), STATE_PORT_TABLE_NAME)
            };

            m_dynamicBuffer = new BufferMgrDynamic(m_config_db.get(), m_state_db.get(), m_app_db.get(), buffer_table_connectors, nullptr, zero_profile);
        }

        void InitPort(const string &port="Ethernet0", const string &admin_status="up")
        {
            portTable.set(port,
                          {
                              {"speed", "100000"},
                              {"mtu", "9100"},
                              {"admin_status", admin_status}
                          });
            m_dynamicBuffer->addExistingData(&portTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void SetPortInitDone()
        {
            appPortTable.set("PortInitDone",
                             {
                                 {"lanes", "0"}
                             });
            m_dynamicBuffer->addExistingData(&appPortTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void InitMmuSize()
        {
            bufferMaxParamTable.set("global",
                                    {
                                        {"mmu_size", "1024000"}
                                    });
            if (m_dynamicBuffer)
                m_dynamicBuffer->addExistingData(&bufferMaxParamTable);
        }

        void InitDefaultLosslessParameter(const string &over_subscribe_ratio="")
        {
            if (over_subscribe_ratio.empty())
            {
                defaultLosslessParameterTable.set("AZURE",
                                                  {
                                                      {"default_dynamic_th", "0"}
                                                  });
            }
            else
            {
                defaultLosslessParameterTable.set("AZURE",
                                                  {
                                                      {"default_dynamic_th", "0"},
                                                      {"over_subscribe_ratio", over_subscribe_ratio}
                                                  });
            }
            if (m_dynamicBuffer)
            {
                m_dynamicBuffer->addExistingData(&defaultLosslessParameterTable);
                static_cast<Orch *>(m_dynamicBuffer)->doTask();
            }
        }

        void InitBufferPool()
        {
            for(auto &i: testBufferPool)
            {
                bufferPoolTable.set(i.first, i.second);
            }

            m_dynamicBuffer->addExistingData(&bufferPoolTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void ClearBufferPool(const string &skippedPool="", const string &clearPool="")
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            for (auto &i: testBufferPool)
            {
                if (skippedPool == i.first)
                    continue;
                if (!clearPool.empty() && clearPool != i.first)
                    continue;
                entries.push_back({i.first, "DEL", {}});
            }

            auto consumer = dynamic_cast<Consumer *>(m_dynamicBuffer->getExecutor(CFG_BUFFER_POOL_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void InitDefaultBufferProfile()
        {
            for (auto &i: testBufferProfile)
            {
                bufferProfileTable.set(i.first, i.second);
            }

            m_dynamicBuffer->addExistingData(&bufferProfileTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void ClearBufferProfile()
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            for (auto &i: testBufferProfile)
                entries.push_back({i.first, "DEL", {}});

            auto consumer = dynamic_cast<Consumer *>(m_dynamicBuffer->getExecutor(CFG_BUFFER_PROFILE_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void InitBufferPg(const string &key, const string &profile="NULL")
        {
            bufferPgTable.set(key,
                              {
                                  {"profile", profile}
                              });
            m_dynamicBuffer->addExistingData(&bufferPgTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void ClearBufferObject(const string &key, const string &tableName)
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({key, "DEL", {}});

            auto consumer = dynamic_cast<Consumer *>(m_dynamicBuffer->getExecutor(tableName));
            consumer->addToSync(entries);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();

            Table tableObject(m_config_db.get(), tableName);
            tableObject.del(key);
        }

        void InitBufferQueue(const string &key, const string &profile)
        {
            bufferQueueTable.set(key,
                                 {
                                     {"profile", profile}
                                 });
            m_dynamicBuffer->addExistingData(&bufferQueueTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void InitBufferProfileList(const string &ports, const string &profileList, Table &appDb)
        {
            appDb.set(ports,
                      {
                          {"profile_list", profileList}
                      });
            m_dynamicBuffer->addExistingData(&appDb);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void InitCableLength(const string &port, const string &length)
        {
            cableLengthTable.set("AZURE",
                                 {
                                     {port, length}
                                 });
            m_dynamicBuffer->addExistingData(&cableLengthTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void HandleTable(Table &table)
        {
            m_dynamicBuffer->addExistingData(&table);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
        }

        void CheckPool(buffer_pool_t &pool, const vector<FieldValueTuple> &tuples)
        {
            for (auto i : tuples)
            {
                if (fvField(i) == buffer_pool_type_field_name)
                {
                    if (fvValue(i) == buffer_value_ingress)
                        ASSERT_EQ(pool.direction, BUFFER_INGRESS);
                    else
                        ASSERT_EQ(pool.direction, BUFFER_EGRESS);
                }
                else if (fvField(i) == buffer_pool_mode_field_name)
                {
                    ASSERT_EQ(pool.mode, fvValue(i));
                }
                else if (fvField(i) == buffer_size_field_name)
                {
                    ASSERT_TRUE(!pool.dynamic_size);
                    ASSERT_EQ("1024000", fvValue(i));
                }
            }
        }

        void CheckProfile(buffer_profile_t &profile, const vector<FieldValueTuple> &tuples)
        {
            for (auto i : tuples)
            {
                if (fvField(i) == buffer_pool_field_name)
                {
                    ASSERT_EQ(profile.pool_name, fvValue(i));
                    if (strstr(profile.pool_name.c_str(), "ingress") != nullptr)
                        ASSERT_EQ(profile.direction, BUFFER_INGRESS);
                    else
                        ASSERT_EQ(profile.direction, BUFFER_EGRESS);
                }
                else if (fvField(i) == buffer_dynamic_th_field_name)
                {
                    ASSERT_EQ(profile.threshold_mode, buffer_dynamic_th_field_name);
                    ASSERT_EQ(profile.threshold, fvValue(i));
                }
                else if (fvField(i) == buffer_size_field_name)
                {
                    ASSERT_EQ(profile.size, fvValue(i));
                }
            }
        }

        void CheckPg(const string &port, const string &key, const string &expectedProfile="")
        {
            vector<FieldValueTuple> fieldValues;

            ASSERT_TRUE(m_dynamicBuffer->m_portPgLookup[port][key].dynamic_calculated);
            ASSERT_TRUE(m_dynamicBuffer->m_portPgLookup[port][key].lossless);

            auto existInDb = (!expectedProfile.empty());
            ASSERT_EQ(appBufferPgTable.get(key, fieldValues), existInDb);
            if (existInDb)
            {
                ASSERT_EQ(m_dynamicBuffer->m_portPgLookup[port][key].running_profile_name, expectedProfile);
                ASSERT_EQ(fvField(fieldValues[0]), "profile");
                ASSERT_EQ(fvValue(fieldValues[0]), expectedProfile);
            }
        }

        void CheckQueue(const string &port, const string &key, const string &expectedProfile, bool existInDb)
        {
            vector<FieldValueTuple> fieldValues;

            ASSERT_EQ(m_dynamicBuffer->m_portQueueLookup[port][key].running_profile_name, expectedProfile);
            ASSERT_EQ(appBufferQueueTable.get(key, fieldValues), existInDb);
            if (existInDb)
            {
                ASSERT_EQ(fvField(fieldValues[0]), "profile");
                ASSERT_EQ(fvValue(fieldValues[0]), expectedProfile);
            }
        }

        void CheckProfileList(const string &port, bool ingress, const string &profileList, bool existInDb=true)
        {
            vector<FieldValueTuple> fieldValues;

            auto direction = ingress ? BUFFER_INGRESS : BUFFER_EGRESS;
            ASSERT_EQ(m_dynamicBuffer->m_portProfileListLookups[direction][port], profileList);

            auto &appDb = ingress ? appBufferIngProfileListTable : appBufferEgrProfileListTable;

            ASSERT_EQ(appDb.get(port, fieldValues), existInDb);
            if (existInDb)
            {
                ASSERT_EQ(fieldValues.size(), 1);
                ASSERT_EQ(fvField(fieldValues[0]), "profile_list");
                ASSERT_EQ(fvValue(fieldValues[0]), profileList);
            }
        }

        void CheckIfVectorsMatch(const vector<FieldValueTuple> &vec1, const vector<FieldValueTuple> &vec2)
        {
            ASSERT_EQ(vec1.size(), vec2.size());
            for (auto &i : vec1)
            {
                bool found = false;
                for (auto &j : vec2)
                {
                    if (i == j)
                    {
                        found = true;
                        break;
                    }
                }
                ASSERT_TRUE(found);
            }
        }

        void TearDown() override
        {
            delete m_dynamicBuffer;
            m_dynamicBuffer = nullptr;

            unsetenv("ASIC_VENDOR");
        }
    };

    /*
     * Dependencies
     * 1. Buffer manager reads default lossless parameter and maximum mmu size at the beginning
     * 2. Maximum mmu size will be pushed ahead of PortInitDone
     * 3. Buffer pools can be ready at any time after PortInitDone
     * 4. Buffer tables can be applied in any order
     * 5. Port and buffer PG can be applied in any order
     * 6. Sequence after config qos clear
     */

    /*
     * Normal starting flow
     * 1. Start buffer manager with default lossless parameter and maximum mmu size
     * 2. PortInitDone
     * 3. Cable length and port configuration
     * 4. Buffer tables: BUFFER_POOL/BUFFER_PROFILE/BUFFER_PG
     * 5. Queue and buffer profile lists with/without port created
     */
    TEST_F(BufferMgrDynTest, BufferMgrTestNormalFlows)
    {
        vector<FieldValueTuple> fieldValues;
        vector<string> keys;

        // Prepare information that will be read at the beginning
        InitDefaultLosslessParameter();
        InitMmuSize();

        StartBufferManager();

        InitPort();
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, PORT_INITIALIZING);

        SetPortInitDone();
        // Timer will be called
        m_dynamicBuffer->doTask(m_selectableTable);

        ASSERT_EQ(m_dynamicBuffer->m_bufferPoolLookup.size(), 0);
        InitBufferPool();
        ASSERT_EQ(m_dynamicBuffer->m_bufferPoolLookup.size(), 3);
        appBufferPoolTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 3);
        for (auto i : testBufferPool)
        {
            CheckPool(m_dynamicBuffer->m_bufferPoolLookup[i.first], testBufferPool[i.first]);
            fieldValues.clear();
            appBufferPoolTable.get(i.first, fieldValues);
            CheckPool(m_dynamicBuffer->m_bufferPoolLookup[i.first], fieldValues);
        }

        InitDefaultBufferProfile();
        appBufferProfileTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 3);
        ASSERT_EQ(m_dynamicBuffer->m_bufferProfileLookup.size(), 3);
        for (auto i : testBufferProfile)
        {
            CheckProfile(m_dynamicBuffer->m_bufferProfileLookup[i.first], testBufferProfile[i.first]);
            fieldValues.clear();
            appBufferProfileTable.get(i.first, fieldValues);
            CheckProfile(m_dynamicBuffer->m_bufferProfileLookup[i.first], fieldValues);
        }

        InitCableLength("Ethernet0", "5m");
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, PORT_READY);

        InitBufferPg("Ethernet0|3-4");

        auto expectedProfile = "pg_lossless_100000_5m_profile";
        CheckPg("Ethernet0", "Ethernet0:3-4", expectedProfile);
        auto &portPgMap = m_dynamicBuffer->m_bufferProfileLookup[expectedProfile].port_pgs;
        ASSERT_EQ(portPgMap.size(), 1);
        ASSERT_TRUE(portPgMap.find("Ethernet0:3-4") != portPgMap.end());

        // Multiple port key
        InitBufferPg("Ethernet2,Ethernet4|3-4");

        CheckPg("Ethernet2", "Ethernet2:3-4");
        CheckPg("Ethernet4", "Ethernet4:3-4");

        // Buffer queue, ingress and egress profile list table
        InitPort("Ethernet2");
        InitPort("Ethernet4");

        InitBufferQueue("Ethernet2,Ethernet4,Ethernet6|3-4", "egress_lossless_profile");
        CheckQueue("Ethernet2", "Ethernet2:3-4", "egress_lossless_profile", true);
        CheckQueue("Ethernet4", "Ethernet4:3-4", "egress_lossless_profile", true);

        InitBufferProfileList("Ethernet2,Ethernet4,Ethernet6", "ingress_lossless_profile", bufferIngProfileListTable);
        CheckProfileList("Ethernet2", true, "ingress_lossless_profile");
        CheckProfileList("Ethernet4", true, "ingress_lossless_profile");

        InitBufferProfileList("Ethernet2,Ethernet4,Ethernet6", "egress_lossless_profile,egress_lossy_profile", bufferEgrProfileListTable);
        CheckProfileList("Ethernet2", false, "egress_lossless_profile,egress_lossy_profile");
        CheckProfileList("Ethernet4", false, "egress_lossless_profile,egress_lossy_profile");

        // Check whether queue, profile lists have been applied after port created
        InitPort("Ethernet6");
        CheckQueue("Ethernet6", "Ethernet6:3-4", "egress_lossless_profile", true);
        CheckProfileList("Ethernet6", true, "ingress_lossless_profile");
        CheckProfileList("Ethernet6", false, "egress_lossless_profile,egress_lossy_profile");
    }

    /*
     * Verify a buffer pool will not be created without corresponding item in BUFFER_POOL
     * otherwise it interferes starting flow
     * 1. Configure oversubscribe ratio
     * 2. Check whether ingress_lossless_pool is created
     */
    TEST_F(BufferMgrDynTest, BufferMgrTestNoPoolCreatedWithoutDb)
    {
        StartBufferManager();

        InitMmuSize();
        InitDefaultLosslessParameter("0");
        InitPort("Ethernet0");

        static_cast<Orch *>(m_dynamicBuffer)->doTask();
        m_dynamicBuffer->doTask(m_selectableTable);

        ASSERT_TRUE(m_dynamicBuffer->m_bufferPoolLookup.empty());

        InitBufferPool();
        static_cast<Orch *>(m_dynamicBuffer)->doTask();

        ASSERT_FALSE(m_dynamicBuffer->m_bufferPoolLookup.empty());
    }

    /*
     * Sad flows test. Order is reversed in the following cases:
     * - The buffer table creating. The tables referencing other tables are created first
     * - Buffer manager starts with neither default lossless parameter nor maximum mmu size available
     *
     * 1. Start buffer manager without default lossless parameter and maximum mmu size
     * 2. Buffer tables are applied in order:
     *    - Port configuration
     *    - BUFFER_QUEUE/buffer profile list
     *    - BUFFER_PG/BUFFER_PROFILE/BUFFER_POOL
     *    - PortInitDone
     * 3. Cable length
     * 4. Create a buffer profile with wrong threshold mode or direction
     *    and verify it will not be propagated to SAI
     */
    TEST_F(BufferMgrDynTest, BufferMgrTestSadFlows)
    {
        vector<string> ts;
        vector<FieldValueTuple> fieldValues;
        vector<string> keys;

        StartBufferManager();

        static_cast<Orch *>(m_dynamicBuffer)->doTask();

        InitPort();

        InitBufferPg("Ethernet0|3-4");
        // No item generated in BUFFER_PG_TABLE
        CheckPg("Ethernet0", "Ethernet0:3-4");

        InitBufferQueue("Ethernet0|3-4", "egress_lossless_profile");
        ASSERT_TRUE(m_dynamicBuffer->m_portQueueLookup["Ethernet0"]["Ethernet0:3-4"].running_profile_name.empty());

        InitBufferProfileList("Ethernet0", "ingress_lossless_profile", bufferIngProfileListTable);
        ASSERT_TRUE(m_dynamicBuffer->m_portProfileListLookups[BUFFER_INGRESS]["Ethernet0"].empty());

        InitBufferProfileList("Ethernet0", "egress_lossless_profile,egress_lossy_profile", bufferEgrProfileListTable);
        ASSERT_TRUE(m_dynamicBuffer->m_portProfileListLookups[BUFFER_EGRESS]["Ethernet0"].empty());

        InitDefaultBufferProfile();
        appBufferProfileTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 0);
        ASSERT_EQ(m_dynamicBuffer->m_bufferProfileLookup.size(), 0);

        ASSERT_EQ(m_dynamicBuffer->m_bufferPoolLookup.size(), 0);
        InitBufferPool();
        appBufferPoolTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 3);
        ASSERT_EQ(m_dynamicBuffer->m_bufferPoolLookup.size(), 3);
        ASSERT_EQ(m_dynamicBuffer->m_bufferProfileLookup.size(), 3);
        for (auto i : testBufferProfile)
        {
            CheckProfile(m_dynamicBuffer->m_bufferProfileLookup[i.first], testBufferProfile[i.first]);
            fieldValues.clear();
            appBufferProfileTable.get(i.first, fieldValues);
            CheckProfile(m_dynamicBuffer->m_bufferProfileLookup[i.first], fieldValues);
        }
        for (auto i : testBufferPool)
        {
            CheckPool(m_dynamicBuffer->m_bufferPoolLookup[i.first], testBufferPool[i.first]);
            fieldValues.clear();
            appBufferPoolTable.get(i.first, fieldValues);
            CheckPool(m_dynamicBuffer->m_bufferPoolLookup[i.first], fieldValues);
        }

        ASSERT_EQ(m_dynamicBuffer->m_portPgLookup.size(), 1);
        static_cast<Orch *>(m_dynamicBuffer)->doTask();
        CheckProfileList("Ethernet0", true, "ingress_lossless_profile", false);
        CheckProfileList("Ethernet0", false, "egress_lossless_profile,egress_lossy_profile", false);

        // All default buffer profiles should be generated and pushed into BUFFER_PROFILE_TABLE
        static_cast<Orch *>(m_dynamicBuffer)->doTask();

        InitMmuSize();
        SetPortInitDone();
        m_dynamicBuffer->doTask(m_selectableTable);

        InitDefaultLosslessParameter();
        m_dynamicBuffer->doTask(m_selectableTable);

        CheckPg("Ethernet0", "Ethernet0:3-4");
        InitCableLength("Ethernet0", "5m");
        auto expectedProfile = "pg_lossless_100000_5m_profile";
        CheckPg("Ethernet0", "Ethernet0:3-4", expectedProfile);
        CheckQueue("Ethernet0", "Ethernet0:3-4", "egress_lossless_profile", true);

        CheckProfileList("Ethernet0", true, "ingress_lossless_profile", true);
        CheckProfileList("Ethernet0", false, "egress_lossless_profile,egress_lossy_profile", true);

        InitPort("Ethernet4");
        InitPort("Ethernet6");
        InitBufferQueue("Ethernet6|0-2", "egress_lossy_profile");
        InitBufferProfileList("Ethernet6", "ingress_lossless_profile", bufferIngProfileListTable);

        // Buffer queue/PG/profile lists with wrong direction should not overwrite the existing ones
        vector<string> ingressProfiles = {"egress_lossy_profile", "ingress_profile", ""};
        vector<string> portsToTest = {"Ethernet0", "Ethernet4"};
        for (auto port : portsToTest)
        {
            for (auto ingressProfile : ingressProfiles)
            {
                InitBufferPg(port + "|3-4", ingressProfile);
                if (port == "Ethernet0")
                {
                    ASSERT_EQ(m_dynamicBuffer->m_portPgLookup["Ethernet0"]["Ethernet0:3-4"].running_profile_name, expectedProfile);
                    ASSERT_TRUE(appBufferPgTable.get("Ethernet0:3-4", fieldValues));
                    CheckIfVectorsMatch(fieldValues, {{"profile", expectedProfile}});
                }
                else
                {
                    ASSERT_TRUE(m_dynamicBuffer->m_portPgLookup[port].find(port + ":3-4") == m_dynamicBuffer->m_portPgLookup[port].end());
                    ASSERT_FALSE(appBufferPgTable.get(port + ":3-4", fieldValues));
                }
            }
        }

        InitBufferQueue("Ethernet4|0-2", "ingress_lossless_profile");
        ASSERT_TRUE(m_dynamicBuffer->m_portQueueLookup["Ethernet4"]["Ethernet0:0-2"].running_profile_name.empty());
        ASSERT_FALSE(appBufferQueueTable.get("Ethernet4:0-2", fieldValues));
        // No pending notifications
        ts.clear();
        m_dynamicBuffer->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 0);

        InitBufferQueue("Ethernet6|0-2", "ingress_lossless_profile");
        ASSERT_EQ(m_dynamicBuffer->m_portQueueLookup["Ethernet6"]["Ethernet6:0-2"].running_profile_name, "egress_lossy_profile");
        ASSERT_TRUE(appBufferQueueTable.get("Ethernet6:0-2", fieldValues));
        CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_profile"}});
        // No pending notifications
        m_dynamicBuffer->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 0);

        // Wrong direction
        InitBufferProfileList("Ethernet4", "egress_lossless_profile", bufferIngProfileListTable);
        ASSERT_TRUE(m_dynamicBuffer->m_portProfileListLookups[BUFFER_INGRESS]["Ethernet4"].empty());
        ASSERT_FALSE(appBufferIngProfileListTable.get("Ethernet4", fieldValues));
        // No pending notifications
        m_dynamicBuffer->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 0);

        InitBufferProfileList("Ethernet6", "egress_lossless_profile", bufferIngProfileListTable);
        ASSERT_EQ(m_dynamicBuffer->m_portProfileListLookups[BUFFER_INGRESS]["Ethernet6"], "ingress_lossless_profile");
        ASSERT_TRUE(appBufferIngProfileListTable.get("Ethernet6", fieldValues));
        CheckIfVectorsMatch(fieldValues, {{"profile_list", "ingress_lossless_profile"}});
        // No pending notifications
        m_dynamicBuffer->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 0);

        // Profile with wrong mode should not override the existing entries
        vector<string> wrong_profile_names = {"ingress_lossless_profile", "wrong_param_profile"};
        vector<vector<FieldValueTuple>> wrong_profile_patterns = {
            // wrong threshold mode
            {
                {"pool", "ingress_lossless_pool"},
                {"static_th", "100"},
                {"size", "0"}
            },
            // unconfigured pool
            {
                {"pool", "ingress_pool"},
                {"dynamic_th", "0"},
                {"size", "0"}
            }
        };
        auto expected_pending_tasks = 0;
        for (auto wrong_profile_name : wrong_profile_names)
        {
            bool exist = (testBufferProfile.find(wrong_profile_name) != testBufferProfile.end());
            for (auto wrong_profile_pattern : wrong_profile_patterns)
            {
                bufferProfileTable.set(wrong_profile_name, wrong_profile_pattern);
                m_dynamicBuffer->addExistingData(&bufferProfileTable);
                static_cast<Orch *>(m_dynamicBuffer)->doTask();
                if (exist)
                    CheckProfile(m_dynamicBuffer->m_bufferProfileLookup[wrong_profile_name], testBufferProfile[wrong_profile_name]);
                else
                    ASSERT_EQ(m_dynamicBuffer->m_bufferProfileLookup.find(wrong_profile_name), m_dynamicBuffer->m_bufferProfileLookup.end());
                ASSERT_EQ(appBufferProfileTable.get(wrong_profile_name, fieldValues), exist);
                // No pending notifications
                ts.clear();
                m_dynamicBuffer->dumpPendingTasks(ts);
                if (get<1>(wrong_profile_pattern[0]) == "ingress_pool")
                    expected_pending_tasks++;
                ASSERT_EQ(ts.size(), expected_pending_tasks);
            }
        }
    }

    /*
     * Clear qos with reclaiming buffer
     *
     * To test clear qos flow with reclaiming buffer.
     * 1. Init buffer manager as normal
     * 2. Configure buffer for 2 ports with admin status being up and down respectively
     * 3. Clear qos
     * 4. Check whether all the buffer items have been removed
     * 5. Repeat the flow from step 2 for two extra times:
     *    - Check whether buffer manager works correctly after clear qos
     *    - STATE_DB.BUFFER_MAX_PARAM is received before and after buffer items received
     */
    TEST_F(BufferMgrDynTest, BufferMgrTestClearQosReclaimingBuffer)
    {
        vector<FieldValueTuple> fieldValues;
        vector<string> keys;
        vector<string> skippedPools = {"", "ingress_lossless_pool", ""};
        int round = 0;

        SetUpReclaimingBuffer();
        shared_ptr<vector<KeyOpFieldsValuesTuple>> zero_profile = make_shared<vector<KeyOpFieldsValuesTuple>>(zeroProfile);

        InitDefaultLosslessParameter();
        InitMmuSize();

        StartBufferManager(zero_profile);

        statePortTable.set("Ethernet0",
                           {
                               {"supported_speeds", "100000,50000,40000,25000,10000,1000"}
                           });
        InitPort("Ethernet0", "down");
        InitPort("Ethernet4", "down");
        InitPort("Ethernet6", "down");
        InitPort("Ethernet8", "down");
        vector<string> adminDownPorts = {"Ethernet0", "Ethernet4", "Ethernet6"};
        vector<string> ports = {"Ethernet0", "Ethernet2", "Ethernet4", "Ethernet6"};
        InitPort("Ethernet2");
        InitCableLength("Ethernet2", "5m");
        auto expectedProfile = "pg_lossless_100000_5m_profile";
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, PORT_ADMIN_DOWN);

        SetPortInitDone();
        for(auto &skippedPool : skippedPools)
        {
            // Call timer
            m_dynamicBuffer->doTask(m_selectableTable);
            ASSERT_EQ(m_dynamicBuffer->m_bufferPoolLookup.size(), 0);
            InitBufferPool();
            ASSERT_EQ(m_dynamicBuffer->m_bufferPoolLookup.size(), 3);
            appBufferPoolTable.getKeys(keys);
            ASSERT_EQ(keys.size(), 3);
            for (auto i : testBufferPool)
            {
                CheckPool(m_dynamicBuffer->m_bufferPoolLookup[i.first], testBufferPool[i.first]);
                fieldValues.clear();
                appBufferPoolTable.get(i.first, fieldValues);
                CheckPool(m_dynamicBuffer->m_bufferPoolLookup[i.first], fieldValues);
            }

            InitDefaultBufferProfile();
            appBufferProfileTable.getKeys(keys);
            ASSERT_EQ(keys.size(), 3);
            ASSERT_EQ(m_dynamicBuffer->m_bufferProfileLookup.size(), 3);
            for (auto i : testBufferProfile)
            {
                CheckProfile(m_dynamicBuffer->m_bufferProfileLookup[i.first], testBufferProfile[i.first]);
                fieldValues.clear();
                appBufferProfileTable.get(i.first, fieldValues);
                CheckProfile(m_dynamicBuffer->m_bufferProfileLookup[i.first], fieldValues);
            }

            for (auto &adminDownPort : adminDownPorts)
            {
                InitBufferPg(adminDownPort + "|3-4", "NULL");
                InitBufferQueue(adminDownPort + "|3-4", "egress_lossless_profile");
                InitBufferQueue(adminDownPort + "|0-2", "egress_lossy_profile");
                InitBufferQueue(adminDownPort + "|5-6", "egress_lossy_profile");
            }
            InitBufferPg("Ethernet0|0", "ingress_lossy_profile");
            InitBufferPg("Ethernet0|3-4");
            InitBufferProfileList("Ethernet0", "ingress_lossless_profile", bufferIngProfileListTable);
            InitBufferProfileList("Ethernet0", "egress_lossless_profile,egress_lossy_profile", bufferEgrProfileListTable);

            // Init buffer items for a normal port and check APPL_DB
            InitBufferQueue("Ethernet2|3-4", "egress_lossless_profile");
            InitBufferQueue("Ethernet2|0-2", "egress_lossy_profile");
            InitBufferPg("Ethernet2|3-4");
            InitBufferProfileList("Ethernet2", "ingress_lossless_profile", bufferIngProfileListTable);
            InitBufferProfileList("Ethernet2", "egress_lossless_profile,egress_lossy_profile", bufferEgrProfileListTable);

            fieldValues.clear();
            ASSERT_TRUE(appBufferPgTable.get("Ethernet2:3-4", fieldValues));
            CheckIfVectorsMatch(fieldValues, {{"profile", expectedProfile}});
            fieldValues.clear();
            ASSERT_TRUE(appBufferQueueTable.get("Ethernet2:0-2", fieldValues));
            CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_profile"}});
            fieldValues.clear();
            ASSERT_TRUE(appBufferQueueTable.get("Ethernet2:3-4", fieldValues));
            CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossless_profile"}});
            fieldValues.clear();
            ASSERT_TRUE(appBufferIngProfileListTable.get("Ethernet2", fieldValues));
            CheckIfVectorsMatch(fieldValues, {{"profile_list", "ingress_lossless_profile"}});
            fieldValues.clear();
            ASSERT_TRUE(appBufferEgrProfileListTable.get("Ethernet2", fieldValues));
            CheckIfVectorsMatch(fieldValues, {{"profile_list", "egress_lossless_profile,egress_lossy_profile"}});

            // Buffer pools ready but the port is not ready to be reclaimed
            m_dynamicBuffer->doTask(m_selectableTable);

            // Push maximum buffer parameters for the port in order to make it ready to reclaim
            if (round == 0)
            {
                // To simulate different sequences
                // The 1st round: STATE_DB.PORT_TABLE is updated after buffer items ready
                // The 2nd, 3rd rounds: before

                for (auto &adminDownPort : adminDownPorts)
                {
                    stateBufferTable.set(adminDownPort,
                                         {
                                             {"max_priority_groups", "8"},
                                             {"max_queues", "16"}
                                         });
                }
                stateBufferTable.set("Ethernet8",
                                     {
                                         {"max_priority_groups", "8"},
                                         {"max_queues", "16"}
                                     });
                m_dynamicBuffer->addExistingData(&stateBufferTable);
                static_cast<Orch *>(m_dynamicBuffer)->doTask();
            }

            m_dynamicBuffer->doTask(m_selectableTable);

            // Check whether zero profiles and pool have been applied
            appBufferPoolTable.getKeys(keys);
            ASSERT_EQ(keys.size(), 4);
            for (auto key : keys)
            {
                if (testBufferPool.find(key) == testBufferPool.end())
                {
                    fieldValues.clear();
                    appBufferPoolTable.get(key, fieldValues);
                    CheckIfVectorsMatch(fieldValues, zeroProfileMap[key]);
                }
            }

            appBufferProfileTable.getKeys(keys);
            for (auto key : keys)
            {
                if (testBufferProfile.find(key) == testBufferProfile.end())
                {
                    fieldValues.clear();
                    appBufferProfileTable.get(key, fieldValues);
                    if (zeroProfileMap.find(key) == zeroProfileMap.end())
                        CheckIfVectorsMatch(fieldValues,
                                            {
                                                {"xon", ""},  // Due to the limitation of mock lua scricpt call,
                                                {"xoff", ""}, // we can not calculate the number
                                                {"size", ""}, // so expected value is the empty string
                                                {"pool", "ingress_lossless_pool"},
                                                {"dynamic_th", "0"}
                                            });
                    else
                        CheckIfVectorsMatch(fieldValues, zeroProfileMap[key]);
                }
            }

            for (auto &adminDownPort : adminDownPorts)
            {
                fieldValues.clear();
                ASSERT_TRUE(appBufferPgTable.get("Ethernet0:0", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "ingress_lossy_pg_zero_profile"}});
                ASSERT_FALSE(appBufferPgTable.get("Ethernet0:3-4", fieldValues));
                fieldValues.clear();
                ASSERT_TRUE(appBufferQueueTable.get(adminDownPort + ":0-2", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_zero_profile"}});
                fieldValues.clear();
                ASSERT_TRUE(appBufferQueueTable.get(adminDownPort + ":3-4", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossless_zero_profile"}});
                fieldValues.clear();
                ASSERT_TRUE(appBufferQueueTable.get(adminDownPort + ":5-6", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_zero_profile"}});
                fieldValues.clear();
            }
            ASSERT_TRUE(appBufferIngProfileListTable.get("Ethernet0", fieldValues));
            CheckIfVectorsMatch(fieldValues, {{"profile_list", "ingress_lossless_zero_profile"}});
            fieldValues.clear();
            ASSERT_TRUE(appBufferEgrProfileListTable.get("Ethernet0", fieldValues));
            CheckIfVectorsMatch(fieldValues, {{"profile_list", "egress_lossless_zero_profile,egress_lossy_zero_profile"}});

            // Configured but not applied items. There is an extra delay
            m_dynamicBuffer->m_waitApplyAdditionalZeroProfiles = 0;
            m_dynamicBuffer->doTask(m_selectableTable);
            for (auto &adminDownPort : adminDownPorts)
            {
                ASSERT_TRUE(appBufferQueueTable.get(adminDownPort + ":7-15", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_zero_profile"}});
                fieldValues.clear();
            }

            if (round == 0)
            {
                ASSERT_TRUE(appBufferQueueTable.get("Ethernet8:0-15", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_zero_profile"}});
                fieldValues.clear();
                ASSERT_TRUE(appBufferPgTable.get("Ethernet8:0", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "ingress_lossy_pg_zero_profile"}});
                fieldValues.clear();
                ClearBufferObject("Ethernet8", CFG_PORT_TABLE_NAME);
                ASSERT_FALSE(appBufferPgTable.get("Ethernet8:0", fieldValues));
                ASSERT_FALSE(appBufferQueueTable.get("Ethernet8:0-15", fieldValues));
            }

            ClearBufferObject("Ethernet0|3-4", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet4|5-6", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet4|0-2", CFG_BUFFER_QUEUE_TABLE_NAME);
            // Clear all qos tables
            ClearBufferPool(skippedPool);
            ClearBufferProfile();
            ClearBufferObject("Ethernet0|0", CFG_BUFFER_PG_TABLE_NAME);
            for (auto &adminDownPort : adminDownPorts)
            {
                ClearBufferObject(adminDownPort + "|3-4", CFG_BUFFER_PG_TABLE_NAME);
            }
            ClearBufferObject("Ethernet2|3-4", CFG_BUFFER_PG_TABLE_NAME);
            ClearBufferObject("Ethernet0|0-2", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet2|0-2", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet2|3-4", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet0|5-6", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet4|3-4", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet6|0-2", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet6|3-4", CFG_BUFFER_QUEUE_TABLE_NAME);
            ClearBufferObject("Ethernet6|5-6", CFG_BUFFER_QUEUE_TABLE_NAME);
            for (auto &port : ports)
            {
                ClearBufferObject(port, CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
                ClearBufferObject(port, CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME);
            }

            // Run timer
            m_dynamicBuffer->doTask(m_selectableTable);

            if (!skippedPool.empty())
            {
                // Clear the pool that was skipped in the previous step
                // This is to simulate the case where all the pools are not removed in one-shot
                ClearBufferPool("", skippedPool);
                m_dynamicBuffer->doTask(m_selectableTable);
            }

            // All internal data and APPL_DB has been cleared
            ASSERT_TRUE((appBufferPgTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferQueueTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferProfileTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferPoolTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferIngProfileListTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferEgrProfileListTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE(m_dynamicBuffer->m_bufferPoolLookup.empty());
            ASSERT_TRUE(m_dynamicBuffer->m_bufferProfileLookup.empty());
            ASSERT_TRUE(m_dynamicBuffer->m_portPgLookup.empty());
            ASSERT_TRUE(m_dynamicBuffer->m_portQueueLookup.empty());
            ASSERT_TRUE(m_dynamicBuffer->m_portProfileListLookups[BUFFER_EGRESS].empty());
            ASSERT_TRUE(m_dynamicBuffer->m_portProfileListLookups[BUFFER_INGRESS].empty());

            round++;
        }
    }


    /*
     * Clear qos with reclaiming buffer sad flows
     * Reclaiming buffer should be triggered via any single buffer item
     */
    TEST_F(BufferMgrDynTest, BufferMgrTestReclaimingBufferSadFlows)
    {
        vector<FieldValueTuple> fieldValues;
        vector<string> keys;
        vector<tuple<Table&, string, string, Table&, string, string>> bufferItems;

        bufferItems.emplace_back(bufferPgTable, "Ethernet0:0", "ingress_lossy_profile", appBufferPgTable, "profile", "ingress_lossy_pg_zero_profile");
        bufferItems.emplace_back(bufferPgTable, "Ethernet0:3-4", "NULL", appBufferPgTable, "", "");
        bufferItems.emplace_back(bufferQueueTable, "Ethernet0:0-2", "egress_lossy_profile", appBufferQueueTable, "profile", "egress_lossy_zero_profile");
        bufferItems.emplace_back(bufferQueueTable, "Ethernet0:3-4", "egress_lossless_profile", appBufferQueueTable, "profile", "egress_lossless_zero_profile");
        bufferItems.emplace_back(bufferIngProfileListTable, "Ethernet0", "ingress_lossless_profile", appBufferIngProfileListTable, "profile_list", "ingress_lossless_zero_profile");
        bufferItems.emplace_back(bufferEgrProfileListTable, "Ethernet0", "egress_lossless_profile,egress_lossy_profile", appBufferEgrProfileListTable, "profile_list", "egress_lossless_zero_profile,egress_lossy_zero_profile");

        SetUpReclaimingBuffer();
        shared_ptr<vector<KeyOpFieldsValuesTuple>> zero_profile = make_shared<vector<KeyOpFieldsValuesTuple>>(zeroProfile);

        InitDefaultLosslessParameter();
        InitMmuSize();

        StartBufferManager(zero_profile);

        stateBufferTable.set("Ethernet0",
                             {
                                 {"max_priority_groups", "8"},
                                 {"max_queues", "16"}
                             });
        m_dynamicBuffer->addExistingData(&stateBufferTable);
        static_cast<Orch *>(m_dynamicBuffer)->doTask();

        InitPort("Ethernet0", "down");

        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, PORT_ADMIN_DOWN);

        SetPortInitDone();
        m_dynamicBuffer->doTask(m_selectableTable);

        // After "config qos clear" the zero buffer profiles are unloaded
        m_dynamicBuffer->unloadZeroPoolAndProfiles();

        // Starts with empty buffer tables
        for(auto &bufferItem : bufferItems)
        {
            auto &cfgTable = get<0>(bufferItem);
            auto &key = get<1>(bufferItem);
            auto &profile = get<2>(bufferItem);
            auto &appTable = get<3>(bufferItem);
            auto &fieldName = get<4>(bufferItem);
            auto &expectedProfile = get<5>(bufferItem);

            cfgTable.set(key,
                         {
                             {fieldName, profile}
                         });
            m_dynamicBuffer->addExistingData(&cfgTable);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();

            ASSERT_FALSE(m_dynamicBuffer->m_bufferCompletelyInitialized);
            ASSERT_FALSE(m_dynamicBuffer->m_zeroProfilesLoaded);
            ASSERT_TRUE(m_dynamicBuffer->m_portInitDone);
            ASSERT_TRUE(m_dynamicBuffer->m_pendingApplyZeroProfilePorts.find("Ethernet0") != m_dynamicBuffer->m_pendingApplyZeroProfilePorts.end());

            InitBufferPool();
            InitDefaultBufferProfile();

            m_dynamicBuffer->doTask(m_selectableTable);

            // Another doTask to ensure all the dependent tables have been drained
            // after buffer pools and profiles have been drained
            static_cast<Orch *>(m_dynamicBuffer)->doTask();

            if (expectedProfile.empty())
            {
                ASSERT_FALSE(appTable.get(key, fieldValues));
            }
            else
            {
                ASSERT_TRUE(appTable.get(key, fieldValues));
                CheckIfVectorsMatch(fieldValues, {{fieldName, expectedProfile}});
            }

            m_dynamicBuffer->m_waitApplyAdditionalZeroProfiles = 0;
            m_dynamicBuffer->doTask(m_selectableTable);

            ASSERT_TRUE(m_dynamicBuffer->m_pendingApplyZeroProfilePorts.empty());
            ASSERT_TRUE(m_dynamicBuffer->m_bufferCompletelyInitialized);

            // Simulate clear qos
            ClearBufferPool();
            ClearBufferProfile();

            // Call timer
            m_dynamicBuffer->doTask(m_selectableTable);
        }
    }

    /*
     * Port removing flow
     */
    TEST_F(BufferMgrDynTest, BufferMgrTestRemovePort)
    {
        vector<FieldValueTuple> fieldValues;
        vector<string> keys;
        vector<string> statuses = {"up", "down"};

        // Prepare information that will be read at the beginning
        InitDefaultLosslessParameter();
        InitMmuSize();

        shared_ptr<vector<KeyOpFieldsValuesTuple>> zero_profile = make_shared<vector<KeyOpFieldsValuesTuple>>(zeroProfile);
        StartBufferManager(zero_profile);

        SetPortInitDone();
        // Timer will be called
        m_dynamicBuffer->doTask(m_selectableTable);

        InitBufferPool();
        appBufferPoolTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 3);
        InitDefaultBufferProfile();
        appBufferProfileTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 3);
        ASSERT_EQ(m_dynamicBuffer->m_bufferProfileLookup.size(), 3);

        m_dynamicBuffer->m_bufferCompletelyInitialized = true;
        m_dynamicBuffer->m_waitApplyAdditionalZeroProfiles = 0;
        InitCableLength("Ethernet0", "5m");

        for(auto status : statuses)
        {
            bool admin_up = (status == "up");

            InitPort("Ethernet0", status);
            ASSERT_TRUE(m_dynamicBuffer->m_portInfoLookup.find("Ethernet0") != m_dynamicBuffer->m_portInfoLookup.end());
            ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, admin_up ? PORT_READY : PORT_ADMIN_DOWN);

            // Init port buffer items
            InitBufferQueue("Ethernet0|3-4", "egress_lossless_profile");
            InitBufferProfileList("Ethernet0", "ingress_lossless_profile", bufferIngProfileListTable);
            InitBufferPg("Ethernet0|3-4");
            if (admin_up)
            {
                InitBufferProfileList("Ethernet0", "egress_lossless_profile,egress_lossy_profile", bufferEgrProfileListTable);

                auto expectedProfile = "pg_lossless_100000_5m_profile";
                CheckPg("Ethernet0", "Ethernet0:3-4", expectedProfile);
                CheckQueue("Ethernet0", "Ethernet0:3-4", "egress_lossless_profile", true);
                CheckProfileList("Ethernet0", true, "ingress_lossless_profile");
                CheckProfileList("Ethernet0", false, "egress_lossless_profile,egress_lossy_profile");
            }
            else
            {
                InitBufferPg("Ethernet0|0", "ingress_lossy_profile");

                stateBufferTable.set("Ethernet0",
                                     {
                                         {"max_priority_groups", "8"},
                                         {"max_queues", "16"}
                                     });
                m_dynamicBuffer->addExistingData(&stateBufferTable);
                static_cast<Orch *>(m_dynamicBuffer)->doTask();

                // Make sure profile list is applied after maximum buffer parameter table
                InitBufferProfileList("Ethernet0", "egress_lossless_profile,egress_lossy_profile", bufferEgrProfileListTable);

                fieldValues.clear();
                ASSERT_TRUE(appBufferPgTable.get("Ethernet0:0", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "ingress_lossy_pg_zero_profile"}});

                fieldValues.clear();
                ASSERT_TRUE(appBufferQueueTable.get("Ethernet0:3-4", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossless_zero_profile"}});

                fieldValues.clear();
                ASSERT_TRUE(appBufferQueueTable.get("Ethernet0:0-2", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_zero_profile"}});

                fieldValues.clear();
                ASSERT_TRUE(appBufferQueueTable.get("Ethernet0:5-15", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile", "egress_lossy_zero_profile"}});

                fieldValues.clear();
                ASSERT_TRUE(appBufferIngProfileListTable.get("Ethernet0", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile_list", "ingress_lossless_zero_profile"}});

                fieldValues.clear();
                ASSERT_TRUE(appBufferEgrProfileListTable.get("Ethernet0", fieldValues));
                CheckIfVectorsMatch(fieldValues, {{"profile_list", "egress_lossless_zero_profile,egress_lossy_zero_profile"}});

                ClearBufferObject("Ethernet0|0", CFG_BUFFER_PG_TABLE_NAME);
            }

            // Remove port
            ClearBufferObject("Ethernet0", CFG_PORT_TABLE_NAME);
            ASSERT_FALSE(m_dynamicBuffer->m_portPgLookup.empty());
            ClearBufferObject("Ethernet0", CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
            ClearBufferObject("Ethernet0", CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME);
            ClearBufferObject("Ethernet0|3-4", CFG_BUFFER_PG_TABLE_NAME);
            ClearBufferObject("Ethernet0|3-4", CFG_BUFFER_QUEUE_TABLE_NAME);
            static_cast<Orch *>(m_dynamicBuffer)->doTask();
            ASSERT_TRUE(m_dynamicBuffer->m_portPgLookup.empty());
            ASSERT_TRUE(m_dynamicBuffer->m_portQueueLookup.empty());
            ASSERT_TRUE(m_dynamicBuffer->m_portProfileListLookups[BUFFER_INGRESS].empty());
            ASSERT_TRUE(m_dynamicBuffer->m_portProfileListLookups[BUFFER_EGRESS].empty());
            ASSERT_TRUE((appBufferPgTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferQueueTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferIngProfileListTable.getKeys(keys), keys.empty()));
            ASSERT_TRUE((appBufferEgrProfileListTable.getKeys(keys), keys.empty()));
        }
    }

    /*
     * Port configuration flow
     * Port table items are received in different order
     */
    TEST_F(BufferMgrDynTest, BufferMgrTestPortConfigFlow)
    {
        // Prepare information that will be read at the beginning
        StartBufferManager();

        /*
         * Speed, admin up, cable length
         */
        portTable.set("Ethernet0",
                      {
                          {"speed", "100000"}
                      });
        HandleTable(portTable);
        ASSERT_TRUE(m_dynamicBuffer->m_portInfoLookup.find("Ethernet0") != m_dynamicBuffer->m_portInfoLookup.end());
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, PORT_ADMIN_DOWN);

        portTable.set("Ethernet0",
                      {
                          {"speed", "100000"},
                          {"admin_status", "up"}
                      });
        HandleTable(portTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, PORT_INITIALIZING);

        cableLengthTable.set("AZURE",
                             {
                                 {"Ethernet0", "5m"}
                             });
        HandleTable(cableLengthTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet0"].state, PORT_READY);

        /*
         * Speed, admin down, cable length, admin up
         */
        portTable.set("Ethernet4",
                      {
                          {"speed", "100000"},
                          {"admin_status", "down"}
                      });
        HandleTable(portTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet4"].state, PORT_ADMIN_DOWN);
        cableLengthTable.set("AZURE",
                             {
                                 {"Ethernet4", "5m"}
                             });
        HandleTable(cableLengthTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet4"].state, PORT_ADMIN_DOWN);
        portTable.set("Ethernet4",
                      {
                          {"speed", "100000"},
                          {"admin_status", "up"}
                      });
        HandleTable(portTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet4"].state, PORT_READY);

        /*
         * Auto-negotiation: supported speeds received after port table
         */
        portTable.set("Ethernet8",
                      {
                          {"speed", "100000"},
                          {"admin_status", "up"},
                          {"autoneg", "on"}
                      });
        HandleTable(portTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet8"].state, PORT_INITIALIZING);
        ASSERT_TRUE(m_dynamicBuffer->m_portInfoLookup["Ethernet8"].effective_speed.empty());

        cableLengthTable.set("AZURE",
                             {
                                 {"Ethernet8", "5m"}
                             });
        HandleTable(cableLengthTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet8"].state, PORT_INITIALIZING);

        statePortTable.set("Ethernet8",
                           {
                               {"supported_speeds", "100000,50000,40000,25000,10000,1000"}
                           });
        HandleTable(statePortTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet8"].effective_speed, "100000");
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet8"].state, PORT_READY);

        /*
         * Auto-negotiation: supported speeds received before port table
         */
        statePortTable.set("Ethernet12",
                           {
                               {"supported_speeds", "100000,50000,40000,25000,10000,1000"}
                           });
        HandleTable(statePortTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet12"].supported_speeds, "100000,50000,40000,25000,10000,1000");

        portTable.set("Ethernet12",
                      {
                          {"speed", "100000"},
                          {"admin_status", "up"},
                          {"autoneg", "on"}
                      });
        HandleTable(portTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet12"].state, PORT_INITIALIZING);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet12"].effective_speed, "100000");

        cableLengthTable.set("AZURE",
                             {
                                 {"Ethernet12", "5m"}
                             });
        HandleTable(cableLengthTable);
        ASSERT_EQ(m_dynamicBuffer->m_portInfoLookup["Ethernet12"].state, PORT_READY);
    }
}
