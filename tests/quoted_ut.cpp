#include <arpa/inet.h>
#include <signal.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <future>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include "logger.h"
#include "recorder.h"
#include "recorder_test_helper.h"
#include "swssnet.h"
#include "cfgmgr/shellcmd.h"

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define DEFAULT_VLAN_ID     "1"

namespace
{

vector<string> waitForRecordedLines(
        const string& path,
        const string& marker,
        size_t expected,
        size_t maxRetries = 50)
{
    for (size_t retry = 0; retry < maxRetries; ++retry)
    {
        ifstream ifs(path);
        vector<string> matches;
        string line;

        while (getline(ifs, line))
        {
            if (line.find(marker) != string::npos)
            {
                matches.push_back(line);
            }
        }

        if (matches.size() >= expected)
        {
            return matches;
        }

        this_thread::sleep_for(chrono::milliseconds(20));
    }

    return {};
}

AsyncSwssRecorderDebugStats waitForRecorderStats(
        SwSSRec& recorder,
        uint64_t expectedEnqueued,
        uint64_t expectedDrained,
        size_t maxRetries = 50)
{
    for (size_t retry = 0; retry < maxRetries; ++retry)
    {
        auto stats = recorder.getAsyncDebugStats();
        if (stats.enqueued_total >= expectedEnqueued &&
            stats.drained_total >= expectedDrained)
        {
            return stats;
        }

        this_thread::sleep_for(chrono::milliseconds(20));
    }

    return recorder.getAsyncDebugStats();
}

using swss_test::ScopedRecorderWorkDir;

static constexpr char kPendingThresholdWarnMarker[] =
        "AsyncSwssRecorder queue depth exceeded threshold";

size_t countOccurrences(const string& haystack, const string& needle)
{
    if (needle.empty())
    {
        return 0;
    }

    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != string::npos)
    {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Captures SWSS logs written to stderr for the duration of a gtest (restores on exit).
class ScopedStderrLogCapture
{
public:
    bool isActive() const
    {
        return m_active;
    }

    ScopedStderrLogCapture()
    {
        if (pipe(m_pipefd) != 0)
        {
            return;
        }

        // Non-blocking read end: the write ends stay open (including stderr itself),
        // so a blocking read() would hang forever once the pipe is empty.
        const int readFlags = fcntl(m_pipefd[0], F_GETFL, 0);
        if (readFlags < 0 || fcntl(m_pipefd[0], F_SETFL, readFlags | O_NONBLOCK) != 0)
        {
            close(m_pipefd[0]);
            close(m_pipefd[1]);
            m_pipefd[0] = m_pipefd[1] = -1;
            return;
        }

        m_savedStderr = dup(STDERR_FILENO);
        if (m_savedStderr < 0)
        {
            close(m_pipefd[0]);
            close(m_pipefd[1]);
            m_pipefd[0] = m_pipefd[1] = -1;
            return;
        }

        if (dup2(m_pipefd[1], STDERR_FILENO) != STDERR_FILENO)
        {
            close(m_savedStderr);
            m_savedStderr = -1;
            close(m_pipefd[0]);
            close(m_pipefd[1]);
            m_pipefd[0] = m_pipefd[1] = -1;
            return;
        }

        setvbuf(stderr, nullptr, _IONBF, 0);

        m_savedMinPrio = Logger::getMinPrio();
        Logger::setMinPrio(Logger::SWSS_WARN);
        Logger::swssOutputNotify("quoted_ut", "STDERR");
        m_active = true;
    }

    string drainCapturedOutput()
    {
        if (!m_active)
        {
            return m_captured;
        }

        fflush(stderr);

        char buffer[4096];
        ssize_t bytesRead = 0;
        while ((bytesRead = read(m_pipefd[0], buffer, sizeof(buffer))) > 0)
        {
            m_captured.append(buffer, static_cast<size_t>(bytesRead));
        }

        return m_captured;
    }

    ~ScopedStderrLogCapture()
    {
        if (!m_active)
        {
            return;
        }

        drainCapturedOutput();
        fflush(stderr);
        dup2(m_savedStderr, STDERR_FILENO);
        close(m_savedStderr);
        close(m_pipefd[1]);
        close(m_pipefd[0]);
        Logger::setMinPrio(m_savedMinPrio);
        Logger::swssOutputNotify("quoted_ut", "SYSLOG");
    }

    ScopedStderrLogCapture(const ScopedStderrLogCapture&) = delete;
    ScopedStderrLogCapture& operator=(const ScopedStderrLogCapture&) = delete;

private:
    bool m_active = false;
    int m_pipefd[2] = {-1, -1};
    int m_savedStderr = -1;
    Logger::Priority m_savedMinPrio = Logger::SWSS_NOTICE;
    string m_captured;
};

deque<KeyOpFieldsValuesTuple> makeThresholdTestEntries(size_t count, size_t keyOffset)
{
    deque<KeyOpFieldsValuesTuple> entries;
    for (size_t i = 0; i < count; ++i)
    {
        entries.push_back(KeyOpFieldsValuesTuple(
            { "threshold-key-" + to_string(keyOffset + i),
              SET_COMMAND,
              { { "field1", "value1" } } }));
    }
    return entries;
}

}

TEST(quoted, copy1_v6)
{
    ostringstream cmd;
    string key = "Ethernet0";
    cmd << "ip link set " << shellquote(key) << " down";
    EXPECT_EQ(cmd.str(), "ip link set \"Ethernet0\" down");

    ostringstream cmd2;
    key = "; rm -rf /; echo '\"'";
    cmd2 << "ip link set " << shellquote(key) << " down";
    EXPECT_EQ(cmd2.str(), "ip link set \"; rm -rf /; echo '\\\"'\" down");

    ostringstream cmds, inner;
    string port_alias = "etp1";
    string tagging_cmd = "pvid untagged";
    int vlan_id = 111;
    inner << IP_CMD " link set " << shellquote(port_alias) << " master " DOT1Q_BRIDGE_NAME " && "
      BRIDGE_CMD " vlan del vid " DEFAULT_VLAN_ID " dev " << shellquote(port_alias) << " && "
      BRIDGE_CMD " vlan add vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " " + tagging_cmd;
    cmds << BASH_CMD " -c " << shellquote(inner.str());
    EXPECT_EQ(cmds.str(), "/bin/bash -c \"/sbin/ip link set \\\"etp1\\\" master Bridge && /sbin/bridge vlan del vid 1 dev \\\"etp1\\\" && /sbin/bridge vlan add vid 111 dev \\\"etp1\\\" pvid untagged\"");

    ostringstream cmd4;
    key = "$(echo hi)";
    cmd4 << "cat /sys/class/net/" << shellquote(key) << "/operstate";
    EXPECT_EQ(cmd4.str(), "cat /sys/class/net/\"\\$(echo hi)\"/operstate");
}

TEST(recorder, preservesProvidedTimestamp)
{
    ScopedRecorderWorkDir workDir;
    ASSERT_TRUE(workDir.created());

    const string dirname = workDir.dirname();
    const string filename = "recorder-ut.rec";
    const string fullpath = dirname + "/" + filename;
    workDir.trackFile(fullpath);
    const string timestamp = "2026-03-25.17:13:05.185522";
    const string payload = "CFG_TEST_TABLE:key|SET|field:value";

    RecWriter writer;
    writer.setRecord(true);
    writer.setLocation(dirname);
    writer.setFileName(filename);
    writer.startRec(true);
    writer.record(timestamp, payload);

    ifstream ifs(fullpath);
    ASSERT_TRUE(ifs.is_open());

    string line;
    string last_line;
    while (getline(ifs, line))
    {
        last_line = line;
    }

    EXPECT_EQ(last_line, timestamp + "|" + payload);
}

TEST(swssrec, asyncUpdatesStatsAndWritesRecord)
{
    ScopedRecorderWorkDir workDir;
    ASSERT_TRUE(workDir.created());

    const string dirname = workDir.dirname();
    const string filename = "swss-async-stats.rec";
    const string fullpath = dirname + "/" + filename;
    workDir.trackFile(fullpath);
    const string prefix = "TEST_TABLE:";
    const string expectedPayload = "TEST_TABLE:async-key|SET|field1:value1|field2:value2";

    SwSSRec recorder;
    recorder.setRecord(true);
    recorder.setLocation(dirname);
    recorder.setFileName(filename);
    recorder.setAsync(true);
    recorder.startRec(true);

    KeyOpFieldsValuesTuple tuple(
        { "async-key",
          SET_COMMAND,
          { { "field1", "value1" }, { "field2", "value2" } } });

    recorder.recordTupleAsync(prefix, tuple);

    const auto lines = waitForRecordedLines(fullpath, "async-key", 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find(expectedPayload), string::npos);

    const auto stats = waitForRecorderStats(recorder, 1, 1);
    EXPECT_EQ(stats.enqueued_total, 1u);
    EXPECT_EQ(stats.drained_total, 1u);
    EXPECT_EQ(stats.pending_count, 0u);
    EXPECT_GE(stats.high_watermark, 1u);
}

TEST(swssrec, syncFallbackWhenAsyncDisabled)
{
    ScopedRecorderWorkDir workDir;
    ASSERT_TRUE(workDir.created());

    const string dirname = workDir.dirname();
    const string filename = "swss-sync-fallback.rec";
    const string fullpath = dirname + "/" + filename;
    workDir.trackFile(fullpath);
    const string prefix = "TEST_TABLE:";
    const string keyPrefix = "sync-key-";

    SwSSRec recorder;
    recorder.setRecord(true);
    recorder.setLocation(dirname);
    recorder.setFileName(filename);
    recorder.setAsync(false);
    recorder.startRec(true);

    deque<KeyOpFieldsValuesTuple> entries;
    entries.push_back(KeyOpFieldsValuesTuple(
        { keyPrefix + "0",
          SET_COMMAND,
          { { "field1", "value1" } } }));
    entries.push_back(KeyOpFieldsValuesTuple(
        { keyPrefix + "1",
          SET_COMMAND,
          { { "field2", "value2" } } }));

    recorder.recordTuplesAsync(prefix, entries);

    const auto lines = waitForRecordedLines(fullpath, keyPrefix, 2);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find("TEST_TABLE:sync-key-0|SET|field1:value1"), string::npos);
    EXPECT_NE(lines[1].find("TEST_TABLE:sync-key-1|SET|field2:value2"), string::npos);

