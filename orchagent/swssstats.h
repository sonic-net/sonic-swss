#pragma once

#include <memory>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "orch.h"

/**
 * SwssStats - Enhanced statistics collector for SWSS components
 * 
 * Features:
 * - Operation counters (SET/DEL)
 * - Latency tracking (processing time)
 * - Queue depth monitoring
 * - Error counting
 * - Per-table and per-component statistics
 */
class SwssStats
{
public:
    static SwssStats* getInstance();
    ~SwssStats();

    // Record incoming task
    void recordIncomingTask(ConsumerBase &consumer, const swss::KeyOpFieldsValuesTuple &tuple);
    
    // Record task completion with latency
    void recordTaskComplete(ConsumerBase &consumer, const swss::KeyOpFieldsValuesTuple &tuple, 
                           uint64_t latency_us);
    
    // Record task error
    void recordTaskError(ConsumerBase &consumer, const swss::KeyOpFieldsValuesTuple &tuple);
    
    // Record queue depth
    void recordQueueDepth(const std::string &table_name, size_t depth);

private:
    struct Stats
    {
        // Operation counters
        std::atomic<std::uint64_t> m_set_count;
        std::atomic<std::uint64_t> m_del_count;
        std::atomic<std::uint64_t> m_completed_count;
        std::atomic<std::uint64_t> m_error_count;
        
        // Latency tracking (microseconds)
        std::atomic<std::uint64_t> m_total_latency_us;
        std::atomic<std::uint64_t> m_min_latency_us;
        std::atomic<std::uint64_t> m_max_latency_us;
        
        // Queue depth
        std::atomic<std::uint64_t> m_current_queue_depth;
        std::atomic<std::uint64_t> m_max_queue_depth;
        
        // Version for change detection
        std::atomic<std::uint64_t> m_version;
        
        Stats() : 
            m_set_count(0), 
            m_del_count(0),
            m_completed_count(0),
            m_error_count(0),
            m_total_latency_us(0),
            m_min_latency_us(UINT64_MAX),
            m_max_latency_us(0),
            m_current_queue_depth(0),
            m_max_queue_depth(0),
            m_version(0) 
        {}
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

    SwssStats(std::uint32_t interval = 1);
    Stats &getTableStats(const std::string &table_name);
    void dumpStats(const std::string &table_name, const Stats& stats, std::vector<swss::FieldValueTuple> &dump);
    void recordStatsThread();
    
    void updateMinMax(std::atomic<std::uint64_t> &min_val, std::atomic<std::uint64_t> &max_val, uint64_t new_val);
};
