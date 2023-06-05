#pragma once

#include <memory>
#include <thread>
#include <unordered_map>
#include <atomic>

#include "orch.h"


class OrchStats
{
public:
    static OrchStats* getInstance();
    ~OrchStats();

    void recordIncomingTask(Consumer &consumer, const swss::KeyOpFieldsValuesTuple &tuple);
private:

    struct Stats
    {
        std::atomic<std::uint64_t> m_version;
        std::atomic<std::uint64_t> m_set;
        std::atomic<std::uint64_t> m_del;
        Stats() : m_version(0), m_set(0), m_del(0) {}
    };

    using StatsTable = std::unordered_map<std::string, Stats>;
    using DumpCounters = std::vector<swss::FieldValueTuple>;

    bool m_run_thread;
    std::uint32_t m_interval;
    std::unique_ptr<std::thread> m_background_thread;
    std::mutex m_mutex;

    std::shared_ptr<swss::DBConnector> m_counter_db;
    std::unique_ptr<swss::Table> m_counter_table;

    StatsTable m_table_stats_map;

    OrchStats(std::uint32_t interval = 1);
    Stats &getTableStats(const std::string &table_name);
    void dumpStats(const Stats& stats, std::vector<swss::FieldValueTuple> &dump);
    void recordStatsThread();
};
