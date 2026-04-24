#include "swssstats.h"

#include "componentstats.h"
#include "logger.h"

#include <atomic>

// Enable SwssStats recording by default. Flip to false to silence the hot path
// without tearing down the singleton or its writer thread.
std::atomic<bool> gSwssStatsRecord(true);

namespace {
// Metric names written into COUNTERS_DB under SWSS_STATS:<table>.
// Keep these in sync with any dashboards or collectors consuming the table.
constexpr const char *kMetricSet      = "SET";
constexpr const char *kMetricDel      = "DEL";
constexpr const char *kMetricComplete = "COMPLETE";
constexpr const char *kMetricError    = "ERROR";
} // namespace

SwssStats* SwssStats::getInstance()
{
    static SwssStats instance;
    return &instance;
}

SwssStats::SwssStats()
    : m_impl(swss::ComponentStats::create("SWSS"))
{
    SWSS_LOG_ENTER();
}

void SwssStats::recordTask(const std::string &table_name, const std::string &op)
{
    if (op == kMetricSet)
    {
        m_impl->increment(table_name, kMetricSet);
    }
    else if (op == kMetricDel)
    {
        m_impl->increment(table_name, kMetricDel);
    }
    // Any other op is silently ignored so orchagent callers never need to
    // filter op strings.
}

void SwssStats::recordComplete(const std::string &table_name, uint64_t count)
{
    m_impl->increment(table_name, kMetricComplete, count);
}

void SwssStats::recordError(const std::string &table_name, uint64_t count)
{
    m_impl->increment(table_name, kMetricError, count);
}

SwssStats::CounterSnapshot SwssStats::getCounters(const std::string &table_name)
{
    CounterSnapshot snap;
    auto all = m_impl->getAll(table_name);
    auto pick = [&](const char *k) -> uint64_t {
        auto it = all.find(k);
        return (it == all.end()) ? 0 : it->second;
    };
    snap.set_count      = pick(kMetricSet);
    snap.del_count      = pick(kMetricDel);
    snap.complete_count = pick(kMetricComplete);
    snap.error_count    = pick(kMetricError);
    return snap;
}
