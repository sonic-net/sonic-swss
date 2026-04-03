#include "swssstats.h"
#include "dbconnector.h"
#include "table.h"
#include "logger.h"
#include <chrono>

using namespace std;
using namespace swss;

#define SWSS_STATS_TABLE "SWSS_STATS"

SwssStats* SwssStats::getInstance()
{
    static SwssStats instance;
    return &instance;
}

SwssStats::SwssStats(uint32_t interval)
    : m_running(true)
    , m_interval_sec(interval)
{
    SWSS_LOG_ENTER();
    
    // Connect to COUNTERS_DB
    m_db = make_shared<DBConnector>("COUNTERS_DB", 0);
    m_table = make_unique<Table>(m_db.get(), SWSS_STATS_TABLE);
    
    // Start background writer thread
    m_thread = make_unique<thread>(&SwssStats::writerThread, this);
    
    SWSS_LOG_NOTICE("SwssStats initialized (interval: %d sec)", m_interval_sec);
}

SwssStats::~SwssStats()
{
    SWSS_LOG_ENTER();
    
    m_running = false;
    if (m_thread && m_thread->joinable())
    {
        m_thread->join();
    }
    
    SWSS_LOG_NOTICE("SwssStats stopped");
}

void SwssStats::recordTask(const string &table_name, const string &op)
{
    SWSS_LOG_ENTER();
    
    auto& stats = getStats(table_name);
    
    if (op == "SET")
    {
        stats.set_count++;
    }
    else if (op == "DEL")
    {
        stats.del_count++;
    }
    
    stats.version++;
}

void SwssStats::recordComplete(const string &table_name)
{
    SWSS_LOG_ENTER();
    
    auto& stats = getStats(table_name);
    stats.complete_count++;
    stats.version++;
}

void SwssStats::recordError(const string &table_name)
{
    SWSS_LOG_ENTER();
    
    auto& stats = getStats(table_name);
    stats.error_count++;
    stats.version++;
}

SwssStats::TableStats& SwssStats::getStats(const string &table_name)
{
    lock_guard<mutex> lock(m_mutex);
    
    auto it = m_stats.find(table_name);
    if (it == m_stats.end())
    {
        it = m_stats.emplace(piecewise_construct,
                            forward_as_tuple(table_name),
                            forward_as_tuple()).first;
        
        SWSS_LOG_INFO("Created stats for table: %s", table_name.c_str());
    }
    
    return it->second;
}

void SwssStats::dumpStats(const string &table_name, const TableStats &stats,
                         vector<FieldValueTuple> &values)
{
    values.clear();
    
    values.emplace_back("SET", to_string(stats.set_count.load()));
    values.emplace_back("DEL", to_string(stats.del_count.load()));
    values.emplace_back("COMPLETE", to_string(stats.complete_count.load()));
    values.emplace_back("ERROR", to_string(stats.error_count.load()));
}

void SwssStats::writerThread()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("SwssStats writer thread started");
    
    unordered_map<string, uint64_t> last_versions;
    
    while (m_running)
    {
        vector<string> table_names;
        vector<vector<FieldValueTuple>> table_values;
        
        {
            lock_guard<mutex> lock(m_mutex);
            
            for (const auto& entry : m_stats)
            {
                const string& name = entry.first;
                const TableStats& stats = entry.second;
                
                uint64_t current_ver = stats.version.load();
                
                // Check if stats changed since last write
                auto ver_it = last_versions.find(name);
                if (ver_it != last_versions.end() && ver_it->second == current_ver)
                {
                    continue;  // No changes
                }
                
                last_versions[name] = current_ver;
                
                table_names.push_back(name);
                table_values.emplace_back();
                dumpStats(name, stats, table_values.back());
            }
        }
        
        // Write to Redis outside the lock
        for (size_t i = 0; i < table_names.size(); i++)
        {
            m_table->set(table_names[i], table_values[i]);
            SWSS_LOG_DEBUG("Updated stats for: %s", table_names[i].c_str());
        }
        
        this_thread::sleep_for(chrono::seconds(m_interval_sec));
    }
    
    SWSS_LOG_NOTICE("SwssStats writer thread stopped");
}
