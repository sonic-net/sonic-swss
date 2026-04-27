#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <cstdint>

// Forward-declare the common library type so users of this header don't need
// to pull in swss-common transitively.
namespace swss { class ComponentStats; }

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

    /// Globally enable / disable recording. Disabling makes record* calls
    /// no-ops but does not tear down the singleton or its writer thread.
    /// Hot-path callers should still gate on isEnabled() before computing
    /// anything expensive (e.g. a container size); these methods themselves
    /// also re-check the flag for defense in depth.
    static void setEnabled(bool on);
    static bool isEnabled();

    /// Increments SET or DEL on the given table depending on `op`.
    /// Only "SET" / "DEL" are counted; any other op is silently ignored so
    /// callers never need to filter op strings. NOT called on retried tasks
    /// (orch.cpp gates on !onRetry), so the counter measures fresh task
    /// arrivals, not total processing attempts.
    void recordTask(const std::string &table_name, const std::string &op);

    /// Adds `count` to the COMPLETE counter for the given table.
    void recordComplete(const std::string &table_name, uint64_t count = 1);

    /// Adds `count` to the ERROR counter. Currently called once per failing
    /// drain pass (with count=1) — i.e. ERROR measures "drain attempts that
    /// threw", not the number of tasks that failed. Items left undrained
    /// after an exception will be retried on the next drain pass and may
    /// contribute to ERROR again if they fail again.
    void recordError(const std::string &table_name, uint64_t count = 1);

    CounterSnapshot getCounters(const std::string &table_name);

private:
    SwssStats();
    ~SwssStats() = default;

    std::shared_ptr<swss::ComponentStats> m_impl;

    static std::atomic<bool> s_enabled;
};
