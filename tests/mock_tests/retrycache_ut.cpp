#define protected public
#include "orch.h"
#undef protected

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace retrycache_test
{

    using ::testing::_;
    using ::testing::Return;
    using ::testing::StrictMock;
    using ::testing::InSequence;

    DBConnector appl_db("APPL_DB", 0);
    DBConnector state_db("STATE_DB", 0);
    DBConnector config_db("CONFIG_DB", 0);
    DBConnector counters_db("COUNTERS_DB", 0);

    class RetryCacheTest : public ::testing::Test
    {
        public:

            class TestOrch: public Orch
            {
            public:
                TestOrch(DBConnector* db, const std::vector<std::string>& consumers, Constraint cst)
                    : Orch(db, consumers), cst(cst) {}

                Constraint cst;

                using Orch::doTask;

                void doTask(Consumer &consumer) override {
                    if (consumer.getName() == "CstResolver") {
                        // Simulate constraint resolution
                        std::cout << "  CstResolver [Selected]: resolves the constraint and notifies." << std::endl;
                        auto it = consumer.m_toSync.begin();
                        consumer.m_toSync.erase(it);
                        Constraint cst = make_constraint(RETRY_CST_PIC, "2");
                        notifyRetry(this, "Dependent", cst);
                    }
                    else if (consumer.getName() == "Dependent")
                    {
                        static int dependent_call = 0;
                        dependent_call ++;

                        if (dependent_call == 1) {
                            std::cout << "  Dependent Executor [Selected]: fails, move the task to retrycache." << std::endl;
                            auto it = consumer.m_toSync.begin();
                            getRetryCache("Dependent")->insert(it->second, cst);
                            consumer.m_toSync.erase(it);
                        } else if (dependent_call == 2) {
                            std::cout << "  Dependent Executor [Retry phase]: skip due to empty m_toSync." << std::endl;
                        } else if (dependent_call == 3) {
                            std::cout << "  Dependent Executor [Retry phase]: drain m_toSync and succeed." << std::endl;
                            consumer.m_toSync.erase(consumer.m_toSync.begin());
                        }
                    }
                };
            };

            TestOrch* testOrch;
            Consumer *oftenFail, *cstResolver;
            RetryCache* cache;
            Constraint cst = make_constraint(RETRY_CST_PIC, "2");

            void SetUp() override
            {
                // 1. The dependent executor often fails and needs retry, which depends on CstResolver.
                // Its retry would always fail unless CstResolver resolves the constraint and notifies it.
                testOrch = new TestOrch(&appl_db, std::vector<std::string>{"CstResolver", "Dependent"}, cst);
                oftenFail = static_cast<Consumer*>(testOrch->getConsumerBase("Dependent"));
                cstResolver = static_cast<Consumer*>(testOrch->getConsumerBase("CstResolver"));

                // 2. Their Orch allocates a retrycache for the dependent executor to help improve its retry efficiency.
                testOrch->createRetryCache("Dependent");
                cache = testOrch->getRetryCache("Dependent");
                ASSERT_TRUE(cache);
            }

            void TearDown() override
            {
                delete testOrch;
            }
    };

    TEST_F(RetryCacheTest, MULTI_MAP)
    {
        // safe to evict non-existing key
        ASSERT_EQ(cache->evict("non_exist_key"), nullptr);

        // empty key is ignored
        ASSERT_TRUE(testOrch->addToRetry("Dependent", {"", "SET", {{"field", "value"}}}, cst));
        ASSERT_TRUE(cache->getRetryMap().empty());

        cache->insert({"key", "DEL", {}}, cst);
        cache->insert({"key", "SET", {{"field", "value"}}}, cst);

        auto tasks = cache->resolve(cst);
        ASSERT_EQ(tasks->size(), 2);
        ASSERT_TRUE(cache->getRetryMap().empty());

        cache->insert({"key", "DEL", {}}, cst);
        Constraint _cst = make_constraint(RETRY_CST_PIC, "3");
        cache->insert({"key", "SET", {{"nexthop_index", "3"}}}, _cst);
        tasks = cache->resolve(cst);
        ASSERT_EQ(tasks->size(), 1);
        ASSERT_EQ((*tasks)[0], (KeyOpFieldsValuesTuple{"key", "DEL", {}}));
        ASSERT_EQ(cache->getRetryMap().size(), 1);
    }

    TEST_F(RetryCacheTest, Basics)
    {
        // Check initial state
        ASSERT_TRUE(oftenFail->m_toSync.empty());
        ASSERT_TRUE(cache->getRetryMap().empty());
        ASSERT_TRUE(cache->getResolvedConstraints().empty());

        std::cout << "Orchdaemon Event Loop 1: " << std::endl;

        // Assume the Dependent Executor receives a task to process
        Task task{"TEST_ROUTE", "SET", {{"PIC", "2"}}};
        oftenFail->addToSync(task);
        // But it fails because of the constraint: PIC 2 doesn't exist
        // It moves the task from m_toSync to m_toRetry
        oftenFail->drain();

        // Pre cst resolution
        // 1. Check if it marks cst as a constraint to resolve
        ASSERT_EQ(cache->m_retryKeys.begin()->first, cst);
        // 2. Check if it records the related key under the corresponding cst
        ASSERT_EQ(*cache->m_retryKeys[cst].begin(), kfvKey(task));
        // 3. Check if the entry {key: (cst, task)} is in m_toRetry
        FailedTask failedTask = make_pair(cst, task);
        ASSERT_EQ(cache->m_toRetry.find(kfvKey(task))->second, failedTask);

        // When Orchdaemon enters the retry phase, it invokes doTask() for every orch
        // Simulates this behavior by:
        testOrch->doTask();

        // However, since the failed task is moved to retrycache, it's skipped and untouched during Orchdaemon's retry. 
        // Repeat the checks and expect the same answer.
        ASSERT_TRUE(oftenFail->m_toSync.empty());
        ASSERT_EQ(cache->m_retryKeys.begin()->first, cst);
        ASSERT_EQ(*cache->m_retryKeys[cst].begin(), kfvKey(task));
        ASSERT_EQ(cache->m_toRetry.find(kfvKey(task))->second, failedTask);

        std::cout << "Orchdaemon Event Loop 2: " << std::endl;

        // Assume in the next event loop, the cstResolver a task to process, which resolves the constraint for oftenFail.
        cstResolver->addToSync({"TEST_PIC", "SET", {{"ID", "2"}}});
        cstResolver->drain();

        // Post-resolution
        // 1. Check if the cache has realized that the cst is resolved
        ASSERT_TRUE(cache->getResolvedConstraints().find(cst) != cache->getResolvedConstraints().end());
        // 2. Check if the failed task is not aware of the resolution at the moment
        ASSERT_EQ(cache->m_retryKeys.begin()->first, cst);
        ASSERT_EQ(cache->m_toRetry.find("TEST_ROUTE")->second, failedTask);
        ASSERT_EQ(*cache->m_retryKeys[cst].begin(), kfvKey(task));

        // When Orchdaemon enters the general retry phase, it invokes doTask() for every orch
        // Simulates this behavior by:
        testOrch->doTask();

        // Check whether the task is moved back to m_toSync
        ASSERT_TRUE(cache->m_toRetry.empty());
        ASSERT_TRUE(cache->m_retryKeys.empty());
        ASSERT_TRUE(cache->m_resolvedConstraints.empty());

        ASSERT_EQ(oftenFail->m_toSync.size(), 1);
        ASSERT_EQ(oftenFail->m_toSync.find("TEST_ROUTE")->second, task);

        std::cout << "Orchdaemon Event Loop 3: " << std::endl;
        testOrch->doTask();
        ASSERT_TRUE(oftenFail->m_toSync.empty());
    }

    TEST_F(RetryCacheTest, SkipDuplicateTasks)
    {
        // Check initial state
        ASSERT_TRUE(oftenFail->m_toSync.empty());
        ASSERT_TRUE(cache->getRetryMap().empty());

        Task setTask{"test_key", "SET", {{"test_field", "test_value"}}};
        // assume there is already a failed task in the cache
        cache->insert(setTask, cst);
        ASSERT_EQ(cache->getRetryMap().size(), 1);

        // Now add the same task to the sync queue
        oftenFail->addToSync(setTask);

        // Check whether the task is skipped
        ASSERT_EQ(cache->getRetryMap().size(), 1);
        ASSERT_TRUE(oftenFail->m_toSync.empty());
    }

    TEST_F(RetryCacheTest, NewDelTask)
    {
        // Check initial state
        ASSERT_TRUE(oftenFail->m_toSync.empty());
        ASSERT_TRUE(cache->getRetryMap().empty());

        // Assume there is a SET task in the retry cache
        Task setTask{"test_key", "SET", {{"test_field", "test_value"}}};
        cache->insert(setTask, cst);

        // Assume there is a new DEL task received by the consumer
        Task delTask{"test_key", "DEL", {{"", ""}}};
        oftenFail->addToSync(delTask);

        // It should remove the SET task from retrycache, since it's no longer needed
        ASSERT_TRUE(cache->getRetryMap().empty());
        ASSERT_TRUE(cache->m_retryKeys.empty());
        ASSERT_EQ(oftenFail->m_toSync.size(), 1);
        
        // reset the m_toSync
        oftenFail->m_toSync.erase("test_key");

        // Assume there are a SET and a DEL task in the retry cache
        cache->insert(setTask, cst);
        Constraint del_cst = make_constraint(RETRY_CST_PIC_REF, "");
        cache->insert(delTask, del_cst);
        ASSERT_EQ(cache->getRetryMap().size(), 2);

        // Assume there is a new DEL task received by the consumer
        oftenFail->addToSync(delTask);

        // Check the SET-task is removed from the retry cache
        ASSERT_EQ(cache->getRetryMap().size(), 1);
        // Check the DEL-task is not in the sync queue, only in the retry cache
        ASSERT_TRUE(oftenFail->m_toSync.empty());
        ASSERT_EQ(cache->getRetryMap().find("test_key")->second.first, del_cst);
        ASSERT_EQ(cache->getRetryMap().find("test_key")->second.second, delTask);
    }

    TEST_F(RetryCacheTest, NewSetTask)
    {
        // Assume there is an old SET task in the retry cache
        Task setTask{"TEST_ROUTE", "SET", {{"PIC", "1"}}};
        Constraint cst_pic_1 = make_constraint(RETRY_CST_PIC, "1");
        cache->insert(setTask, cst_pic_1);

        // Assume there is a new task with same field, received by the consumer
        Task setTask2{"TEST_ROUTE", "SET", {{"PIC", "2"}}};
        oftenFail->addToSync(setTask2);
        // Check if it clears the retrycache
        ASSERT_TRUE(cache->getRetryMap().empty());
        ASSERT_EQ(cache->m_retryKeys.find(cst_pic_1), cache->m_retryKeys.end());
        // Check if it adds the task 2 into the sync queue
        auto iter = oftenFail->m_toSync.find("TEST_ROUTE");
        ASSERT_EQ(iter->second, setTask2);

        // Assume PIC 2 also doesn't exist, the new task fails too
        oftenFail->m_toSync.erase(iter);
        Constraint cst_pic_2 = make_constraint(RETRY_CST_PIC, "2");
        cache->insert(setTask2, cst_pic_2);

        // Assume there is a new SET task with a new field, received by the consumer
        Task setTask3{"TEST_ROUTE", "SET", {{"VRF", "1"}}};
        oftenFail->addToSync(setTask3);

        // Check if the task2 is removed from the retrycache
        ASSERT_TRUE(cache->getRetryMap().empty());
        ASSERT_TRUE(cache->m_retryKeys.empty());
        // Check if the task2 and task3 are merged correctly in m_toSync
        ASSERT_EQ(oftenFail->m_toSync.size(), 1);
        auto merged = oftenFail->m_toSync.find("TEST_ROUTE")->second;
        auto fvs = kfvFieldsValues(merged);
        ASSERT_EQ(fvs.size(), 2);
        for (auto fv: fvs)
        {
            if (fvField(fv) == "VRF")
                ASSERT_EQ(fvValue(fv), "1");
            else if (fvField(fv) == "PIC")
                ASSERT_EQ(fvValue(fv), "2");
            else
                ASSERT_FALSE(true); // unexpected field
        }
    }

    TEST_F(RetryCacheTest, NewSetWithTwoOldTasks)
    {
        // Assume there are a SET and a DEL task in the retry cache
        // simulate by firstly adding a DEL, then adding a SET into the retry cache
        Task delTask{"TEST_ROUTE", "DEL", {{"", ""}}};
        cache->insert(delTask, DUMMY_CONSTRAINT);
        Task setTask{"TEST_ROUTE", "SET", {{"PIC", "1"}}};
        Constraint cst = make_constraint(RETRY_CST_PIC, "1");
        cache->insert(setTask, cst);

        ASSERT_EQ(cache->getRetryMap().size(), 2);
        ASSERT_EQ(cache->m_retryKeys.size(), 2);
        ASSERT_EQ(*cache->m_retryKeys[DUMMY_CONSTRAINT].begin(), "TEST_ROUTE");
        ASSERT_EQ(*cache->m_retryKeys[cst].begin(), "TEST_ROUTE");

        // Assume there is a new SET task received by the consumer
        Task setTask2{"TEST_ROUTE", "SET", {{"VRF", "1"}}};
        oftenFail->addToSync(setTask2);

        ASSERT_EQ(cache->getRetryMap().size(), 1);
        ASSERT_EQ(oftenFail->m_toSync.size(), 1);
        auto fvs = kfvFieldsValues(oftenFail->m_toSync.find("TEST_ROUTE")->second);
        ASSERT_EQ(fvs.size(), 2);
        for (auto fv: fvs)
        {
            if (fvField(fv) == "VRF")
                ASSERT_EQ(fvValue(fv), "1");
            else if (fvField(fv) == "PIC")
                ASSERT_EQ(fvValue(fv), "1");
             else
                ASSERT_FALSE(true); // unexpected field
        }
    }
}