#include <arpa/inet.h>
#include <signal.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include "recorder.h"
#include "swssnet.h"
#include "cfgmgr/shellcmd.h"

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define DEFAULT_VLAN_ID     "1"

namespace
{

vector<string> waitForRecordedLines(const string& path, const string& marker, size_t expected)
{
    for (size_t retry = 0; retry < 50; ++retry)
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
        uint64_t expectedDrained)
{
    for (size_t retry = 0; retry < 50; ++retry)
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
    char dir_template[] = "/tmp/swss-recorder-ut-XXXXXX";
    auto dir = mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr);

    const string dirname(dir);
    const string filename = "recorder-ut.rec";
    const string fullpath = dirname + "/" + filename;
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
    char dir_template[] = "/tmp/swss-recorder-ut-XXXXXX";
    auto dir = mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr);

    const string dirname(dir);
    const string filename = "swss-async-stats.rec";
    const string fullpath = dirname + "/" + filename;
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
    char dir_template[] = "/tmp/swss-recorder-ut-XXXXXX";
    auto dir = mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr);

    const string dirname(dir);
    const string filename = "swss-sync-fallback.rec";
    const string fullpath = dirname + "/" + filename;
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
    char dir_template[] = "/tmp/swss-recorder-ut-XXXXXX";
    auto dir = mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr);

    const string dirname(dir);
    const string filename = "swss-disable-drain.rec";
    const string fullpath = dirname + "/" + filename;
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

    ASSERT_EQ(remove(fullpath.c_str()), 0);
    ASSERT_EQ(rmdir(dirname.c_str()), 0);
}

TEST(swssrec, signalSafeStatsDumpFormatsOutput)
{
    char dir_template[] = "/tmp/swss-recorder-ut-XXXXXX";
    auto dir = mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr);

    const string dirname(dir);
    const string filename = "swss-signal-stats.rec";
    const string fullpath = dirname + "/" + filename;
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
