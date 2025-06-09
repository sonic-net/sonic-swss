#ifndef SWSS_RETRY_CACHE_H
#define SWSS_RETRY_CACHE_H

#include <unordered_set>
#include <unordered_map>
#include <sys/time.h>
#include "timestamp.h"
#include <fstream>

using namespace swss;

enum ConstraintType
{
    RETRY_CST_DUMMY,
    RETRY_CST_NHG,          // nhg doesn't exist
    RETRY_CST_NHG_REF,      // nhg refcnt nonzero
    RETRY_CST_PIC,          // context doesn't exist
    RETRY_CST_PIC_REF,      // context refcnt nonzero
    RETRY_CST_ECMP          // ecmp resources exhausted
};

using ConstraintData = std::string;
using Constraint = std::pair<ConstraintType, ConstraintData>;

const Constraint DUMMY_CONSTRAINT{RETRY_CST_DUMMY, ""};

inline Constraint make_constraint(ConstraintType type, ConstraintData data = "") {
    return {type, data};
}

typedef swss::KeyOpFieldsValuesTuple Task;
typedef std::pair<Constraint, Task> FailedTask;
typedef std::unordered_map<std::string, FailedTask> RetryMap;

namespace std {
    template<>
    struct hash<::Constraint> {
        std::size_t operator()(const ::Constraint& c) const {
            return hash<::ConstraintType>{}(c.first) ^
                   (hash<::ConstraintData>{}(c.second) << 2);
        }
    };
}

using RetryKeysMap = std::unordered_map<Constraint, std::unordered_set<std::string>>;

class RetryCache
{
private:

    std::string m_executorName; // name of the corresponding executor
    std::unordered_set<Constraint> m_resolvedConstraints; // store the resolved constraints notified
    RetryKeysMap m_retryKeys; // group failed tasks by constraints
    RetryMap m_toRetry; // cache the data about the failed tasks for a ConsumerBase instance

public:

    RetryCache(std::string executorName) : m_executorName (executorName) {}

    std::unordered_set<Constraint>& getResolvedConstraints()
    {
        return m_resolvedConstraints;
    }

    RetryMap& getRetryMap()
    {
        return m_toRetry;
    }

    /** When notified of a constraint resolution, ignore it if it's unrelated,
     * otherwise record this resolution event by adding this cst into inner bookkeeping.
     * Then when the executor performs retry, it only retries those with cst recorded as resolved.
     * @param cst a constraint that's already been resolved
     */
    void add_resolution(const Constraint &cst) {
        if (m_retryKeys.find(cst) == m_retryKeys.end())
        {
            return;
        }
        else
        {
            m_resolvedConstraints.emplace(cst.first, cst.second);
        }
    }

    /** Insert a failed task with its constraint to m_toRetry and m_retryKeys
     * @param task the task that has failed
     * @param cst constraint needs to be resolved for the task to succeed
     */
    void cache_failed_task(const Task &task, const Constraint &cst) {
        const auto& key = kfvKey(task);
        m_retryKeys[cst].insert(key);
        m_toRetry.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(cst, task)
        );
    }

    /** For a new task, if it has a stale version in RetryCache, we need to clear the cache
     * @param key key of swss::KeyOpFieldsValuesTuple task
     * @return the task that has failed before and stored in retry cache
     */
    std::shared_ptr<Task> erase_stale_cache(const std::string &key) {

        auto it = m_toRetry.find(key);
        if (it == m_toRetry.end())
            return std::make_shared<Task>();

        Constraint cst = it->second.first;
        auto task = std::make_shared<Task>(std::move(it->second.second));

        m_retryKeys[cst].erase(key);
        m_toRetry.erase(key);

        if (m_retryKeys[cst].empty()) {
            m_retryKeys.erase(cst);
            m_resolvedConstraints.erase(cst);
        }

        return task;
    }

    /** Find cached failed tasks that can be resolved by the constraint, remove them from the retry cache.
     * @param cst the retry constraint
     * @return the resolved failed tasks
     */
    std::shared_ptr<std::deque<KeyOpFieldsValuesTuple>> resolve(const Constraint &cst, size_t threshold = 30000) {

        auto tasks = std::make_shared<std::deque<KeyOpFieldsValuesTuple>>();

        // get a set of keys that correspond to tasks constrained by the cst
        std::unordered_set<std::string>& keys = m_retryKeys[cst];

        size_t count = 0;

        for (auto it = keys.begin(); it != keys.end() && count < threshold; it = keys.erase(it), count++)
        {
            auto failed_task_it = m_toRetry.find(*it);
            if (failed_task_it != m_toRetry.end())
            {
                tasks->push_back(std::move(failed_task_it->second.second));
                m_toRetry.erase(failed_task_it);
            }
        }

       if (keys.empty()) {
            m_retryKeys.erase(cst);
            m_resolvedConstraints.erase(cst);
        }

        return tasks;
    }
};

typedef std::unordered_map<std::string, std::shared_ptr<RetryCache>> RetryCacheMap;

#endif /* SWSS_RETRY_CACHE_H */