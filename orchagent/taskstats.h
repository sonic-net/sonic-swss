#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

// Per-Executor run-time/scheduling statistics and the RAII timer that
// records them. Header-only so both OrchDaemon (select-loop execute path)
// and Orch::doTask() (idle-sweep path) can charge work to the same slots.

// P-square quantile estimator (Jain & Chlamtac, 1985).
// O(1) memory, O(1) per update; tracks an estimate of a single quantile p
// over a stream without keeping the samples. Used here to track Q1, median,
// and Q3 of per-Executor run times so the median is robust to tail outliers
// and so high/low outliers can be classified by the Tukey 1.5*IQR rule
// without buffering history.
class P2Quantile
{
public:
    explicit P2Quantile(double p) : m_p(p) {}

    // Insert a sample and update the quantile estimate.
    void add(double x)
    {
        // Bootstrap: hold the first 5 samples, sort them on the 5th to
        // initialize the markers.
        if (!m_bootstrapped)
        {
            m_init_buf[m_n++] = x;
            if (m_n == 5)
            {
                std::sort(m_init_buf, m_init_buf + 5);
                for (int i = 0; i < 5; ++i)
                {
                    m_q[i] = m_init_buf[i];
                    m_pos[i] = i + 1;          // 1..5 (1-based)
                }
                m_np[0] = 1.0;
                m_np[1] = 1.0 + 2.0 * m_p;
                m_np[2] = 1.0 + 4.0 * m_p;
                m_np[3] = 3.0 + 2.0 * m_p;
                m_np[4] = 5.0;
                m_dn[0] = 0.0;
                m_dn[1] = m_p / 2.0;
                m_dn[2] = m_p;
                m_dn[3] = (1.0 + m_p) / 2.0;
                m_dn[4] = 1.0;
                m_bootstrapped = true;
            }
            return;
        }

        // Find the cell k that x falls into. Adjust q[0] / q[4] if x is a
        // new extremum.
        int k;
        if (x < m_q[0])      { m_q[0] = x; k = 0; }
        else if (x >= m_q[4]) { m_q[4] = x; k = 3; }
        else
        {
            for (k = 0; k < 4; ++k)
                if (m_q[k] <= x && x < m_q[k + 1]) break;
        }

        // Increment positions of markers above k.
        for (int i = k + 1; i < 5; ++i) m_pos[i] += 1.0;

        // Update desired positions for all markers.
        for (int i = 0; i < 5; ++i) m_np[i] += m_dn[i];

        // Adjust the three interior markers if needed.
        for (int i = 1; i < 4; ++i)
        {
            double d = m_np[i] - m_pos[i];
            if ((d >=  1.0 && (m_pos[i + 1] - m_pos[i]) >  1.0) ||
                (d <= -1.0 && (m_pos[i - 1] - m_pos[i]) < -1.0))
            {
                int s = (d >= 0.0) ? 1 : -1;
                double qp = parabolic(i, s);
                if (m_q[i - 1] < qp && qp < m_q[i + 1])
                    m_q[i] = qp;
                else
                    m_q[i] = linear(i, s);
                m_pos[i] += s;
            }
        }
    }

    // Whether the estimator has seen the >=5 samples needed to bootstrap.
    bool ready() const { return m_bootstrapped; }

    // Best-effort estimate of the tracked quantile. Before bootstrap it
    // sorts the (<=5) buffered samples in place; callers that care about
    // stability should gate on ready().
    double value() const
    {
        if (m_bootstrapped) return m_q[2];
        if (m_n == 0) return 0.0;
        // Manual selection sort over a bounded prefix — keeps GCC's
        // array-bounds analysis happy under -Werror.
        double tmp[5] = {0, 0, 0, 0, 0};
        const int n = (m_n < 5) ? m_n : 5;
        for (int i = 0; i < n; ++i) tmp[i] = m_init_buf[i];
        for (int i = 0; i + 1 < n; ++i)
        {
            int best = i;
            for (int j = i + 1; j < n; ++j)
                if (tmp[j] < tmp[best]) best = j;
            if (best != i)
            {
                double t = tmp[i]; tmp[i] = tmp[best]; tmp[best] = t;
            }
        }
        int idx = static_cast<int>(std::round(m_p * (n - 1)));
        if (idx < 0) idx = 0;
        if (idx > n - 1) idx = n - 1;
        return tmp[idx];
    }

    void reset()
    {
        m_n = 0;
        m_bootstrapped = false;
        for (int i = 0; i < 5; ++i)
        {
            m_q[i] = 0.0;
            m_pos[i] = 0.0;
            m_np[i] = 0.0;
            m_dn[i] = 0.0;
            m_init_buf[i] = 0.0;
        }
    }

private:
    double parabolic(int i, int s) const
    {
        double t1 = static_cast<double>(s) / (m_pos[i + 1] - m_pos[i - 1]);
        double t2 = (m_pos[i] - m_pos[i - 1] + s) * (m_q[i + 1] - m_q[i])
                    / (m_pos[i + 1] - m_pos[i]);
        double t3 = (m_pos[i + 1] - m_pos[i] - s) * (m_q[i] - m_q[i - 1])
                    / (m_pos[i] - m_pos[i - 1]);
        return m_q[i] + t1 * (t2 + t3);
    }