    const auto stats = recorder.getAsyncDebugStats();
    EXPECT_EQ(stats.enqueued_total, 0u);
    EXPECT_EQ(stats.drained_total, 0u);
    EXPECT_EQ(stats.pending_count, 0u);
    EXPECT_EQ(stats.high_watermark, 0u);
}

TEST(swssrec, setAsyncDisableWithoutStartedWorkerKeepsStatsClear)
{
    SwSSRec recorder;

    recorder.setAsync(true);
    EXPECT_TRUE(recorder.isAsyncEnabled());

    recorder.setAsync(false);
    EXPECT_FALSE(recorder.isAsyncEnabled());

    // Repeating the same state should take the no-op early return path.
    recorder.setAsync(false);
    EXPECT_FALSE(recorder.isAsyncEnabled());

    const auto stats = recorder.getAsyncDebugStats();
    EXPECT_EQ(stats.enqueued_total, 0u);
    EXPECT_EQ(stats.drained_total, 0u);
    EXPECT_EQ(stats.pending_count, 0u);
    EXPECT_EQ(stats.high_watermark, 0u);
}

TEST(swssrec, setAsyncDisableDrainsWorkerAndSingleTupleFallsBackToSync)
{
    ScopedRecorderWorkDir workDir;
    ASSERT_TRUE(workDir.created());

    const string dirname = workDir.dirname();
    const string filename = "swss-disable-drain.rec";
    const string fullpath = dirname + "/" + filename;
    workDir.trackFile(fullpath);
    const string prefix = "TEST_TABLE:";

    {
        SwSSRec recorder;
        recorder.setRecord(true);
        recorder.setLocation(dirname);
        recorder.setFileName(filename);
        recorder.setAsync(true);
        recorder.startRec(true);

        KeyOpFieldsValuesTuple asyncTuple(
            { "async-key",
              SET_COMMAND,
              { { "field1", "value1" } } });

        recorder.recordTupleAsync(prefix, asyncTuple);
        const auto asyncLines = waitForRecordedLines(fullpath, "async-key", 1);
        ASSERT_EQ(asyncLines.size(), 1u);

        const auto drainedStats = waitForRecorderStats(recorder, 1, 1);
        EXPECT_EQ(drainedStats.pending_count, 0u);

        recorder.setAsync(false);
        EXPECT_FALSE(recorder.isAsyncEnabled());

        KeyOpFieldsValuesTuple syncTuple(
            { "sync-key",
              SET_COMMAND,
              { { "field2", "value2" } } });

        recorder.recordTupleAsync(prefix, syncTuple);
        const auto syncLines = waitForRecordedLines(fullpath, "TEST_TABLE:sync-key|SET", 1);
        ASSERT_EQ(syncLines.size(), 1u);
        EXPECT_NE(syncLines[0].find("TEST_TABLE:sync-key|SET|field2:value2"), string::npos);

        const auto finalStats = recorder.getAsyncDebugStats();
        EXPECT_EQ(finalStats.enqueued_total, 1u);
        EXPECT_EQ(finalStats.drained_total, 1u);
        EXPECT_EQ(finalStats.pending_count, 0u);
        EXPECT_GE(finalStats.high_watermark, 1u);
    }

}

