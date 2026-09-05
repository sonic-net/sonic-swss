#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mirrororch_sample_port_sai_wrap.h"

using namespace swss;

namespace sflow_test
{
    sai_status_t failSaiCreate(sai_object_id_t *, sai_object_id_t, uint32_t,
                               const sai_attribute_t *)
    {
        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    sai_status_t failSaiSetSwitchAttribute(sai_object_id_t, const sai_attribute_t *)
    {
        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    sai_status_t failSaiRemove(sai_object_id_t)
    {
        return SAI_STATUS_FAILURE;
    }

    template <typename SaiApi>
    class ScopedSaiApiOverride final
    {
    public:
        explicit ScopedSaiApiOverride(SaiApi *&target) : target(target), original(target), replacement(*target)
        {
            this->target = &replacement;
        }

        ~ScopedSaiApiOverride()
        {
            target = original;
        }

        ScopedSaiApiOverride(const ScopedSaiApiOverride &) = delete;
        ScopedSaiApiOverride &operator=(const ScopedSaiApiOverride &) = delete;

        SaiApi &api()
        {
            return replacement;
        }

    private:
        SaiApi *&target;
        SaiApi *original;
        SaiApi replacement;
    };

    // Identifies an object created or removed during MOD initialization.
    // Declared in initializeDropMonitor() create order; cleanup removes them in reverse.
    enum class ObjectType
    {
        TamReport,
        TamEventAction,
        TamTransport,
        Policer,
        HostifTrapGroup,
        HostifUserDefinedTrap,
        TamCollector,
        TamEvent,
        Tam,
    };

    // Size of the ObjectType-indexed callback tables; Tam must stay the last enumerator.
    constexpr size_t kObjectTypeCount = static_cast<size_t>(ObjectType::Tam) + 1;

    // Created: the create call succeeded. Destroyed: a later remove call also succeeded.
    enum class ObjectStatus
    {
        Created,
        Destroyed,
    };

    // Stores the final status of an object successfully created during initialization.
    struct TrackedObject
    {
        ObjectType type;
        ObjectStatus status;

        bool operator==(const TrackedObject &other) const
        {
            return type == other.type && status == other.status;
        }
    };

    // Tracks successfully created objects and updates their status after successful cleanup.
    class ObjectTracker final
    {
    public:
        using CreateFn = sai_status_t (*)(sai_object_id_t *, sai_object_id_t, uint32_t,
                                          const sai_attribute_t *);
        using RemoveFn = sai_status_t (*)(sai_object_id_t);

        // Remembers the callbacks that the wrappers of one object type delegate to.
        static void save(ObjectType type, CreateFn create, RemoveFn remove)
        {
            auto &state = getState();
            state.creates.at(index(type)) = create;
            state.removes.at(index(type)) = remove;
        }

        static void reset()
        {
            getState() = {};
        }
        static const std::vector<TrackedObject> &objects()
        {
            return getState().objects;
        }

        // One distinct wrapper per object type; the template argument carries the tag that a
        // C function pointer cannot capture.
        template <ObjectType Type>
        static sai_status_t create(sai_object_id_t *oid, sai_object_id_t switch_id,
                                   uint32_t attr_count, const sai_attribute_t *attr_list)
        {
            return createAndTrack(Type, oid, switch_id, attr_count, attr_list);
        }
        template <ObjectType Type>
        static sai_status_t remove(sai_object_id_t oid)
        {
            return removeAndTrack(Type, oid);
        }

    private:
        struct State
        {
            std::array<CreateFn, kObjectTypeCount> creates = {};
            std::array<RemoveFn, kObjectTypeCount> removes = {};
            std::vector<TrackedObject> objects;
            std::map<sai_object_id_t, size_t> object_indices;
        };

        static State &getState()
        {
            static State state;
            return state;
        }
        static constexpr size_t index(ObjectType type)
        {
            return static_cast<size_t>(type);
        }
        static sai_status_t createAndTrack(ObjectType type, sai_object_id_t *oid, sai_object_id_t switch_id, uint32_t attr_count, const sai_attribute_t *attr_list)
        {
            const auto status = getState().creates.at(index(type))(oid, switch_id, attr_count, attr_list);
            if (status == SAI_STATUS_SUCCESS)
            {
                auto &state = getState();
                state.object_indices[*oid] = state.objects.size();
                state.objects.push_back({ type, ObjectStatus::Created });
            }
            return status;
        }
        static sai_status_t removeAndTrack(ObjectType type, sai_object_id_t oid)
        {
            const auto status = getState().removes.at(index(type))(oid);
            if (status == SAI_STATUS_SUCCESS)
            {
                auto &state = getState();
                const auto object = state.object_indices.find(oid);
                if (object != state.object_indices.end())
                    state.objects[object->second].status = ObjectStatus::Destroyed;
            }
            return status;
        }
    };

    // Redirects the create and remove callbacks of the copied API tables to ObjectTracker.
    // Construct it after any injected failure so the tracker also wraps that callback.
    class ScopedObjectTracker final
    {
    public:
        ScopedObjectTracker(sai_tam_api_t &tam, sai_hostif_api_t &hostif, sai_policer_api_t &policer)
        {
            ObjectTracker::reset();
            install<ObjectType::TamReport>(tam.create_tam_report, tam.remove_tam_report);
            install<ObjectType::TamEventAction>(tam.create_tam_event_action, tam.remove_tam_event_action);
            install<ObjectType::TamTransport>(tam.create_tam_transport, tam.remove_tam_transport);
            install<ObjectType::Policer>(policer.create_policer, policer.remove_policer);
            install<ObjectType::HostifTrapGroup>(hostif.create_hostif_trap_group, hostif.remove_hostif_trap_group);
            install<ObjectType::HostifUserDefinedTrap>(hostif.create_hostif_user_defined_trap, hostif.remove_hostif_user_defined_trap);
            install<ObjectType::TamCollector>(tam.create_tam_collector, tam.remove_tam_collector);
            install<ObjectType::TamEvent>(tam.create_tam_event, tam.remove_tam_event);
            install<ObjectType::Tam>(tam.create_tam, tam.remove_tam);
        }
        ~ScopedObjectTracker()
        {
            ObjectTracker::reset();
        }
        ScopedObjectTracker(const ScopedObjectTracker &) = delete;
        ScopedObjectTracker &operator=(const ScopedObjectTracker &) = delete;

    private:
        // Saves the original callbacks of one object type, then points both slots at its wrappers.
        template <ObjectType Type>
        static void install(ObjectTracker::CreateFn &create_slot, ObjectTracker::RemoveFn &remove_slot)
        {
            ObjectTracker::save(Type, create_slot, remove_slot);
            create_slot = ObjectTracker::create<Type>;
            remove_slot = ObjectTracker::remove<Type>;
        }
    };

    class MockSflowOrch final
    {
    public:
        MockSflowOrch()
        {
            this->appDb = std::make_shared<DBConnector>("APPL_DB", 0);
            std::vector<string> sflow_tables = {
                APP_SFLOW_TABLE_NAME,
                APP_SFLOW_SESSION_TABLE_NAME,
                APP_SFLOW_SAMPLE_RATE_TABLE_NAME
            };
            sflowOrch = std::make_shared<SflowOrch>(this->appDb.get(),  sflow_tables);
        }
        ~MockSflowOrch() = default;

        void doSflowTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_SFLOW_TABLE_NAME, 1, 1),
                this->sflowOrch.get(), APP_SFLOW_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(this->sflowOrch.get())->doTask(*consumer);
        }

        void doSflowSessionTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_SFLOW_SESSION_TABLE_NAME, 1, 1),
                this->sflowOrch.get(), APP_SFLOW_SESSION_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(this->sflowOrch.get())->doTask(*consumer);
        }

        void doSflowSampleTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_SFLOW_SAMPLE_RATE_TABLE_NAME, 1, 1),
                this->sflowOrch.get(), APP_SFLOW_SAMPLE_RATE_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(this->sflowOrch.get())->doTask(*consumer);
        }

        SflowOrch& get()
        {
            return *sflowOrch;
        }

    private:
        std::shared_ptr<SflowOrch> sflowOrch;
        std::shared_ptr<DBConnector> appDb;
    };

    void setDropMonitorLimit(MockSflowOrch &mock_orch, const string &limit)
    {
        auto table = deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "global",
                    SET_COMMAND,
                    {
                        {"admin_state", "up"},
                        {"drop_monitor_limit", limit}
                    }
                }
            });
        mock_orch.doSflowTableTask(table);
    }

    void setSflowGlobal(MockSflowOrch &mock_orch, const vector<FieldValueTuple> &fvs)
    {
        auto table = deque<KeyOpFieldsValuesTuple>({ { "global", SET_COMMAND, fvs } });
        mock_orch.doSflowTableTask(table);
    }

    class SflowOrchTest : public ::testing::Test
    {
    public:
        SflowOrchTest()
        {
            this->initDb();
        }
        virtual ~SflowOrchTest() = default;

        void SetUp() override
        {
            this->initSaiApi();
            this->initSwitch();
            this->initOrch();
            this->initPorts();
        }

        void TearDown() override
        {
            this->deinitOrch();
            this->deinitSwitch();
            this->deinitSaiApi();
        }

    private:
        void initSaiApi()
        {
            std::map<std::string, std::string> profileMap = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"        }
            };
            auto status = ut_helper::initSaiApi(profileMap);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void deinitSaiApi()
        {
            auto status = ut_helper::uninitSaiApi();
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void initSwitch()
        {
            sai_status_t status;
            sai_attribute_t attr;

            // Create switch
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            // Get switch default virtual router ID
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gVirtualRouterId = attr.value.oid;
        }

        void deinitSwitch()
        {
            // Remove switch
            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gSwitchId = SAI_NULL_OBJECT_ID;
            gVirtualRouterId = SAI_NULL_OBJECT_ID;
        }

        void initOrch()
        {
            //
            // SwitchOrch
            //

            TableConnector switchCapTableStateDb(this->stateDb.get(), "SWITCH_CAPABILITY");
            TableConnector asicSensorsTableCfgDb(this->configDb.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector switchTableAppDb(this->appDb.get(), APP_SWITCH_TABLE_NAME);

            std::vector<TableConnector> switchTableList = {
                asicSensorsTableCfgDb,
                switchTableAppDb
            };

            gSwitchOrch = new SwitchOrch(this->appDb.get(), switchTableList, switchCapTableStateDb);
            gDirectory.set(gSwitchOrch);
            resourcesList.push_back(gSwitchOrch);

            //
            // PortsOrch
            //

            const int portsorchBasePri = 40;

            std::vector<table_name_with_pri_t> portTableList = {
                { APP_PORT_TABLE_NAME,        portsorchBasePri + 5 },
                { APP_VLAN_TABLE_NAME,        portsorchBasePri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorchBasePri     },
                { APP_LAG_TABLE_NAME,         portsorchBasePri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME,  portsorchBasePri     }
            };

            gPortsOrch = new PortsOrch(this->appDb.get(), this->stateDb.get(), portTableList, this->chassisAppDb.get());
            gDirectory.set(gPortsOrch);
            resourcesList.push_back(gPortsOrch);

            //
            // QosOrch
            //

            std::vector<std::string> qosTableList = {
                CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
                CFG_SCHEDULER_TABLE_NAME,
                CFG_DSCP_TO_TC_MAP_TABLE_NAME,
                CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME,
                CFG_DOT1P_TO_TC_MAP_TABLE_NAME,
                CFG_QUEUE_TABLE_NAME,
                CFG_PORT_QOS_MAP_TABLE_NAME,
                CFG_WRED_PROFILE_TABLE_NAME,
                CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME,
                CFG_DSCP_TO_FC_MAP_TABLE_NAME,
                CFG_EXP_TO_FC_MAP_TABLE_NAME
            };
            gQosOrch = new QosOrch(this->configDb.get(), qosTableList);
            gDirectory.set(gQosOrch);
            resourcesList.push_back(gQosOrch);

            //
            // BufferOrch
            //

            std::vector<std::string> bufferTableList = {
                APP_BUFFER_POOL_TABLE_NAME,
                APP_BUFFER_PROFILE_TABLE_NAME,
                APP_BUFFER_QUEUE_TABLE_NAME,
                APP_BUFFER_PG_TABLE_NAME,
                APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
            };
            gBufferOrch = new BufferOrch(this->appDb.get(), this->configDb.get(), this->stateDb.get(), bufferTableList);
            gDirectory.set(gBufferOrch);
            resourcesList.push_back(gBufferOrch);

            //
            // FlexCounterOrch
            //

            std::vector<std::string> flexCounterTableList = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };

            auto flexCounterOrch = new FlexCounterOrch(this->configDb.get(), flexCounterTableList);
            gDirectory.set(flexCounterOrch);
            resourcesList.push_back(flexCounterOrch);
        }

        void deinitOrch()
        {
            std::reverse(this->resourcesList.begin(), this->resourcesList.end());
            for (auto &it : this->resourcesList)
            {
                delete it;
            }

            gSwitchOrch = nullptr;
            gPortsOrch = nullptr;
            gQosOrch = nullptr;
            gBufferOrch = nullptr;

            Portal::DirectoryInternal::clear(gDirectory);
            EXPECT_TRUE(Portal::DirectoryInternal::empty(gDirectory));
        }

        void initPorts()
        {
            auto portTable = Table(this->appDb.get(), APP_PORT_TABLE_NAME);

            // Get SAI default ports to populate DB
            auto ports = ut_helper::getInitialSaiPorts();

            // Populate port table with SAI ports
            for (const auto &cit : ports)
            {
                portTable.set(cit.first, cit.second);
            }

            // Set PortConfigDone
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();

            // Set PortInitDone
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();
        }

        void initDb()
        {
            this->appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            this->configDb = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            this->stateDb = std::make_shared<swss::DBConnector>("STATE_DB", 0);
            this->chassisAppDb = std::make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
        }

        std::shared_ptr<DBConnector> appDb;
        std::shared_ptr<DBConnector> configDb;
        std::shared_ptr<DBConnector> stateDb;
        std::shared_ptr<DBConnector> chassisAppDb;

        std::vector<Orch*> resourcesList;
    };

    /* Test enabling/disabling SFLOW */
    TEST_F(SflowOrchTest, SflowEnableDisable)
    {
        MockSflowOrch mock_orch;
        {
            auto table1 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "down"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table1);

            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
        {
            auto table2 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table2);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
    }

    /* Test create/delete SFLOW */
    TEST_F(SflowOrchTest, SflowCreateDelete)
    {
        MockSflowOrch mock_orch;
        {
            auto table3 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                        }
                    }
                });
            mock_orch.doSflowTableTask(table3);
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
        {
            auto table4 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        DEL_COMMAND,
                        {
                            {"admin_state", "up"},
                        }
                    }
                });
            mock_orch.doSflowTableTask(table4);
            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
    }

    /* Test enabling/disabling SFLOW drop monitor */
    TEST_F(SflowOrchTest, SflowDropMonitorEnableDisable)
    {
        // SFLOW drop monitor only enable when SFLOW is enabled
        MockSflowOrch mock_orch;
        {
            auto table1 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "down"},
                            {"drop_monitor_limit", "100"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table1);

            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        }
        {
            auto table2 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "100"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table2);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        }
        // Disable SFLOW drop monitor by setting rate limit to 0
        {
            auto table3 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "0"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table3);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        }
    }

    /* Test changing SFLOW drop monitor limit rate */
    TEST_F(SflowOrchTest, SflowDropMonitorChangeLimitRate)
    {
        MockSflowOrch mock_orch;
        {
            auto table1 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "100"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table1);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
            ASSERT_EQ(Portal::SflowOrchInternal::getSflowDropMonitorLimitRate(mock_orch.get()), 100);
        }
        {
            auto table2 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "200"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table2);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
            ASSERT_EQ(Portal::SflowOrchInternal::getSflowDropMonitorLimitRate(mock_orch.get()), 200);
        }
    }

    void expectPolicerRate(sai_object_id_t policer, uint64_t cir, uint64_t cbs)
    {
        sai_attribute_t attrs[2] = {};
        attrs[0].id = SAI_POLICER_ATTR_CIR;
        attrs[1].id = SAI_POLICER_ATTR_CBS;
        ASSERT_EQ(sai_policer_api->get_policer_attribute(policer, 2, attrs), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attrs[0].value.u64, cir);
        EXPECT_EQ(attrs[1].value.u64, cbs);
    }

    /* Test: rate change updates CIR/CBS on the same policer without recreating it. */
    TEST_F(SflowOrchTest, SflowDropMonitorRateUpdateInPlace)
    {
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        auto &monitor = mock_orch.get().m_sflowDropMonitor;
        ASSERT_TRUE(monitor.isEnabled());
        const auto policer = monitor.m_policer;
        const auto tam = monitor.m_tam;
        expectPolicerRate(policer, 100, 100);

        setDropMonitorLimit(mock_orch, "200");
        EXPECT_TRUE(monitor.isEnabled());
        EXPECT_EQ(monitor.getLimitRate(), 200);
        EXPECT_EQ(monitor.m_policer, policer);
        EXPECT_EQ(monitor.m_tam, tam);
        expectPolicerRate(policer, 200, 200);

        setDropMonitorLimit(mock_orch, "0");
        EXPECT_FALSE(monitor.isEnabled());
    }

    /* Test: a set_policer_attribute failure reports failure and leaves cached state and MOD intact. */
    TEST_F(SflowOrchTest, SflowDropMonitorPolicerSetFailure)
    {
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        auto &monitor = mock_orch.get().m_sflowDropMonitor;
        ASSERT_TRUE(monitor.isEnabled());
        const auto policer = monitor.m_policer;
        const auto original_set = policer_api_override.api().set_policer_attribute;
        policer_api_override.api().set_policer_attribute =
            [](sai_object_id_t, const sai_attribute_t *) -> sai_status_t { return SAI_STATUS_FAILURE; };

        EXPECT_FALSE(monitor.enableDropMonitor(200));
        EXPECT_TRUE(monitor.isEnabled());
        EXPECT_EQ(monitor.getLimitRate(), 100);
        EXPECT_EQ(monitor.m_policer, policer);
        expectPolicerRate(policer, 100, 100);

        // Once SAI accepts the update again, the next request applies both attributes.
        policer_api_override.api().set_policer_attribute = original_set;
        setDropMonitorLimit(mock_orch, "200");
        EXPECT_EQ(monitor.getLimitRate(), 200);
        expectPolicerRate(policer, 200, 200);
        setDropMonitorLimit(mock_orch, "0");
    }

    /* Test: CIR failure after CBS success keeps the cached rate; a different limit repairs both. */
    TEST_F(SflowOrchTest, SflowDropMonitorCirSetFailure)
    {
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        auto &monitor = mock_orch.get().m_sflowDropMonitor;
        ASSERT_TRUE(monitor.isEnabled());
        const auto policer = monitor.m_policer;
        static sai_set_policer_attribute_fn original_set;
        original_set = policer_api_override.api().set_policer_attribute;
        policer_api_override.api().set_policer_attribute =
            [](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
                return attr->id == SAI_POLICER_ATTR_CIR ? SAI_STATUS_FAILURE : original_set(oid, attr);
            };

        EXPECT_FALSE(monitor.enableDropMonitor(200));
        EXPECT_TRUE(monitor.isEnabled());
        EXPECT_EQ(monitor.getLimitRate(), 100);
        EXPECT_EQ(monitor.m_policer, policer);
        // CBS was applied before CIR failed; no rollback is attempted.
        expectPolicerRate(policer, 100, 200);

        policer_api_override.api().set_policer_attribute = original_set;
        setDropMonitorLimit(mock_orch, "300");
        EXPECT_EQ(monitor.getLimitRate(), 300);
        expectPolicerRate(policer, 300, 300);
        setDropMonitorLimit(mock_orch, "0");
    }

    /* Test: getDropMonitorCpuQueue fallback when config file not found */
    TEST_F(SflowOrchTest, SflowDropMonitorCpuQueueFileNotFound)
    {
        MockSflowOrch mock_orch;
        uint32_t queue = Portal::SflowOrchInternal::getSflowDropMonitorCpuQueue(
            mock_orch.get(), "./nonexistent_sflow_mod.json");
        ASSERT_EQ(queue, 47);
    }

    /* Test: getDropMonitorCpuQueue reads valid config file */
    TEST_F(SflowOrchTest, SflowDropMonitorCpuQueueFromFile)
    {
        MockSflowOrch mock_orch;
        uint32_t queue = Portal::SflowOrchInternal::getSflowDropMonitorCpuQueue(
            mock_orch.get(), "./sflow_mod_valid.json");
        ASSERT_EQ(queue, 99);
    }

    /* Test: getDropMonitorCpuQueue fallback when config value is invalid type */
    TEST_F(SflowOrchTest, SflowDropMonitorCpuQueueInvalidValue)
    {
        MockSflowOrch mock_orch;
        uint32_t queue = Portal::SflowOrchInternal::getSflowDropMonitorCpuQueue(
            mock_orch.get(), "./sflow_mod_invalid.json");
        ASSERT_EQ(queue, 47);
    }

    /* Test: TAM report create failure keeps MOD disabled with no object to remove. */
    TEST_F(SflowOrchTest, SflowDropMonitorTamReportCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        tam_api_override.api().create_tam_report = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    /* Test: TAM create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorTamCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_tam() after its predecessor objects are created.
        tam_api_override.api().create_tam = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
            { ObjectType::TamTransport, ObjectStatus::Destroyed },
            { ObjectType::Policer, ObjectStatus::Destroyed },
            { ObjectType::HostifTrapGroup, ObjectStatus::Destroyed },
            { ObjectType::HostifUserDefinedTrap, ObjectStatus::Destroyed },
            { ObjectType::TamCollector, ObjectStatus::Destroyed },
            { ObjectType::TamEvent, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: HOSTIF trap group create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorTrapGroupCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_hostif_trap_group() after its predecessor objects are created.
        hostif_api_override.api().create_hostif_trap_group = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
            { ObjectType::TamTransport, ObjectStatus::Destroyed },
            { ObjectType::Policer, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: TAM event action create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorTamEventActionCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_tam_event_action() after its predecessor objects are created.
        tam_api_override.api().create_tam_event_action = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: TAM transport create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorTamTransportCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_tam_transport() after its predecessor objects are created.
        tam_api_override.api().create_tam_transport = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: policer create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorPolicerCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_policer() after its predecessor objects are created.
        policer_api_override.api().create_policer = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
            { ObjectType::TamTransport, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: HOSTIF user-defined trap create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorUserDefinedTrapCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_hostif_user_defined_trap() after its predecessor objects are created.
        hostif_api_override.api().create_hostif_user_defined_trap = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
            { ObjectType::TamTransport, ObjectStatus::Destroyed },
            { ObjectType::Policer, ObjectStatus::Destroyed },
            { ObjectType::HostifTrapGroup, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: TAM collector create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorTamCollectorCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_tam_collector() after its predecessor objects are created.
        tam_api_override.api().create_tam_collector = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
            { ObjectType::TamTransport, ObjectStatus::Destroyed },
            { ObjectType::Policer, ObjectStatus::Destroyed },
            { ObjectType::HostifTrapGroup, ObjectStatus::Destroyed },
            { ObjectType::HostifUserDefinedTrap, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: TAM event create failure keeps MOD disabled and removes earlier objects. */
    TEST_F(SflowOrchTest, SflowDropMonitorTamEventCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail create_tam_event() after its predecessor objects are created.
        tam_api_override.api().create_tam_event = failSaiCreate;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
            { ObjectType::TamTransport, ObjectStatus::Destroyed },
            { ObjectType::Policer, ObjectStatus::Destroyed },
            { ObjectType::HostifTrapGroup, ObjectStatus::Destroyed },
            { ObjectType::HostifUserDefinedTrap, ObjectStatus::Destroyed },
            { ObjectType::TamCollector, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: MOD remains enabled when switch unbind fails during rate reconfiguration. */
    TEST_F(SflowOrchTest, SflowDropMonitorReconfigureSwitchUnbindFailure)
    {
        ScopedSaiApiOverride<sai_switch_api_t> switch_api_override(sai_switch_api);
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        switch_api_override.api().set_switch_attribute = failSaiSetSwitchAttribute;
        setDropMonitorLimit(mock_orch, "200");
        EXPECT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    /* Test: switch bind failure keeps MOD disabled and removes every created object. */
    TEST_F(SflowOrchTest, SflowDropMonitorSwitchBindFailure)
    {
        ScopedSaiApiOverride<sai_switch_api_t> switch_api_override(sai_switch_api);
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // Fail the bind after initializeDropMonitor() created every object, including the TAM.
        switch_api_override.api().set_switch_attribute = failSaiSetSwitchAttribute;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Destroyed },
            { ObjectType::TamEventAction, ObjectStatus::Destroyed },
            { ObjectType::TamTransport, ObjectStatus::Destroyed },
            { ObjectType::Policer, ObjectStatus::Destroyed },
            { ObjectType::HostifTrapGroup, ObjectStatus::Destroyed },
            { ObjectType::HostifUserDefinedTrap, ObjectStatus::Destroyed },
            { ObjectType::TamCollector, ObjectStatus::Destroyed },
            { ObjectType::TamEvent, ObjectStatus::Destroyed },
            { ObjectType::Tam, ObjectStatus::Destroyed },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);
    }

    /* Test: cleanup after a switch bind failure reports every failed remove and refuses a retry. */
    TEST_F(SflowOrchTest, SflowDropMonitorCleanupRemoveFailure)
    {
        ScopedSaiApiOverride<sai_switch_api_t> switch_api_override(sai_switch_api);
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        // All nine objects are created, then the bind fails and cleanup runs.
        switch_api_override.api().set_switch_attribute = failSaiSetSwitchAttribute;
        // Cleanup must walk the whole list even though no object can be removed.
        tam_api_override.api().remove_tam = failSaiRemove;
        tam_api_override.api().remove_tam_event = failSaiRemove;
        tam_api_override.api().remove_tam_collector = failSaiRemove;
        tam_api_override.api().remove_tam_transport = failSaiRemove;
        tam_api_override.api().remove_tam_event_action = failSaiRemove;
        tam_api_override.api().remove_tam_report = failSaiRemove;
        hostif_api_override.api().remove_hostif_user_defined_trap = failSaiRemove;
        hostif_api_override.api().remove_hostif_trap_group = failSaiRemove;
        policer_api_override.api().remove_policer = failSaiRemove;
        ScopedObjectTracker object_tracker(tam_api_override.api(), hostif_api_override.api(),
                                           policer_api_override.api());
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        // Every object was created and none could be removed, so each remove was attempted
        // and reported an error.
        const std::vector<TrackedObject> expected_objects = {
            { ObjectType::TamReport, ObjectStatus::Created },
            { ObjectType::TamEventAction, ObjectStatus::Created },
            { ObjectType::TamTransport, ObjectStatus::Created },
            { ObjectType::Policer, ObjectStatus::Created },
            { ObjectType::HostifTrapGroup, ObjectStatus::Created },
            { ObjectType::HostifUserDefinedTrap, ObjectStatus::Created },
            { ObjectType::TamCollector, ObjectStatus::Created },
            { ObjectType::TamEvent, ObjectStatus::Created },
            { ObjectType::Tam, ObjectStatus::Created },
        };
        EXPECT_EQ(ObjectTracker::objects(), expected_objects);

        // The failed removes left the object ids in place, so re-enabling is refused
        // instead of creating a second set of objects.
        setDropMonitorLimit(mock_orch, "200");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        EXPECT_EQ(Portal::SflowOrchInternal::getSflowDropMonitorLimitRate(mock_orch.get()), 0);
    }

    /* Test: a drop_monitor_limit that is not a number is rejected and leaves MOD disabled. */
    TEST_F(SflowOrchTest, SflowDropMonitorInvalidLimit)
    {
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "abc");
        EXPECT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        EXPECT_EQ(Portal::SflowOrchInternal::getSflowDropMonitorLimitRate(mock_orch.get()), 0);
    }

    /* Test: the global handler tolerates sample_rate, including the "error" value. */
    TEST_F(SflowOrchTest, SflowGlobalSampleRateParsing)
    {
        MockSflowOrch mock_orch;
        setSflowGlobal(mock_orch,
                       { {"admin_state", "up"}, {"sample_rate", "1000"}, {"drop_monitor_limit", "0"} });
        EXPECT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));

        setSflowGlobal(mock_orch,
                       { {"admin_state", "up"}, {"sample_rate", "error"}, {"drop_monitor_limit", "0"} });
        EXPECT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
    }

    /* Test: MOD remains enabled when switch unbind fails during disable. */
    TEST_F(SflowOrchTest, SflowDropMonitorSwitchUnbindFailure)
    {
        ScopedSaiApiOverride<sai_switch_api_t> switch_api_override(sai_switch_api);
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        switch_api_override.api().set_switch_attribute = failSaiSetSwitchAttribute;
        setDropMonitorLimit(mock_orch, "0");
        EXPECT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowAddPortRejectsConflictingEgressBinding)
    {
        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 8888;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        sai_object_id_t sflow_sample;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 5678;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&sflow_sample, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        ASSERT_FALSE(Portal::SflowOrchInternal::sflowAddPort(
            mock_orch.get(), sflow_sample, port.m_port_id, "tx"));

        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
        sai_samplepacket_api->remove_samplepacket(sflow_sample);
    }

    TEST_F(SflowOrchTest, SflowAddPortAllowsSflowOwnedOidBinding)
    {
        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Create two real samplepacket OIDs (SAI VS validates OIDs) and treat
        // both as sflow-owned via seedSamplePacketOid.
        sai_object_id_t prior_sflow_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 10000;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&prior_sflow_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);

        sai_object_id_t new_sflow_oid;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 20000;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&new_sflow_oid, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        Portal::SflowOrchInternal::seedSamplePacketOid(mock_orch.get(), 10000, prior_sflow_oid);

        // Pre-bind port egress to that sflow-owned OID
        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = prior_sflow_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        // Bind a different sflow sample_id on tx; pre-check sees prior_sflow_oid is
        // sflow-owned and lets the bind through.
        ASSERT_TRUE(Portal::SflowOrchInternal::sflowAddPort(
            mock_orch.get(), new_sflow_oid, port.m_port_id, "tx"));

        // Egress attr is now the new sflow OID
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, new_sflow_oid);

        // Cleanup
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(prior_sflow_oid);
        sai_samplepacket_api->remove_samplepacket(new_sflow_oid);
    }

    TEST_F(SflowOrchTest, SflowUpdateDirectionRejectsConflictingBinding)
    {
        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Seed port info so sflowUpdateSampleDirection can find the sample_id.
        // Initial direction is "rx"; we are transitioning to "tx".
        sai_object_id_t my_sflow_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 4242;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&my_sflow_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);
        Portal::SflowOrchInternal::seedPortInfo(
            mock_orch.get(), port.m_port_id, my_sflow_oid, "rx");

        // Pre-bind port egress to a foreign (non-sflow) OID
        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 7777;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        // rx -> tx wants to assign egress; pre-check must detect the conflict
        // and bail out before any SAI mutation happens.
        ASSERT_FALSE(Portal::SflowOrchInternal::sflowUpdateSampleDirection(
            mock_orch.get(), port.m_port_id, "rx", "tx"));

        // Egress binding must remain the foreign OID (no SAI mutation happened)
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        // Cleanup
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(my_sflow_oid);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
    }

    TEST_F(SflowOrchTest, SflowAddPortRejectsConflictingIngressBinding)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;

        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Pre-bind port ingress to a foreign (non-sflow) OID via the wrap.
        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 8888;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        sai_object_id_t sflow_sample;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 5678;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&sflow_sample, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        // rx bind must be rejected because ingress is held by a foreign OID.
        ASSERT_FALSE(Portal::SflowOrchInternal::sflowAddPort(
            mock_orch.get(), sflow_sample, port.m_port_id, "rx"));

        // Ingress binding must remain the foreign OID (pre-check bails before mutation).
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
        sai_samplepacket_api->remove_samplepacket(sflow_sample);
    }

    TEST_F(SflowOrchTest, SflowUpdateDirectionRejectsConflictingIngressBinding)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;

        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Seed port info with initial direction "tx"; transition tx -> rx assigns
        // ingress, so the ingress conflict pre-check runs.
        sai_object_id_t my_sflow_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 4242;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&my_sflow_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);
        Portal::SflowOrchInternal::seedPortInfo(
            mock_orch.get(), port.m_port_id, my_sflow_oid, "tx");

        // Pre-bind port ingress to a foreign (non-sflow) OID via the wrap.
        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 7777;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        // tx -> rx wants to assign ingress; pre-check must detect the conflict
        // and bail out before any SAI mutation happens.
        ASSERT_FALSE(Portal::SflowOrchInternal::sflowUpdateSampleDirection(
            mock_orch.get(), port.m_port_id, "tx", "rx"));

        // Ingress binding must remain the foreign OID (no SAI mutation happened).
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(my_sflow_oid);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
    }

}
