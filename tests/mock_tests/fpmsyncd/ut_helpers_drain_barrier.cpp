#include "ut_helpers_drain_barrier.h"

#include "schema.h"
#include "table.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace swss;

extern "C" {
// Forward declaration of the test-DB reset hook from mock_dbconnector.cpp
// (used by all the other fpmsyncd tests' SetUp/TearDown).
void testing_db_reset();
}

// Local fallback shim — some builds expose this via the swss::testing_db
// namespace and others via a free function. We just call the namespaced
// one here; the existing tests link to it fine.
namespace testing_db { void reset(); }

namespace ut_drain {

std::vector<FieldValueTuple> RestartCheckPayload::toValues() const
{
    std::vector<FieldValueTuple> v;
    if (hasAutoResumeTimeoutSec)
    {
        v.emplace_back("autoResumeTimeoutSec", std::to_string(autoResumeTimeoutSec));
    }
    if (hasResume)
    {
        v.emplace_back("resume", resume ? "true" : "false");
    }
    return v;
}

}  // namespace ut_drain


void WarmRebootDrainFixture::SetUp()
{
    // Construction of m_db / m_pipeline / m_routeSync / inspection Tables
    // happens at fixture-ctor time via in-class initializers (see header).
    // SetUp just resets the per-test mock-DB state so prior tests don't
    // leak keys into this one.
    //
    // Note: we explicitly do NOT clear the drain flag here. Each test in
    // this fixture gets a freshly-constructed RouteSync (gtest constructs
    // a new Test instance per TEST_F), so m_drainingForWarmRestart starts
    // false out of the gate.
    testing_db::reset();
}

void WarmRebootDrainFixture::TearDown()
{
    testing_db::reset();
}

bool WarmRebootDrainFixture::routePresent(const std::string& prefix)
{
    std::vector<FieldValueTuple> fvs;
    return m_routeTableInspect->get(prefix, fvs);
}

size_t WarmRebootDrainFixture::routeKeyCount()
{
    std::vector<std::string> keys;
    m_routeTableInspect->getKeys(keys);
    return keys.size();
}

bool WarmRebootDrainFixture::labelRoutePresent(const std::string& label)
{
    std::vector<FieldValueTuple> fvs;
    return m_labelRouteTableInspect->get(label, fvs);
}

void WarmRebootDrainFixture::injectRouteSet(const std::string& prefix,
                                            const std::string& nexthop,
                                            const std::string& ifname)
{
    // Build a RouteTableFieldValueTupleWrapper with the minimum fields a
    // typical IPv4 unicast route has after fpmsyncd's onMsgRaw path
    // populates it. We don't care about the field VALUES — only that
    // setRouteWithWarmRestart routes them through the gate (or doesn't).
    RouteTableFieldValueTupleWrapper fvw(prefix, std::string("static"),
                                         m_routeSync.isNbZmqEnabled());
    fvw.nexthop = nexthop;
    fvw.ifname  = ifname;

    m_routeSync.setRouteWithWarmRestart(fvw, *m_routeSync.m_routeTable);
}

void WarmRebootDrainFixture::injectRouteDel(const std::string& prefix)
{
    RouteTableFieldValueTupleWrapper fvw(prefix, std::string(""),
                                         m_routeSync.isNbZmqEnabled());
    m_routeSync.delWithWarmRestart(std::move(fvw), *m_routeSync.m_routeTable);
}

void WarmRebootDrainFixture::injectVnetSet(const std::string& key)
{
    // VNET path goes through RouteSync::setTable, which is NOT gated.
    VnetRouteTableFieldValueTupleWrapper fvw(key, m_routeSync.isNbZmqEnabled());
    fvw.nexthop = "10.200.0.1";
    fvw.ifname  = "Vlan100";
    m_routeSync.setTable(fvw, m_routeSync.m_vnet_routeTable);
}

void WarmRebootDrainFixture::writeDrainStateRow(const std::string& drain_state,
                                                const std::string& drain_queue_size)
{
    m_warmRestartTable->hset("fpmsyncd", "drain_state",      drain_state);
    m_warmRestartTable->hset("fpmsyncd", "drain_queue_size", drain_queue_size);
}

void WarmRebootDrainFixture::writeLegacyQueueSize(const std::string& v)
{
    m_warmRestartTable->hset("fpmsyncd", "queueSize", v);
}

bool WarmRebootDrainFixture::drainStateRowEmpty()
{
    return getDrainStateField().empty() &&
           getDrainQueueSizeField().empty() &&
           getLegacyQueueSizeField().empty();
}

std::string WarmRebootDrainFixture::getDrainStateField()
{
    std::string v;
    m_warmRestartTable->hget("fpmsyncd", "drain_state", v);
    return v;
}

std::string WarmRebootDrainFixture::getDrainQueueSizeField()
{
    std::string v;
    m_warmRestartTable->hget("fpmsyncd", "drain_queue_size", v);
    return v;
}

std::string WarmRebootDrainFixture::getLegacyQueueSizeField()
{
    std::string v;
    m_warmRestartTable->hget("fpmsyncd", "queueSize", v);
    return v;
}

void WarmRebootDrainFixture::clearDrainStateFields()
{
    m_warmRestartTable->hdel("fpmsyncd", "drain_state");
    m_warmRestartTable->hdel("fpmsyncd", "drain_queue_size");
    m_warmRestartTable->hdel("fpmsyncd", "queueSize");
}


void WarmRebootDrainFixtureZmqOn::SetUp()
{
    WarmRebootDrainFixture::SetUp();
    // The drain barrier's implicit gate runs on m_hasZmqProducerTables.
    // Setting it directly is sufficient for testing the gating semantics
    // — we don't need the producer tables to actually be
    // ZmqProducerStateTable to exercise the runtime branches.
    m_routeSync.m_hasZmqProducerTables = true;
}
