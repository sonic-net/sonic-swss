/*
 * Unit tests: startup helpers in fpmsyncd_warmreboot.
 *
 *   parseDrainAutoResumeSec               — pure string parser (no DB)
 *   loadDrainAutoResumeSecFromConfigDb    — thin Table::hget wrapper
 *   clearStaleDrainStateFields            — STATE_DB|WARM_RESTART_TABLE|fpmsyncd HDEL
 *
 * Branch coverage we aim for:
 *
 *   parseDrainAutoResumeSec
 *     - empty string                            → Default, value=defaultSec
 *     - "None" (sonic-cfggen rendering of None) → Default
 *     - valid in-range int "30"                 → ConfigDb, value=30
 *     - in-range edges (minSec, maxSec)         → ConfigDb
 *     - just-out-of-range (minSec-1, maxSec+1)  → OutOfRange, value=defaultSec
 *     - far-out-of-range (negative, huge)       → OutOfRange
 *     - garbage "abc"                           → Invalid
 *     - mixed "5abc"                            → Invalid (strict parse)
 *     - leading whitespace " 42"                → ConfigDb (stoi accepts)
 *     - parse-overflow ("9999999999999999")     → Invalid (catches std::out_of_range)
 *
 *   loadDrainAutoResumeSecFromConfigDb
 *     - field absent                            → returns defaultSec
 *     - field set to valid value                → returns parsed value
 *     - field set to invalid value              → returns defaultSec
 *
 *   clearStaleDrainStateFields
 *     - all three fields present                → all removed
 *     - row empty (no fields)                   → no-op (no exception)
 *     - partial presence (only drain_state)     → drain_state removed, others stay absent
 *     - non-fpmsyncd row not disturbed
 */

#include "ut_helpers_drain_barrier.h"

#include "fpmsyncd/warmreboot_handler.h"
#include "schema.h"
#include "table.h"

#include <gtest/gtest.h>

#include <string>

using namespace fpmsyncd_warmreboot;
using namespace swss;

namespace testing_db { void reset(); }

namespace {

constexpr int kDefaultSec = 30;
constexpr int kMinSec     = 1;
constexpr int kMaxSec     = 600;

}  // namespace

// ===========================================================
// parseDrainAutoResumeSec — pure parser (no DB)
// ===========================================================

TEST(ParseDrainAutoResumeSec, Empty_ReturnsDefault)
{
    auto p = parseDrainAutoResumeSec("", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::Default, p.source);
}

TEST(ParseDrainAutoResumeSec, NoneLiteral_ReturnsDefault)
{
    // sonic-cfggen renders Python None as the literal string "None" when
    // a field is absent in CONFIG_DB. Treat it identically to empty.
    auto p = parseDrainAutoResumeSec("None", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::Default, p.source);
}

TEST(ParseDrainAutoResumeSec, ValidInRange_UsesConfigValue)
{
    auto p = parseDrainAutoResumeSec("45", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(45, p.value);
    EXPECT_EQ(45, p.rawParsed);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::ConfigDb, p.source);
}

