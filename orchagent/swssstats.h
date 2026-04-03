#pragma once

#include <memory>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

namespace swss {
    class DBConnector;
    class Table;
    class FieldValueTuple;
}

/**
 * SwssStats - Lightweight statistics collector for SWSS orchestration
 * 
 * Tracks operation counts (SET/DEL/COMPLETE/ERROR) per table with minimal overhead.
 * Uses atomic operations and a background thread for periodic Redis updates.
 */
class SwssStats
{
public:
    static SwssStats* getInstance();
    ~SwssStats();

    // Record an incoming task
    void recordTask(const std::string &table_name, const std::string &op);
    
    // Record task completion
    void recordComplete(const std::string &table_name);
    
    // Record task error  
    void recordError(const std::string &table_name);

private:
    struct TableStats
    {
        std::atomic<uint64_t> set_count;
        std::atomic<uint64_t> del_count;
        std::atomic<uint64_t> complete_count;
        std::atomic<uint64_t> error_count;
        std::atomic<uint64_t> version;
        
        TableStats() : 
            set_count(0), 
            del_count(0), 
            complete_count(0), 
            error_count(0),
            version(0) 
        {}
    };

    bool m_running;
    uint32_t m_interval_sec;
    std::unique_ptr<std::thread> m_thread;
    std::mutex m_mutex;
    
    std::shared_ptr<swss::DBConnector> m_db;
    std::unique_ptr<swss::Table> m_table;
    
    std::unordered_map<std::string, TableStats> m_stats;
    
    SwssStats(uint32_t interval = 1);
    
    TableStats& getStats(const std::string &table_name);
    void writerThread();
    void dumpStats(const std::string &table_name, const TableStats &stats, 
                   std::vector<swss::FieldValueTuple> &values);
};