    double linear(int i, int s) const
    {
        return m_q[i] + s * (m_q[i + s] - m_q[i]) / (m_pos[i + s] - m_pos[i]);
    }

    double m_p;
    double m_q[5]        = {0, 0, 0, 0, 0};   // marker heights
    double m_pos[5]      = {0, 0, 0, 0, 0};   // current marker positions (1-based)
    double m_np[5]       = {0, 0, 0, 0, 0};   // desired positions
    double m_dn[5]       = {0, 0, 0, 0, 0};   // desired position increments
    double m_init_buf[5] = {0, 0, 0, 0, 0};
    int    m_n            = 0;
    bool   m_bootstrapped = false;
};

struct ExecutorStat
{
    // Don't classify outliers until P² has seen enough samples for the
    // IQR estimate to stabilize. Until then the bootstrap markers can
    // collapse to identical heights (IQR=0) and the Tukey rule flags
    // every non-equal sample as an outlier.
    static constexpr uint64_t OUTLIER_WARMUP = 30;

    // ---------- Per-run wall-clock duration ----------
    uint64_t count    = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns   = 0;

    // Robust quantile estimators for run time. The median is what the
    // CLI reports as the headline number; q1/q3 also drive the Tukey
    // rule and feed the show table's quartile column.
    P2Quantile q1{0.25};
    P2Quantile median{0.50};
    P2Quantile q3{0.75};

    // Tukey 1.5*IQR outlier counts: x > Q3 + 1.5*IQR (high) or
    // x < Q1 - 1.5*IQR (low). Classified before the new sample is folded
    // into the estimators so the estimators self-converge regardless.
    uint64_t high_outliers = 0;
    uint64_t low_outliers  = 0;

    // ---------- Scheduling latency ----------
    // Time between the previous run's TaskTimer dtor (i.e. when this
    // Executor finished the last run() and yielded back to the select
    // loop) and the next TaskTimer ctor (next time the loop scheduled
    // it). 0 = "no prior return yet"; uses steady_clock ticks so only
    // the delta matters.
    uint64_t last_return_ns = 0;
    uint64_t sched_count    = 0;
    uint64_t total_sched_ns = 0;
    uint64_t sched_max_ns   = 0;
    P2Quantile sched_q1{0.25};
    P2Quantile sched_median{0.50};
    P2Quantile sched_q3{0.75};

    void record_run(uint64_t ns)
    {
        ++count;
        total_ns += ns;
        if (ns > max_ns) max_ns = ns;

        if (count >= OUTLIER_WARMUP && median.ready())
        {
            double q1v = q1.value();
            double q3v = q3.value();
            double iqr = q3v - q1v;
            double xd  = static_cast<double>(ns);
            if (iqr > 0.0)
            {
                if (xd > q3v + 1.5 * iqr)      ++high_outliers;
                else if (xd < q1v - 1.5 * iqr) ++low_outliers;
            }
        }

        double xd = static_cast<double>(ns);
        q1.add(xd);
        median.add(xd);
        q3.add(xd);
    }

    void record_sched(uint64_t ns)
    {
        ++sched_count;
        total_sched_ns += ns;
        if (ns > sched_max_ns) sched_max_ns = ns;
        double xd = static_cast<double>(ns);
        sched_q1.add(xd);
        sched_median.add(xd);
        sched_q3.add(xd);
    }

    // Backwards-compatible name kept for any caller still on the old
    // single-record path; equivalent to record_run().
    void record(uint64_t ns) { record_run(ns); }

    void reset()
    {
        count = 0;
        total_ns = 0;
        max_ns = 0;
        q1.reset();
        median.reset();
        q3.reset();
        high_outliers = 0;
        low_outliers  = 0;

        last_return_ns = 0;
        sched_count    = 0;
        total_sched_ns = 0;
        sched_max_ns   = 0;
        sched_q1.reset();
        sched_median.reset();
        sched_q3.reset();
    }
};

class TaskTimer
{
public:
    explicit TaskTimer(ExecutorStat& slot)
        : t0_(std::chrono::steady_clock::now()), slot_(slot)
    {
        // Scheduling latency: gap between the previous run's dtor
        // (last_return_ns) and now (t0_). Skip on the first invocation
        // for a given slot — there's no "previous" yet.
        if (slot_.last_return_ns != 0)
        {
            uint64_t now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    t0_.time_since_epoch()).count());
            if (now_ns > slot_.last_return_ns)
            {
                slot_.record_sched(now_ns - slot_.last_return_ns);
            }
        }
    }

    ~TaskTimer()
    {
        auto end_tp = std::chrono::steady_clock::now();
        uint64_t run_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end_tp - t0_).count());
        slot_.record_run(run_ns);
        slot_.last_return_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end_tp.time_since_epoch()).count());
    }

    TaskTimer(const TaskTimer&) = delete;
    TaskTimer& operator=(const TaskTimer&) = delete;

private:
    std::chrono::steady_clock::time_point t0_;
    ExecutorStat& slot_;
};
