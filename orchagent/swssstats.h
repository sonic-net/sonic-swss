#pragma once

#include <memory>
#include <thread>
#include <map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>

namespace swss {
    class DBConnector;
    class Table;
}

// Defined in orch.cpp; set to false to disable all SwssStats recording
extern std::atomic<bool> gSwssStatsRecord;

/**
 * SwssStats - Lightweight statistics collector for SWSS orchestration
 *
 * Tracks operation counts (SET/DEL/COMPLETE/ERROR) per table with minimal overhead.
 * Uses atomic operations and a background thread for periodic Redis updates.
 */
class SwssStats
{
public:
    // Snapshot of counters for a single table (used for testing and diagnostics)
    struct CounterSnapshot
    {
        uint64_t set_count      = 0;
        uint64_t del_count      = 0;
        uint64_t complete_count = 0;
        uint64_t error_count    = 0;
    };

    // Internal stats storage — public so file-local helpers in swssstats.cpp
    // can access it without needing a friend declaration.
    struct TableStats
    {
        std::atomic<uint64_t> set_count;
        std::atomic<uint64_t> del_count;
        std::atomic<uint64_t> complete_count;
        std::atomic<uint64_t> error_count;
        // version is incremented after counter updates so the writer thread
        // can skip Redis writes when nothing changed
        std::atomic<uint64_t> version;

        TableStats() :
            set_count(0),
            del_count(0),
            complete_count(0),
            error_count(0),
            version(0)
        {}

        // Atomics are not copyable
        TableStats(const TableStats&) = delete;
        TableStats& operator=(const TableStats&) = delete;
    };

    static SwssStats* getInstance();
    ~SwssStats();

    // Record an incoming task (called from addToSync)
    void recordTask(const std::string &table_name, const std::string &op);

    // Record task completions (count tasks removed from sync queue)
    void recordComplete(const std::string &table_name, uint64_t count = 1);

    // Record task error
    void recordError(const std::string &table_name, uint64_t count = 1);

    // Return a snapshot of counters for the given table.
    // Returns zeroed snapshot if the table has no stats yet.
    CounterSnapshot getCounters(const std::string &table_name);

private:
    // m_running uses atomic to avoid data race between main and writer threads
    std::atomic<bool> m_running;
    uint32_t m_interval_sec;
    std::unique_ptr<std::thread> m_thread;
    std::mutex m_mutex;
    // m_cv allows the destructor to wake the writer thread immediately
    std::condition_variable m_cv;

    std::shared_ptr<swss::DBConnector> m_db;
    std::unique_ptr<swss::Table> m_table;

    // std::map is used instead of unordered_map: map iterators and references
    // to existing elements remain valid after new insertions, which is required
    // because recordTask() holds a reference after releasing m_mutex.
    std::map<std::string, TableStats> m_stats;

    SwssStats(uint32_t interval = 1);

    // Returns a stable reference to the TableStats for the given table,
    // creating it if it does not exist. Safe to use after m_mutex is released
    // because std::map never invalidates existing element references.
    TableStats& getOrCreateStats(const std::string &table_name);
    void writerThread();
};
