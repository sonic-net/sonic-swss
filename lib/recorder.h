#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <sys/time.h>

#include "table.h"

namespace swss {

class RecBase {
public:
    RecBase() = default;
    /* Setters */
    void setRecord(bool record)  { m_recording = record; }
    void setRotate(bool rotate)  { m_rotate = rotate; }
    void setLocation(const std::string& loc) { m_location = loc; }
    void setFileName(const std::string& name) { m_filename = name; }
    void setName(const std::string& name)  { m_name = name; }

    /* getters */
    bool isRecord()  { return m_recording; }
    bool isRotate()  { return m_rotate; }
    std::string getLoc() { return m_location; }
    std::string getFile() { return m_filename; }
    std::string getName() { return m_name; }

private:
    bool m_recording;
    bool m_rotate;
    std::string m_location;
    std::string m_filename;
    std::string m_name;
};

class RecWriter : public RecBase {
public:
    RecWriter() = default;
    virtual ~RecWriter();
    void startRec(bool exit_if_failure);
    void record(const std::string& val);
    void record(const std::string& timestamp, const std::string& val);

protected:
    void logfileReopen();

private:
    std::ofstream record_ofs;
    std::string fname;
};

class RetryRec : public RecWriter {
public:
    RetryRec();
};

struct AsyncSwssRecorderDebugStats
{
    uint64_t pending_count;
    uint64_t high_watermark;
    uint64_t enqueued_total;
    uint64_t drained_total;
};

class SwSSRec : public RecWriter {
public:
    SwSSRec();
    ~SwSSRec() override;

    void setAsync(bool enabled);
    bool isAsyncEnabled() const;
    void recordTupleAsync(const std::string& prefix, const KeyOpFieldsValuesTuple& tuple);
    void recordTuplesAsync(const std::string& prefix, const std::deque<KeyOpFieldsValuesTuple>& entries);
    AsyncSwssRecorderDebugStats getAsyncDebugStats() const;
    void dumpAsyncSignalSafeStats(int fd, int signo) const;

private:
    struct AsyncSwssRecordEntry
    {
        struct timeval received_time;
        std::string prefix;
        KeyOpFieldsValuesTuple tuple;
    };

    void ensureAsyncWorkerLocked();
    void stopAsyncWorker();
    void onEnqueue(size_t count);
    void onDrain();
    std::string formatTimestamp(const struct timeval& tv) const;
    std::string serialize(const AsyncSwssRecordEntry& entry) const;
    void drain();

    static size_t appendLiteral(char *buffer, size_t pos, const char *text, size_t capacity);
    static size_t appendUnsigned(char *buffer, size_t pos, uint64_t value, size_t capacity);

    std::atomic<bool> m_asyncEnabled{false};
    bool m_shutdown = false;
    bool m_workerStarted = false;
    mutable std::atomic<uint64_t> m_pendingCount{0};
    mutable std::atomic<uint64_t> m_highWatermark{0};
    mutable std::atomic<uint64_t> m_enqueuedTotal{0};
    mutable std::atomic<uint64_t> m_drainedTotal{0};
    std::mutex m_stateMutex;
    std::mutex m_mutex;
    std::condition_variable m_signal;
    std::deque<AsyncSwssRecordEntry> m_queue;
    std::thread m_worker;
};

/* Record Handler for Response Publisher Class */
class ResPubRec : public RecWriter {
public:
    ResPubRec();
};

class SaiRedisRec : public RecBase {
public:
    SaiRedisRec();
};

/* Interface to access recorder classes */
class Recorder {
public:
    static Recorder& Instance();
    static const std::string DEFAULT_DIR;
    static const std::string REC_START;
    static const std::string SWSS_FNAME;
    static const std::string SAIREDIS_FNAME;
    static const std::string RESPPUB_FNAME;
    static const std::string RETRY_FNAME;

    Recorder() = default;
    /* Individual Handlers */
    SwSSRec swss;
    SaiRedisRec sairedis;
    ResPubRec respub;
    RetryRec retry;
};

AsyncSwssRecorderDebugStats getAsyncSwssRecorderDebugStats();
void dumpAsyncSwssRecorderSignalSafeStats(int fd, int signo);

}
