#include "recorder.h"
#include "timestamp.h"
#include "logger.h"
#include <cstring>
#include <inttypes.h>
#include <unistd.h>

using namespace swss;

const std::string Recorder::DEFAULT_DIR = ".";
const std::string Recorder::REC_START = "|recording started";
const std::string Recorder::SWSS_FNAME = "swss.rec";
const std::string Recorder::SAIREDIS_FNAME = "sairedis.rec";
const std::string Recorder::RESPPUB_FNAME = "responsepublisher.rec";
const std::string Recorder::RETRY_FNAME = "retry.rec";

Recorder& Recorder::Instance()
{
    static Recorder m_recorder;
    return m_recorder;
}

AsyncSwssRecorderDebugStats swss::getAsyncSwssRecorderDebugStats()
{
    return Recorder::Instance().swss.getAsyncDebugStats();
}

void swss::dumpAsyncSwssRecorderSignalSafeStats(int fd, int signo)
{
    Recorder::Instance().swss.dumpAsyncSignalSafeStats(fd, signo);
}


RetryRec::RetryRec() 
{
    /* Set Default values */
    setRecord(true);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::RETRY_FNAME);
    setName("Retry");
}


SwSSRec::SwSSRec() 
{
    /* Set Default values */
    setRecord(true);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::SWSS_FNAME);
    setName("SwSS");
}

SwSSRec::~SwSSRec()
{
    stopAsyncWorker();
}

void SwSSRec::setAsync(bool enabled)
{
    bool previous = m_asyncEnabled.exchange(enabled, std::memory_order_relaxed);
    if (previous == enabled)
    {
        return;
    }

    if (!enabled)
    {
        stopAsyncWorker();
    }
}

bool SwSSRec::isAsyncEnabled() const
{
    return m_asyncEnabled.load(std::memory_order_relaxed);
}

void SwSSRec::recordTupleAsync(const std::string& prefix, const KeyOpFieldsValuesTuple& tuple)
{
    if (!m_asyncEnabled.load(std::memory_order_relaxed))
    {
        AsyncSwssRecordEntry entry = {{}, prefix, tuple};
        record(serialize(entry));
        return;
    }

    if (!isRecord())
    {
        return;
    }
    struct timeval received_time;
    gettimeofday(&received_time, nullptr);

    {
        std::unique_lock<std::mutex> stateLock(m_stateMutex);

        if (!m_asyncEnabled.load(std::memory_order_relaxed))
        {
            stateLock.unlock();
            AsyncSwssRecordEntry entry = {{}, prefix, tuple};
            record(serialize(entry));
            return;
        }

        ensureAsyncWorkerLocked();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back({received_time, prefix, tuple});
        onEnqueue(1);
    }

    m_signal.notify_one();
}

void SwSSRec::recordTuplesAsync(const std::string& prefix, const std::deque<KeyOpFieldsValuesTuple>& entries)
{
    if (!m_asyncEnabled.load(std::memory_order_relaxed))
    {
        for (const auto& entry : entries)
        {
            record(serialize({{}, prefix, entry}));
        }
        return;
    }

    if (!isRecord() || entries.empty())
    {
        return;
    }
    {
        std::unique_lock<std::mutex> stateLock(m_stateMutex);

        if (!m_asyncEnabled.load(std::memory_order_relaxed))
        {
            stateLock.unlock();
            for (const auto& entry : entries)
            {
                record(serialize({{}, prefix, entry}));
            }
            return;
        }

        ensureAsyncWorkerLocked();

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : entries)
        {
            struct timeval received_time;
            gettimeofday(&received_time, nullptr);
            m_queue.push_back({received_time, prefix, entry});
        }
        onEnqueue(entries.size());
    }

    m_signal.notify_one();
}

AsyncSwssRecorderDebugStats SwSSRec::getAsyncDebugStats() const
{
    return {
        m_pendingCount.load(std::memory_order_relaxed),
        m_highWatermark.load(std::memory_order_relaxed),
        m_enqueuedTotal.load(std::memory_order_relaxed),
        m_drainedTotal.load(std::memory_order_relaxed),
    };
}

void SwSSRec::dumpAsyncSignalSafeStats(int fd, int signo) const
{
    auto stats = getAsyncDebugStats();
    char buffer[256];
    size_t pos = 0;

    pos = appendLiteral(buffer, pos, "AsyncSwssRecorderStats signal=", sizeof(buffer));
    pos = appendUnsigned(buffer, pos, static_cast<uint64_t>(signo), sizeof(buffer));
    pos = appendLiteral(buffer, pos, " pending=", sizeof(buffer));
    pos = appendUnsigned(buffer, pos, stats.pending_count, sizeof(buffer));
    pos = appendLiteral(buffer, pos, " high_watermark=", sizeof(buffer));
    pos = appendUnsigned(buffer, pos, stats.high_watermark, sizeof(buffer));
    pos = appendLiteral(buffer, pos, " enqueued=", sizeof(buffer));
    pos = appendUnsigned(buffer, pos, stats.enqueued_total, sizeof(buffer));
    pos = appendLiteral(buffer, pos, " drained=", sizeof(buffer));
    pos = appendUnsigned(buffer, pos, stats.drained_total, sizeof(buffer));
    pos = appendLiteral(buffer, pos, "\n", sizeof(buffer));

    ssize_t bytes_written = write(fd, buffer, pos);
    if (bytes_written < 0)
    {
        /* Best effort only in fatal-signal context. */
    }
}

