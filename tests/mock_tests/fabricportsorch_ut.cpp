#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#define private public
#define protected public
#include "orchagent/fabricportsorch.h"
#undef private
#undef protected

#include "dbconnector.h"
#include "sai_serialize.h"
#include "table.h"

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern std::string gMySwitchType;

namespace fabricportsorch_test
{
using namespace swss;

namespace
{
sai_status_t stubGetSwitchAttribute(sai_object_id_t, uint32_t attr_count,
                                    sai_attribute_t *attr_list)
{
    for (uint32_t i = 0; i < attr_count; ++i)
    {
        if (attr_list[i].id == SAI_SWITCH_ATTR_NUMBER_OF_FABRIC_PORTS)
        {
            attr_list[i].value.u32 = 0;
        }
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t stubSetSwitchAttribute(sai_object_id_t, const sai_attribute_t *)
{
    return SAI_STATUS_SUCCESS;
}

class FabricPortsOrchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_oldSwitchApi = sai_switch_api;
        m_oldSwitchType = gMySwitchType;
        if (m_oldSwitchApi != nullptr)
        {
            m_stubSwitchApi = *m_oldSwitchApi;
        }
        else
        {
            m_stubSwitchApi = {};
        }
        m_stubSwitchApi.get_switch_attribute = stubGetSwitchAttribute;
        m_stubSwitchApi.set_switch_attribute = stubSetSwitchAttribute;
        sai_switch_api = &m_stubSwitchApi;
        gMySwitchType = "tor";

        DBConnector appl_db("APPL_DB", 0);
        DBConnector state_db("STATE_DB", 0);
        DBConnector counters_db("COUNTERS_DB", 0);
        Table(&appl_db, APP_FABRIC_MONITOR_DATA_TABLE_NAME).del("FABRIC_MONITOR_DATA");
        Table(&state_db, APP_FABRIC_PORT_TABLE_NAME).del("PORT0");
        Table(&counters_db, COUNTERS_TABLE).del("oid:0x1000000000099");
    }

    void TearDown() override
    {
        DBConnector appl_db("APPL_DB", 0);
        DBConnector state_db("STATE_DB", 0);
        DBConnector counters_db("COUNTERS_DB", 0);
        Table(&appl_db, APP_FABRIC_MONITOR_DATA_TABLE_NAME).del("FABRIC_MONITOR_DATA");
        Table(&state_db, APP_FABRIC_PORT_TABLE_NAME).del("PORT0");
        Table(&counters_db, COUNTERS_TABLE).del("oid:0x1000000000099");

        sai_switch_api = m_oldSwitchApi;
        gMySwitchType = m_oldSwitchType;
    }

    sai_switch_api_t *m_oldSwitchApi = nullptr;
    sai_switch_api_t m_stubSwitchApi = {};
    std::string m_oldSwitchType;
};

// Helpers for the updateFabricRate() tests below: one fabric lane (0) backed
// by port OID 0x1000000000099, state in STATE_DB FABRIC_PORT_TABLE|PORT0.
const sai_object_id_t kRatePortOid = 0x1000000000099;

long long nowSeconds()
{
    return std::chrono::time_point_cast<std::chrono::seconds>(
               std::chrono::system_clock::now())
        .time_since_epoch()
        .count();
}

void setRateState(double oldRate, uint64_t oldData, long long lastTime)
{
    DBConnector state_db("STATE_DB", 0);
    Table(&state_db, APP_FABRIC_PORT_TABLE_NAME).set("PORT0", {
        {"OLD_RX_RATE_AVG", std::to_string(oldRate)},
        {"OLD_RX_DATA", std::to_string(oldData)},
        {"OLD_TX_RATE_AVG", std::to_string(oldRate)},
        {"OLD_TX_DATA", std::to_string(oldData)},
        {"LAST_TIME", std::to_string(lastTime)},
    });
}

void setRateCounters(uint64_t rxBytes, uint64_t txBytes)
{
    DBConnector counters_db("COUNTERS_DB", 0);
    Table(&counters_db, COUNTERS_TABLE).set(sai_serialize_object_id(kRatePortOid), {
        {"SAI_PORT_STAT_IF_IN_OCTETS", std::to_string(rxBytes)},
        {"SAI_PORT_STAT_IF_OUT_OCTETS", std::to_string(txBytes)},
    });
}

double getRate(const std::string &field)
{
    DBConnector state_db("STATE_DB", 0);
    std::string value;
    EXPECT_TRUE(Table(&state_db, APP_FABRIC_PORT_TABLE_NAME).hget("PORT0", field, value));
    return std::stod(value);
}

long long getLastTime()
{
    DBConnector state_db("STATE_DB", 0);
    std::string value;
    EXPECT_TRUE(Table(&state_db, APP_FABRIC_PORT_TABLE_NAME).hget("PORT0", "LAST_TIME", value));
    return std::stoll(value);
}

std::unique_ptr<FabricPortsOrch> makeRateOrch(DBConnector &appl_db)
{
    std::vector<table_name_with_pri_t> tables = {
        {APP_FABRIC_MONITOR_PORT_TABLE_NAME, 30},
        {APP_FABRIC_MONITOR_DATA_TABLE_NAME, 30},
    };
    auto orch = std::make_unique<FabricPortsOrch>(&appl_db, tables, false, false);
    orch->m_fabricLanePortMap[0] = kRatePortOid;
    orch->m_getFabricPortListDone = true;
    return orch;
}

// updateFabricRate() keeps an exponential moving average
//   avg = old * w + (deltaMegabits / deltaTime) * (1 - w),
//   w = exp(-deltaTime / FABRIC_DEBUG_POLLING_INTERVAL_DEFAULT).
// The tests below solve that formula for the interval L from the stored
// result and check that L is a whole number of seconds in [12, 60], so they
// are exact without hard-coding the interval. Rates are stored with 6
// decimals, which keeps the solved L within 1e-4 of an integer.
void expectWholeSecondInterval(double interval)
{
    const double rounded = std::round(interval);
    EXPECT_LT(std::fabs(interval - rounded), 1e-3) << "interval " << interval;
    EXPECT_GE(rounded, 12.0) << "interval " << interval;
    EXPECT_LE(rounded, 60.0) << "interval " << interval;
}

// A poll that arrives before a full interval has elapsed must still move the
// average towards the new sample. With integer division the weight was
// exp(0) == 1 for any deltaTime shorter than the interval, so the sample was
// dropped and the average stayed at its old value (0).
TEST_F(FabricPortsOrchTest, UpdateFabricRateSubIntervalPollAdvancesAverage)
{
    DBConnector appl_db("APPL_DB", 0);
    auto orch = makeRateOrch(appl_db);

    // 125,000,000 bytes == 1,000 Mb on RX (2,000 Mb on TX), polled 1s (2s if
    // the wall clock ticks in between) after the last sample.
    const long long oldTime = nowSeconds() - 1;
    setRateState(0.0, 0, oldTime);
    setRateCounters(125000000, 250000000);

    orch->updateFabricRate();

    // deltaTime as seen by updateFabricRate(): the LAST_TIME it stored.
    const double deltaTime = static_cast<double>(getLastTime() - oldTime);
    ASSERT_GE(deltaTime, 1.0);
    ASSERT_LE(deltaTime, 30.0);
    const double rx = getRate("OLD_RX_RATE_AVG");
    // rx = 1000 / dt * (1 - exp(-dt / L))
    expectWholeSecondInterval(-deltaTime / std::log(1.0 - rx * deltaTime / 1000.0));
    EXPECT_NEAR(getRate("OLD_TX_RATE_AVG"), 2.0 * rx, 0.001);
}

// Two polls within the same wall-clock second must not divide by zero. Before
// the guard, deltaTime == 0 produced inf * 0 == NaN, which was written to
// STATE_DB and then read back as the old average on every following poll, so
// the rate never recovered.
TEST_F(FabricPortsOrchTest, UpdateFabricRateSameSecondPollStaysFinite)
{
    DBConnector appl_db("APPL_DB", 0);
    auto orch = makeRateOrch(appl_db);

    // Retry until updateFabricRate() stored the same LAST_TIME we set, i.e.
    // it saw deltaTime == 0.
    bool sameSecond = false;
    for (int attempt = 0; attempt < 5 && !sameSecond; ++attempt)
    {
        const long long t0 = nowSeconds();
        setRateState(100.0, 0, t0);
        setRateCounters(125000000, 125000000);
        orch->updateFabricRate();
        sameSecond = (getLastTime() == t0);
    }
    ASSERT_TRUE(sameSecond);

    // Treated as a 1s interval: rx = 100 * w + 1000 * (1 - w), w = exp(-1 / L).
    const double rx = getRate("OLD_RX_RATE_AVG");
    ASSERT_TRUE(std::isfinite(rx));
    expectWholeSecondInterval(-1.0 / std::log((1000.0 - rx) / 900.0));
}

// A wall clock that stepped backwards (LAST_TIME in the future) must be
// treated like a 1s interval rather than producing a weight exp(+n) > 1.
TEST_F(FabricPortsOrchTest, UpdateFabricRateClockStepBackIsClampedToOneSecond)
{
    DBConnector appl_db("APPL_DB", 0);
    auto orch = makeRateOrch(appl_db);

    // No traffic since the last sample: the average decays by one 1s step,
    // rx = 100 * exp(-1 / L), instead of staying frozen (weight exp(0)) or
    // growing (weight exp(+n)).
    setRateState(100.0, 1000000000, nowSeconds() + 100);
    setRateCounters(1000000000, 1000000000);

    orch->updateFabricRate();

    const double rx = getRate("OLD_RX_RATE_AVG");
    ASSERT_GT(rx, 0.0);
    expectWholeSecondInterval(-1.0 / std::log(rx / 100.0));
    EXPECT_NEAR(getRate("OLD_TX_RATE_AVG"), rx, 0.001);
}

}  // namespace
}  // namespace fabricportsorch_test