TEST(swssrec, recordTuplesAsyncBulkEnqueue)
{
    ScopedRecorderWorkDir workDir;
    ASSERT_TRUE(workDir.created());

    const string dirname = workDir.dirname();
    const string filename = "swss-bulk-enqueue.rec";
    const string fullpath = dirname + "/" + filename;
    workDir.trackFile(fullpath);
    const string prefix = "TEST_TABLE:";
    const string keyPrefix = "batch-enqueue-key-";
    const size_t batchSize = 1024;

    SwSSRec recorder;
    recorder.setRecord(true);
    recorder.setLocation(dirname);
    recorder.setFileName(filename);
    recorder.setAsync(true);
    recorder.startRec(true);

    deque<KeyOpFieldsValuesTuple> entries;
    for (size_t i = 0; i < batchSize; ++i)
    {
        entries.push_back(KeyOpFieldsValuesTuple(
            { keyPrefix + to_string(i),
              SET_COMMAND,
              { { "field1", "value1" } } }));
    }

    recorder.recordTuplesAsync(prefix, entries);

    const auto stats = waitForRecorderStats(recorder, batchSize, batchSize, 200);
    EXPECT_EQ(stats.enqueued_total, batchSize);
    EXPECT_EQ(stats.drained_total, batchSize);
    EXPECT_EQ(stats.pending_count, 0u);
    EXPECT_GE(stats.high_watermark, batchSize);

    const auto lines = waitForRecordedLines(fullpath, keyPrefix, batchSize, 200);
    ASSERT_EQ(lines.size(), batchSize);

    const string firstMarker = keyPrefix + "0|SET";
    const string lastMarker = keyPrefix + to_string(batchSize - 1) + "|SET";
    bool foundFirst = false;
    bool foundLast = false;
    for (const auto& line : lines)
    {
        if (line.find(firstMarker) != string::npos)
        {
            foundFirst = true;
        }
        if (line.find(lastMarker) != string::npos)
        {
            foundLast = true;
        }
    }
    EXPECT_TRUE(foundFirst);
    EXPECT_TRUE(foundLast);
}

