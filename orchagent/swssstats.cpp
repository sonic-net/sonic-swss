#include "swssstats.h"
#include "logger.h"

using namespace std;
using namespace swss;

#define SWSS_STATS_TABLE "SWSS_STATS_TABLE"

SwssStats* SwssStats::getInstance()
{
    static SwssStats instance;
    return &instance;
}

SwssStats::SwssStats(uint32_t interval) :
    m_interval(interval)
{
    SWSS_LOG_ENTER();

    m_run_thread = true;
    m_counter_db = make_shared<DBConnector>("COUNTERS_DB", 0);
    m_counter_table = make_unique<Table>(m_counter_db.get(), SWSS_STATS_TABLE);

    m_background_thread = make_unique<thread>(&SwssStats::recordStatsThread, this);
    
    SWSS_LOG_NOTICE("SwssStats initialized with interval %d seconds", m_interval);
}

SwssStats::~SwssStats()
{
    SWSS_LOG_ENTER();

    m_run_thread = false;
    if (m_background_thread && m_background_thread->joinable())
    {
        m_background_thread->join();
    }
    
    SWSS_LOG_NOTICE("SwssStats shutdown");
}

void SwssStats::recordIncomingTask(ConsumerBase &consumer, const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();

    auto table_name = consumer.getTableName();
    auto op = kfvOp(tuple);

    auto& stats = getTableStats(table_name);
    
    if (op == SET_COMMAND)
    {
        stats.m_set_count++;
    }
    else if (op == DEL_COMMAND)
    {
        stats.m_del_count++;
    }
    
    stats.m_version++;
}

void SwssStats::recordTaskComplete(ConsumerBase &consumer, const KeyOpFieldsValuesTuple &tuple, 
                                  uint64_t latency_us)
{
    SWSS_LOG_ENTER();

    auto table_name = consumer.getTableName();
    auto& stats = getTableStats(table_name);
    
    stats.m_completed_count++;
    stats.m_total_latency_us += latency_us;
    
    // Update min/max latency
    updateMinMax(stats.m_min_latency_us, stats.m_max_latency_us, latency_us);
    
    stats.m_version++;
}

void SwssStats::recordTaskError(ConsumerBase &consumer, const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();

    auto table_name = consumer.getTableName();
    auto& stats = getTableStats(table_name);
    
    stats.m_error_count++;
    stats.m_version++;
}

void SwssStats::recordQueueDepth(const std::string &table_name, size_t depth)
{
    SWSS_LOG_ENTER();

    auto& stats = getTableStats(table_name);
    
    stats.m_current_queue_depth = depth;
    
    // Update max queue depth
    uint64_t current_max = stats.m_max_queue_depth.load();
    while (depth > current_max && 
           !stats.m_max_queue_depth.compare_exchange_weak(current_max, depth))
    {
        // Loop until successful update
    }
    
    stats.m_version++;
}

SwssStats::Stats &SwssStats::getTableStats(const std::string &table_name)
{
    SWSS_LOG_ENTER();

    auto it = m_table_stats_map.find(table_name);
    if (it == m_table_stats_map.end())
    {
        lock_guard<mutex> lock(m_mutex);
        it = m_table_stats_map.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(table_name),
            std::forward_as_tuple()).first;
        
        SWSS_LOG_INFO("Created stats for table: %s", table_name.c_str());
    }

    return it->second;
}

void SwssStats::dumpStats(const std::string &table_name, const Stats& stats, vector<FieldValueTuple> &dump)
{
    SWSS_LOG_ENTER();

    dump.clear();

    // Operation counters
    dump.emplace_back("SET", to_string(stats.m_set_count.load()));
    dump.emplace_back("DEL", to_string(stats.m_del_count.load()));
    dump.emplace_back("COMPLETED", to_string(stats.m_completed_count.load()));
    dump.emplace_back("ERROR", to_string(stats.m_error_count.load()));
    
    // Latency statistics (in microseconds)
    uint64_t completed = stats.m_completed_count.load();
    uint64_t total_latency = stats.m_total_latency_us.load();
    uint64_t avg_latency = (completed > 0) ? (total_latency / completed) : 0;
    
    dump.emplace_back("AVG_LATENCY_US", to_string(avg_latency));
    dump.emplace_back("MIN_LATENCY_US", to_string(stats.m_min_latency_us.load()));
    dump.emplace_back("MAX_LATENCY_US", to_string(stats.m_max_latency_us.load()));
    dump.emplace_back("TOTAL_LATENCY_US", to_string(total_latency));
    
    // Queue depth
    dump.emplace_back("QUEUE_DEPTH", to_string(stats.m_current_queue_depth.load()));
    dump.emplace_back("MAX_QUEUE_DEPTH", to_string(stats.m_max_queue_depth.load()));
}

void SwssStats::recordStatsThread()
{
    SWSS_LOG_ENTER();

    std::unordered_map<std::string, std::uint64_t> dump_version;

    SWSS_LOG_NOTICE("SwssStats background thread started");

    while (m_run_thread)
    {
        vector<string> stats_names;
        vector<vector<FieldValueTuple>> stats_values;
        
        {
            lock_guard<mutex> lock(m_mutex);
            
            for (const auto& table_stats : m_table_stats_map)
            {
                auto ver = dump_version.find(table_stats.first);
                if (ver == dump_version.end())
                {
                    ver = dump_version.emplace(table_stats.first, 0).first;
                }
                else if (ver->second == table_stats.second.m_version.load())
                {
                    // No changes, skip
                    continue;
                }
                
                ver->second = table_stats.second.m_version.load();
                stats_names.emplace_back(table_stats.first);
                stats_values.emplace_back();
                dumpStats(table_stats.first, table_stats.second, stats_values.back());
            }
        }
        
        // Write to Redis outside the lock
        for (size_t i = 0; i < stats_names.size(); i++)
        {
            m_counter_table->set(stats_names[i], stats_values[i]);
            SWSS_LOG_DEBUG("Updated stats for table: %s", stats_names[i].c_str());
        }
        
        this_thread::sleep_for(chrono::seconds(m_interval));
    }
    
    SWSS_LOG_NOTICE("SwssStats background thread stopped");
}

void SwssStats::updateMinMax(std::atomic<std::uint64_t> &min_val, std::atomic<std::uint64_t> &max_val, uint64_t new_val)
{
    // Update minimum
    uint64_t current_min = min_val.load();
    while (new_val < current_min && 
           !min_val.compare_exchange_weak(current_min, new_val))
    {
        // Loop until successful update
    }
    
    // Update maximum
    uint64_t current_max = max_val.load();
    while (new_val > current_max && 
           !max_val.compare_exchange_weak(current_max, new_val))
    {
        // Loop until successful update
    }
}
