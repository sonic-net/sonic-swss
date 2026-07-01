#include "swssstats.h"

#include "componentstats.h"
#include "logger.h"
#include "schema.h"   // SET_COMMAND / DEL_COMMAND

#include <atomic>

namespace {
// Metric names written into COUNTERS_DB under SWSS_STATS:<table>.
// These are the on-the-wire field names for dashboards / collectors and
// must NOT be replaced with SET_COMMAND / DEL_COMMAND (those are the swss
// op-string vocabulary, which is allowed to drift independently).
constexpr const char *kMetricSet      = "SET";
constexpr const char *kMetricDel      = "DEL";
constexpr const char *kMetricComplete = "COMPLETE";
constexpr const char *kMetricError    = "ERROR";
} // namespace

// Recording is enabled by default. Toggle via SwssStats::setEnabled().
std::atomic<bool> SwssStats::s_enabled{true};

void SwssStats::setEnabled(bool on)
{
    s_enabled.store(on, std::memory_order_relaxed);
}

bool SwssStats::isEnabled()
{
    return s_enabled.load(std::memory_order_relaxed);
}

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
    if (!isEnabled())
    {
        return;
    }
    if (op == SET_COMMAND)
    {
        m_impl->increment(table_name, kMetricSet);
    }
    else if (op == DEL_COMMAND)
    {
        m_impl->increment(table_name, kMetricDel);
    }
    // Any other op is silently ignored so orchagent callers never need to
    // filter op strings.
}

void SwssStats::recordComplete(const std::string &table_name, uint64_t count)
{
    if (!isEnabled())
    {
        return;
    }
    m_impl->increment(table_name, kMetricComplete, count);
}

void SwssStats::recordError(const std::string &table_name, uint64_t count)
{
    if (!isEnabled())
    {
        return;
    }
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