TEST(swssrec, pendingThresholdWarningTracksHighWatermark)
{
    ScopedStderrLogCapture logCapture;
    ASSERT_TRUE(logCapture.isActive());
    ScopedRecorderWorkDir workDir;
    ASSERT_TRUE(workDir.created());

    const string dirname = workDir.dirname();
    const string filename = "swss-pending-threshold.rec";
    const string fullpath = dirname + "/" + filename;
    workDir.trackFile(fullpath);
    const string prefix = "TEST_TABLE:";
    const size_t batchSize =
            static_cast<size_t>(ASYNC_SWSS_RECORDER_PENDING_WARN_THRESHOLD) + 5;
    const size_t concurrentBatch = 256;

    SwSSRec recorder;
    recorder.setRecord(true);
    recorder.setLocation(dirname);
    recorder.setFileName(filename);
    recorder.setAsync(true);
    recorder.startRec(true);

    size_t keyOffset = 0;
    // One batch enqueue crossing the threshold must emit a single WARN (not per entry).
    recorder.recordTuplesAsync(prefix, makeThresholdTestEntries(batchSize, keyOffset));
    keyOffset += batchSize;

    promise<void> ready0;
    promise<void> ready1;
    auto peerReady0 = ready0.get_future();
    auto peerReady1 = ready1.get_future();

    thread concurrentRefill0([&]() {
        ready0.set_value();
        peerReady1.wait();
        recorder.recordTuplesAsync(
                prefix, makeThresholdTestEntries(concurrentBatch, keyOffset));
    });
    thread concurrentRefill1([&]() {
        ready1.set_value();
        peerReady0.wait();
        recorder.recordTuplesAsync(
                prefix,
                makeThresholdTestEntries(concurrentBatch, keyOffset + concurrentBatch));
    });
    concurrentRefill0.join();
    concurrentRefill1.join();

    const size_t firstCycleTotal = batchSize + concurrentBatch * 2;
    const auto stats = waitForRecorderStats(recorder, firstCycleTotal, firstCycleTotal, 500);
    EXPECT_EQ(stats.enqueued_total, firstCycleTotal);
    EXPECT_EQ(stats.drained_total, firstCycleTotal);
    EXPECT_EQ(stats.pending_count, 0u);
    EXPECT_GE(stats.high_watermark, static_cast<uint64_t>(ASYNC_SWSS_RECORDER_PENDING_WARN_THRESHOLD));

    const string afterFirstCycle = logCapture.drainCapturedOutput();
    const size_t firstCycleWarns =
            countOccurrences(afterFirstCycle, kPendingThresholdWarnMarker);
    EXPECT_GE(firstCycleWarns, 1u) << "crossing the threshold must log at least once";

    keyOffset = firstCycleTotal;
    recorder.recordTuplesAsync(prefix, makeThresholdTestEntries(batchSize, keyOffset));
    const size_t secondCycleTotal = firstCycleTotal + batchSize;
    const auto statsAfterRefill = waitForRecorderStats(
            recorder, secondCycleTotal, secondCycleTotal, 200);
    EXPECT_EQ(statsAfterRefill.pending_count, 0u);

    const string afterSecondCycle = logCapture.drainCapturedOutput();
    EXPECT_EQ(countOccurrences(afterSecondCycle, kPendingThresholdWarnMarker),
              firstCycleWarns + 1u)
            << "after hysteresis clear and drain, crossing threshold again should log exactly once more";
}