void SwSSRec::ensureAsyncWorkerLocked()
{
    if (m_workerStarted || !m_asyncEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = false;
    }
    m_worker = std::thread(&SwSSRec::drain, this);
    m_workerStarted = true;
}

void SwSSRec::stopAsyncWorker()
{
    std::unique_lock<std::mutex> stateLock(m_stateMutex);

    if (!m_workerStarted)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }

    m_signal.notify_one();
    m_worker.join();
    m_workerStarted = false;
    stateLock.unlock();

    auto stats = getAsyncDebugStats();
    SWSS_LOG_INFO(
        "AsyncSwssRecorder graceful shutdown: pending_after_shutdown=%" PRIu64
        " drained_total=%" PRIu64 " high_watermark=%" PRIu64,
        stats.pending_count,
        stats.drained_total,
        stats.high_watermark);
}

void SwSSRec::onEnqueue(size_t count)
{
    auto depth = m_pendingCount.fetch_add(count, std::memory_order_relaxed) + count;
    m_enqueuedTotal.fetch_add(count, std::memory_order_relaxed);

    auto current = m_highWatermark.load(std::memory_order_relaxed);
    while (current < depth &&
           !m_highWatermark.compare_exchange_weak(current, depth, std::memory_order_relaxed))
    {
    }
}

void SwSSRec::onDrain()
{
    m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
    m_drainedTotal.fetch_add(1, std::memory_order_relaxed);
}

std::string SwSSRec::formatTimestamp(const struct timeval& tv) const
{
    char buffer[64];
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);

    size_t size = strftime(buffer, 32, "%Y-%m-%d.%T.", &tm_info);
    snprintf(&buffer[size], 32, "%06" PRIu64, static_cast<uint64_t>(tv.tv_usec));

    return std::string(buffer);
}

std::string SwSSRec::serialize(const AsyncSwssRecordEntry& entry) const
{
    std::string s = entry.prefix + kfvKey(entry.tuple) + "|" + kfvOp(entry.tuple);
    for (const auto& fv : kfvFieldsValues(entry.tuple))
    {
        s += "|" + fvField(fv) + ":" + fvValue(fv);
    }
    return s;
}

void SwSSRec::drain()
{
    while (true)
    {
        std::deque<AsyncSwssRecordEntry> pending;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_signal.wait(lock, [this]() {
                return m_shutdown || !m_queue.empty();
            });

            if (m_shutdown && m_queue.empty())
            {
                break;
            }

            pending.swap(m_queue);
        }

        for (const auto& entry : pending)
        {
            record(formatTimestamp(entry.received_time), serialize(entry));
            onDrain();
        }
    }
}

size_t SwSSRec::appendLiteral(char *buffer, size_t pos, const char *text, size_t capacity)
{
    while (*text != '\0' && pos < capacity)
    {
        buffer[pos++] = *text++;
    }

    return pos;
}

size_t SwSSRec::appendUnsigned(char *buffer, size_t pos, uint64_t value, size_t capacity)
{
    char digits[32];
    size_t count = 0;

    do
    {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));

    while (count > 0 && pos < capacity)
    {
        buffer[pos++] = digits[--count];
    }

    return pos;
}


ResPubRec::ResPubRec() 
{
    /* Set Default values */
    setRecord(false);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::RESPPUB_FNAME);
    setName("Response Publisher");
}


SaiRedisRec::SaiRedisRec() 
{
    /* Set Default values */
    setRecord(true);
    setRotate(false);
    setLocation(Recorder::DEFAULT_DIR);
    setFileName(Recorder::SAIREDIS_FNAME);
    setName("SaiRedis");
}


void RecWriter::startRec(bool exit_if_failure)
{
    if (!isRecord())
    {
        return ;
    }

    fname = getLoc() + "/" + getFile();
    record_ofs.open(fname, std::ofstream::out | std::ofstream::app);
    if (!record_ofs.is_open())
    {
        SWSS_LOG_ERROR("%s Recorder: Failed to open recording file %s: error %s", getName().c_str(), fname.c_str(), strerror(errno));
        if (exit_if_failure)
        {
            exit(EXIT_FAILURE);
        }
        else
        {
            setRecord(false);
        }
    }
    record_ofs << swss::getTimestamp() << Recorder::REC_START << std::endl;
    SWSS_LOG_NOTICE("%s Recorder: Recording started at %s", getName().c_str(), fname.c_str());
}


RecWriter::~RecWriter()
{
    if (record_ofs.is_open())
    {
        record_ofs.close();      
    }
}


void RecWriter::record(const std::string& val)
{
    record(swss::getTimestamp(), val);
}

void RecWriter::record(const std::string& timestamp, const std::string& val)
{
    if (!isRecord())
    {
        return ;
    }
    if (isRotate())
    {
        setRotate(false);
        logfileReopen();
    }
    record_ofs << timestamp << "|" << val << std::endl;
}


void RecWriter::logfileReopen()
{
    /*
     * On log rotate we will use the same file name, we are assuming that
     * logrotate daemon move filename to filename.1 and we will create new
     * empty file here.
     */
    record_ofs.close();
    record_ofs.open(fname, std::ofstream::out | std::ofstream::app);

    if (!record_ofs.is_open())
    {
        SWSS_LOG_ERROR("%s Recorder: Failed to open file %s: %s", getName().c_str(), fname.c_str(), strerror(errno));
        return;
    }
    SWSS_LOG_INFO("%s Recorder: LogRotate request handled", getName().c_str());
}