TEST(ParseDrainAutoResumeSec, LowerEdge_Honored)
{
    auto p = parseDrainAutoResumeSec("1", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(1, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::ConfigDb, p.source);
}

TEST(ParseDrainAutoResumeSec, UpperEdge_Honored)
{
    auto p = parseDrainAutoResumeSec("600", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(600, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::ConfigDb, p.source);
}

TEST(ParseDrainAutoResumeSec, JustBelowMin_OutOfRange)
{
    auto p = parseDrainAutoResumeSec("0", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value)
        << "out-of-range parse must fall back to defaultSec";
    EXPECT_EQ(0, p.rawParsed);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::OutOfRange, p.source);
}

TEST(ParseDrainAutoResumeSec, JustAboveMax_OutOfRange)
{
    auto p = parseDrainAutoResumeSec("601", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(601, p.rawParsed);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::OutOfRange, p.source);
}

TEST(ParseDrainAutoResumeSec, Negative_OutOfRange)
{
    auto p = parseDrainAutoResumeSec("-5", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(-5, p.rawParsed);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::OutOfRange, p.source);
}

TEST(ParseDrainAutoResumeSec, HugePositive_OutOfRange)
{
    auto p = parseDrainAutoResumeSec("100000", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::OutOfRange, p.source);
}

TEST(ParseDrainAutoResumeSec, NonNumeric_Invalid)
{
    auto p = parseDrainAutoResumeSec("abc", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::Invalid, p.source);
}

TEST(ParseDrainAutoResumeSec, TrailingGarbage_Invalid)
{
    // We explicitly reject strings that std::stoi would parse loosely
    // (e.g. "5abc" → 5). This protects against typos slipping past the
    // CONFIG_DB validator silently.
    auto p = parseDrainAutoResumeSec("5abc", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::Invalid, p.source);
}

TEST(ParseDrainAutoResumeSec, LeadingWhitespace_Accepted)
{
    // std::stoi skips leading whitespace AND counts those characters
    // in `pos`, so our strict "consumed == size" check still passes
    // and " 42" is treated as a valid 42. This is a minor leniency we
    // accept: in production the field comes from sonic-cfggen which
    // never produces leading whitespace. Trailing garbage is still
    // rejected (see TrailingGarbage_Invalid).
    auto p = parseDrainAutoResumeSec(" 42", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(42, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::ConfigDb, p.source);
}

TEST(ParseDrainAutoResumeSec, TrailingWhitespace_Invalid)
{
    // "42 " has consumed=2 (just the digits), size=3 → consumed != size
    // → strict check rejects. This catches the asymmetric case where
    // std::stoi would parse leadingly-whitespace input but our strict
    // check still flags trailing junk.
    auto p = parseDrainAutoResumeSec("42 ", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::Invalid, p.source);
}

TEST(ParseDrainAutoResumeSec, ParseOverflow_Invalid)
{
    // std::stoi throws std::out_of_range when the integer doesn't fit
    // in int. The parser must catch that and report Invalid, NOT crash.
    auto p = parseDrainAutoResumeSec("9999999999999999", kDefaultSec, kMinSec, kMaxSec);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::Invalid, p.source);
}

TEST(ParseDrainAutoResumeSec, CustomRangeRespected)
{
    // Some hypothetical caller wants a tighter [10, 100] window.
    auto p = parseDrainAutoResumeSec("5", kDefaultSec, /*minSec=*/10, /*maxSec=*/100);
    EXPECT_EQ(kDefaultSec, p.value);
    EXPECT_EQ(DrainAutoResumeSecParse::Source::OutOfRange, p.source);
}

// ===========================================================
// loadDrainAutoResumeSecFromConfigDb — Table::hget wrapper
// ===========================================================

class LoadDrainAutoResumeSecFromConfigDbTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        testing_db::reset();
        m_configDb = std::make_shared<DBConnector>("CONFIG_DB", 0, true);
        m_deviceMetadata = std::make_unique<Table>(m_configDb.get(), "DEVICE_METADATA");
    }

    void TearDown() override
    {
        m_deviceMetadata.reset();
        m_configDb.reset();
        testing_db::reset();
    }

    std::shared_ptr<DBConnector> m_configDb;
    std::unique_ptr<Table>       m_deviceMetadata;
};

TEST_F(LoadDrainAutoResumeSecFromConfigDbTest, FieldAbsent_ReturnsDefault)
{
    int v = loadDrainAutoResumeSecFromConfigDb(*m_deviceMetadata, kDefaultSec);
    EXPECT_EQ(kDefaultSec, v);
}

TEST_F(LoadDrainAutoResumeSecFromConfigDbTest, FieldValid_ReturnsParsedValue)
{
    m_deviceMetadata->hset("localhost", "fpmsyncd_drain_auto_resume_sec", "120");
    int v = loadDrainAutoResumeSecFromConfigDb(*m_deviceMetadata, kDefaultSec);
    EXPECT_EQ(120, v);
}

TEST_F(LoadDrainAutoResumeSecFromConfigDbTest, FieldInvalid_FallsBackToDefault)
{
    m_deviceMetadata->hset("localhost", "fpmsyncd_drain_auto_resume_sec", "wat");
    int v = loadDrainAutoResumeSecFromConfigDb(*m_deviceMetadata, kDefaultSec);
    EXPECT_EQ(kDefaultSec, v);
}

TEST_F(LoadDrainAutoResumeSecFromConfigDbTest, FieldOutOfRange_FallsBackToDefault)
{
    m_deviceMetadata->hset("localhost", "fpmsyncd_drain_auto_resume_sec", "9999");
    int v = loadDrainAutoResumeSecFromConfigDb(*m_deviceMetadata, kDefaultSec);
    EXPECT_EQ(kDefaultSec, v);
}

