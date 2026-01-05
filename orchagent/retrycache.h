#pragma once

#include <unordered_set>
#include <unordered_map>
#include "recorder.h"
#include "rediscommand.h"

using namespace swss;

enum ConstraintType
{
    RETRY_CST_DUMMY,
    RETRY_CST_PIC,          // context doesn't exist
    RETRY_CST_PIC_REF      // context refcnt nonzero
};

static inline std::ostream& operator<<(std::ostream& os, ConstraintType t) {
    switch(t) {
        case ConstraintType::RETRY_CST_DUMMY:   return os << "RETRY_CST_DUMMY";
        case ConstraintType::RETRY_CST_PIC: return os << "RETRY_CST_PIC";
        case ConstraintType::RETRY_CST_PIC_REF:  return os << "RETRY_CST_PIC_REF";
        default:           return os << "UNKNOWN";
    }
}

using ConstraintData = std::string;
using Constraint = std::pair<ConstraintType, ConstraintData>;

const Constraint DUMMY_CONSTRAINT{RETRY_CST_DUMMY, ""};

static inline Constraint make_constraint(ConstraintType type, ConstraintData data = "") {
    return {type, data};
}

template<typename T, typename U>
std::ostream& operator<<(std::ostream& os, const std::pair<T, U>& p) {
    return os << "(" << p.first << ", " << p.second << ")";
}

typedef swss::KeyOpFieldsValuesTuple Task;
typedef std::pair<Constraint, Task> FailedTask;
typedef std::multimap<std::string, FailedTask> RetryMap;

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
public:
    std::string m_executorName; // name of the corresponding executor
    std::unordered_set<Constraint> m_resolvedConstraints; // store the resolved constraints notified
    RetryKeysMap m_retryKeys; // group failed tasks by constraints
    RetryMap m_toRetry; // cache the data about the failed tasks for a ConsumerBase instance

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
    void mark_resolved(const Constraint &cst)
    {
        if (m_retryKeys.find(cst) == m_retryKeys.end())
            return;

        m_resolvedConstraints.emplace(cst.first, cst.second);

        std::stringstream ss;
        ss << cst << " resolution notified -> " << m_retryKeys[cst].size() << " task(s)";
        Recorder::Instance().retry.record(ss.str());
    }

    /** Insert a failed task with its constraint to m_toRetry and m_retryKeys
     * @param task the task that has failed
     * @param cst constraint needs to be resolved for the task to succeed
     */
    void insert(const Task &task, const Constraint &cst)
    {
        const auto& key = kfvKey(task);
        if (key.empty())
            return;
        m_retryKeys[cst].insert(key);
        m_toRetry.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(cst, task)
        );
    }

    /** Delete a SET task from the retry cache by its key.
     * @param key key of swss::KeyOpFieldsValuesTuple task
     * @return the task that has failed before and stored in retry cache
     */
    std::shared_ptr<Task> evict(const std::string &key)
    {
        // m_toRetry is multimap, hence there are at most 2 tasks mapped from key.
        auto range = m_toRetry.equal_range(key);
        auto it = range.first;
        for (; it != range.second; it++)
        {
            if (kfvOp(it->second.second) == SET_COMMAND)
            {
                break;
            }
        }

        if (it == range.second)
        {
            return nullptr;
        }

        // parse the corresponding task and its constraint
        Constraint cst = it->second.first;
        auto task = std::make_shared<Task>(std::move(it->second.second));

        // Erase the task from m_toRetry, and unbind cst with it.
        m_toRetry.erase(it);
        m_retryKeys[cst].erase(key);

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
        auto it = keys.begin();
        while (it != keys.end() && count < threshold)
        {
            auto range = m_toRetry.equal_range(*it);
            // key may map to multiple tasks, e.g. a SET and a DEL
            // but they may have different constraints
            auto failed_task_it = range.first;
            while (failed_task_it != range.second)
            {
                auto failed_task = failed_task_it->second;
                if (failed_task.first == cst)
                {
                    tasks->push_back(std::move(failed_task.second));
                    failed_task_it = m_toRetry.erase(failed_task_it);
                    count++;
                } else {
                    ++failed_task_it;
                }
            }
            it = keys.erase(it);
        }

        std::stringstream ss;
        ss << cst << " | " << m_executorName << " | " << tasks->size() << " retried";

        if (keys.empty()) {
            m_retryKeys.erase(cst);
            m_resolvedConstraints.erase(cst);
        } else {
            ss << " (rest:" << keys.size() << ")";
        }

        Recorder::Instance().retry.record(ss.str());

        return tasks;
    }
};

typedef std::unordered_map<std::string, std::shared_ptr<RetryCache>> RetryCacheMap;

