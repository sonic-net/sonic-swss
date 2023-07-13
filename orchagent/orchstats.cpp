#include "orchstats.h"

using namespace std;
using namespace swss;

#define ORCH_STATS_TABLE "ORCH_STATS_TABLE"

OrchStats* OrchStats::getInstance()
{
    static OrchStats instance;
    return &instance;
}

OrchStats::OrchStats(uint32_t interval) :
    m_interval(interval)
{
    SWSS_LOG_ENTER();

    m_run_thread = true;
    m_counter_db = make_shared<DBConnector>("COUNTERS_DB", 0);
    m_counter_table = make_unique<Table>(m_counter_db.get(), ORCH_STATS_TABLE);

    m_background_thread = make_unique<thread>(&OrchStats::recordStatsThread, this);
}

OrchStats::~OrchStats()
{
    SWSS_LOG_ENTER();

    m_run_thread = false;
    m_background_thread->join();
}

void OrchStats::recordIncomingTask(ConsumerBase &consumer, const KeyOpFieldsValuesTuple &tuple)
{
    SWSS_LOG_ENTER();

    auto table_name = consumer.getTableName();
    auto op = kfvOp(tuple);

    auto& stats = getTableStats(table_name);
    if (op == SET_COMMAND)
    {
        stats.m_set++;
    }
    else
    {
        stats.m_del++;
    }
    stats.m_version++;
}

OrchStats::Stats &OrchStats::getTableStats(const std::string &table_name)
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
    }

    return it->second;
}

void OrchStats::dumpStats(const OrchStats::Stats& stats, vector<FieldValueTuple> &dump)
{
    SWSS_LOG_ENTER();

    dump.clear();

    dump.emplace_back("SET", to_string(stats.m_set));
    dump.emplace_back("DEL", to_string(stats.m_del));
}

void OrchStats::recordStatsThread()
{
    SWSS_LOG_ENTER();

    std::unordered_map<std::string, std::uint64_t> dump_version;

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
                else if (ver->second == table_stats.second.m_version)
                {
                    continue;
                }
                ver->second = table_stats.second.m_version;
                stats_names.emplace_back(table_stats.first);
                stats_values.emplace_back();
                dumpStats(table_stats.second, stats_values.back());
            }
        }
        for (size_t i = 0; i < stats_names.size(); i++)
        {
            m_counter_table->set(stats_names[i], stats_values[i]);
        }
        this_thread::sleep_for(chrono::seconds(m_interval));
    }
}
