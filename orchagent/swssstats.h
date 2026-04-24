#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <cstdint>

// Forward-declare the common library type so users of this header don't need
// to pull in swss-common transitively.
namespace swss { class ComponentStats; }

/// Global enable flag. Setting it to false disables recording on the hot path
/// without tearing down the singleton or its writer thread.
extern std::atomic<bool> gSwssStatsRecord;

/**
 * SwssStats — a thin orchagent-specific facade over swss::ComponentStats.
 *
 * The generic plumbing (atomic counters, version-based dirty tracking, writer
 * thread, deferred DB connect, cv-based shutdown) lives in
 * sonic-swss-common/common/componentstats.{h,cpp}. This class owns only the
 * swss-specific metric vocabulary (SET / DEL / COMPLETE / ERROR) and the
 * Redis key prefix ("SWSS_STATS:<table>").
 *
 * Other SONiC containers (gnmi, bmp, telemetry...) can get the same storage
 * + flushing behaviour by calling swss::ComponentStats::create("<name>")
 * directly with their own metric names.
 */
class SwssStats
{
public:
    /// Snapshot shape used by unit tests and diagnostics.
    struct CounterSnapshot
    {
        uint64_t set_count      = 0;
        uint64_t del_count      = 0;
        uint64_t complete_count = 0;
        uint64_t error_count    = 0;
    };

    static SwssStats* getInstance();

    void recordTask(const std::string &table_name, const std::string &op);
    void recordComplete(const std::string &table_name, uint64_t count = 1);
    void recordError(const std::string &table_name, uint64_t count = 1);

    CounterSnapshot getCounters(const std::string &table_name);

private:
    SwssStats();
    ~SwssStats() = default;

    std::shared_ptr<swss::ComponentStats> m_impl;
};