TEST(swssrec, signalSafeStatsDumpFormatsOutput)
{
    ScopedRecorderWorkDir workDir;
    ASSERT_TRUE(workDir.created());

    const string dirname = workDir.dirname();
    const string filename = "swss-signal-stats.rec";
    const string fullpath = dirname + "/" + filename;
    workDir.trackFile(fullpath);
    const string prefix = "TEST_TABLE:";

    SwSSRec recorder;
    recorder.setRecord(true);
    recorder.setLocation(dirname);
    recorder.setFileName(filename);
    recorder.setAsync(true);
    recorder.startRec(true);

    KeyOpFieldsValuesTuple tuple(
        { "signal-key",
          SET_COMMAND,
          { { "field1", "value1" } } });

    recorder.recordTupleAsync(prefix, tuple);

    const auto lines = waitForRecordedLines(fullpath, "signal-key", 1);
    ASSERT_EQ(lines.size(), 1u);

    const auto stats = waitForRecorderStats(recorder, 1, 1);
    ASSERT_EQ(stats.enqueued_total, 1u);
    ASSERT_EQ(stats.drained_total, 1u);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    recorder.dumpAsyncSignalSafeStats(pipefd[1], SIGABRT);
    close(pipefd[1]);

    char buffer[256] = {};
    ssize_t bytesRead = read(pipefd[0], buffer, sizeof(buffer) - 1);
    close(pipefd[0]);

    ASSERT_GT(bytesRead, 0);
    string output(buffer, static_cast<size_t>(bytesRead));

    EXPECT_NE(output.find("AsyncSwssRecorderStats signal=" + to_string(SIGABRT)), string::npos);
    EXPECT_NE(output.find("pending=0"), string::npos);
    EXPECT_NE(output.find("high_watermark="), string::npos);
    EXPECT_NE(output.find("enqueued=1"), string::npos);
    EXPECT_NE(output.find("drained=1"), string::npos);
}
