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

    {
        lock_guard<mutex> lock(m_mutex);
        m_running = false;
    }
    // Wake the writer thread immediately instead of waiting up to m_interval_sec
    m_cv.notify_all();

    if (m_thread && m_thread->joinable())
    {
        m_thread->join();
    }

    SWSS_LOG_NOTICE("SwssStats stopped");
}

void SwssStats::recordTask(const string &table_name, const string &op)
{
    auto& stats = getOrCreateStats(table_name);

    if (op == "SET")
    {
        stats.set_count.fetch_add(1, memory_order_relaxed);
    }
    else if (op == "DEL")
    {
        stats.del_count.fetch_add(1, memory_order_relaxed);
    }

    // Release ordering ensures counter writes above are visible to the writer
    // thread before it observes the version increment
    stats.version.fetch_add(1, memory_order_release);
}

void SwssStats::recordComplete(const string &table_name, uint64_t count)
{
    auto& stats = getOrCreateStats(table_name);
    stats.complete_count.fetch_add(count, memory_order_relaxed);
    stats.version.fetch_add(1, memory_order_release);
}

void SwssStats::recordError(const string &table_name, uint64_t count)
{
    auto& stats = getOrCreateStats(table_name);
    stats.error_count.fetch_add(count, memory_order_relaxed);
    stats.version.fetch_add(1, memory_order_release);
}

SwssStats::TableStats& SwssStats::getOrCreateStats(const string &table_name)
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

    // Safe to return a reference after releasing the lock: std::map never
    // invalidates references to existing elements on insert or erase of
    // other elements (unlike unordered_map which rehashes).
    return it->second;
}

void SwssStats::dumpStats(const TableStats &stats,
                          vector<FieldValueTuple> &values)
{
    values.clear();

    values.emplace_back("SET",      to_string(stats.set_count.load(memory_order_relaxed)));
    values.emplace_back("DEL",      to_string(stats.del_count.load(memory_order_relaxed)));
    values.emplace_back("COMPLETE", to_string(stats.complete_count.load(memory_order_relaxed)));
    values.emplace_back("ERROR",    to_string(stats.error_count.load(memory_order_relaxed)));
}

void SwssStats::writerThread()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("SwssStats writer thread started");

    unordered_map<string, uint64_t> last_versions;

    while (true)
    {
        {
            unique_lock<mutex> lock(m_mutex);
            // Wait for either the interval to elapse or a shutdown signal
            m_cv.wait_for(lock, chrono::seconds(m_interval_sec),
                          [this]{ return !m_running.load(memory_order_relaxed); });

            if (!m_running.load(memory_order_relaxed))
            {
                break;
            }
        }

        vector<string> table_names;
        vector<vector<FieldValueTuple>> table_values;

        {
            lock_guard<mutex> lock(m_mutex);

            for (const auto& entry : m_stats)
            {
                const string& name = entry.first;
                const TableStats& stats = entry.second;

                // Acquire ordering pairs with the release in record* methods,
                // ensuring we see all counter updates made before version++
                uint64_t current_ver = stats.version.load(memory_order_acquire);

                auto ver_it = last_versions.find(name);
                if (ver_it != last_versions.end() && ver_it->second == current_ver)
                {
                    continue;  // No changes since last write
                }

                last_versions[name] = current_ver;

                table_names.push_back(name);
                table_values.emplace_back();
                dumpStats(stats, table_values.back());
            }
        }

        // Write to Redis outside the lock
        for (size_t i = 0; i < table_names.size(); i++)
        {
            m_table->set(table_names[i], table_values[i]);
            SWSS_LOG_DEBUG("Updated stats for: %s", table_names[i].c_str());
        }
    }

    SWSS_LOG_NOTICE("SwssStats writer thread stopped");
}