TEST_F(LoadDrainAutoResumeSecFromConfigDbTest, FieldLiteralNone_FallsBackToDefault)
{
    // sonic-cfggen behavior: explicit "None" in CONFIG_DB after a delete.
    m_deviceMetadata->hset("localhost", "fpmsyncd_drain_auto_resume_sec", "None");
    int v = loadDrainAutoResumeSecFromConfigDb(*m_deviceMetadata, kDefaultSec);
    EXPECT_EQ(kDefaultSec, v);
}

// ===========================================================
// clearStaleDrainStateFields — STATE_DB HDEL helper
// ===========================================================
//
// Reuses the WarmRebootDrainFixture's m_warmRestartTable inspector
// for consistent setup with the rest of the drain-barrier tests.

TEST_F(WarmRebootDrainFixture, ClearStaleDrainStateFields_RemovesAllThree)
{
    writeDrainStateRow("draining", "17");
    writeLegacyQueueSize("99");
    EXPECT_FALSE(drainStateRowEmpty());

    fpmsyncd_warmreboot::clearStaleDrainStateFields(*m_warmRestartTable);

    EXPECT_TRUE(drainStateRowEmpty())
        << "drain_state / drain_queue_size / queueSize must all be gone "
           "after clearStaleDrainStateFields";
}

TEST_F(WarmRebootDrainFixture, ClearStaleDrainStateFields_IdempotentOnEmptyRow)
{
    EXPECT_TRUE(drainStateRowEmpty());

    // No throw on empty row.
    EXPECT_NO_THROW(fpmsyncd_warmreboot::clearStaleDrainStateFields(*m_warmRestartTable));
    EXPECT_NO_THROW(fpmsyncd_warmreboot::clearStaleDrainStateFields(*m_warmRestartTable));
    EXPECT_TRUE(drainStateRowEmpty());
}

TEST_F(WarmRebootDrainFixture, ClearStaleDrainStateFields_PartialPresence)
{
    // Only drain_state present (drain_queue_size and queueSize absent).
    m_warmRestartTable->hset("fpmsyncd", "drain_state", "draining");
    EXPECT_EQ("draining", getDrainStateField());
    EXPECT_TRUE(getDrainQueueSizeField().empty());

    fpmsyncd_warmreboot::clearStaleDrainStateFields(*m_warmRestartTable);

    EXPECT_TRUE(getDrainStateField().empty());
    EXPECT_TRUE(getDrainQueueSizeField().empty());
}

TEST_F(WarmRebootDrainFixture, ClearStaleDrainStateFields_DoesNotTouchStateField)
{
    // fpmsyncd's WarmStartHelper owns the "state" field on the fpmsyncd row
    // (initialized / restored / reconciled). The startup HDEL must NOT
    // disturb that — see comment in fpmsyncd/warmreboot_handler.cpp.
    m_warmRestartTable->hset("fpmsyncd", "state",            "reconciled");
    m_warmRestartTable->hset("fpmsyncd", "drain_state",      "draining");
    m_warmRestartTable->hset("fpmsyncd", "drain_queue_size", "5");

    fpmsyncd_warmreboot::clearStaleDrainStateFields(*m_warmRestartTable);

    std::string stateVal;
    m_warmRestartTable->hget("fpmsyncd", "state", stateVal);
    EXPECT_EQ("reconciled", stateVal)
        << "clearStaleDrainStateFields must NOT touch the WarmStartHelper-owned `state` field";
    EXPECT_TRUE(getDrainStateField().empty());
    EXPECT_TRUE(getDrainQueueSizeField().empty());
}

TEST_F(WarmRebootDrainFixture, ClearStaleDrainStateFields_DoesNotTouchOtherRows)
{
    // Only fpmsyncd row should be cleared. Another app's row stays intact.
    m_warmRestartTable->hset("fpmsyncd", "drain_state",       "draining");
    m_warmRestartTable->hset("bgp",      "state",             "reconciled");
    m_warmRestartTable->hset("swss",     "drain_state",       "draining"); // bogus, but tests isolation

    fpmsyncd_warmreboot::clearStaleDrainStateFields(*m_warmRestartTable);

    std::string bgpState;
    m_warmRestartTable->hget("bgp", "state", bgpState);
    EXPECT_EQ("reconciled", bgpState);

    std::string swssDrain;
    m_warmRestartTable->hget("swss", "drain_state", swssDrain);
    EXPECT_EQ("draining", swssDrain)
        << "the helper only touches the fpmsyncd row, not other apps' rows";

    EXPECT_TRUE(getDrainStateField().empty());
}
