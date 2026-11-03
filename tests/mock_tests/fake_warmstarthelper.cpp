#include "warmRestartHelper.h"

#include <gtest/gtest.h>
#include <unordered_set>

static swss::DBConnector gDb("APPL_DB", 0);

// Mock-specific static variables for testing warm restart state
using MockRefreshMap = std::unordered_map<std::string, swss::KeyOpFieldsValuesTuple>;
static std::unordered_map<std::string, MockRefreshMap> g_mockRefreshMaps;
static swss::WarmStart::WarmStartState g_mockState = swss::WarmStart::RECONCILED;
static bool g_mockEnabled = true;
// Tables registered on each live helper; like the real helper, registrations last as long as it does
static std::unordered_map<const swss::WarmStartHelper *, std::unordered_set<std::string>> g_mockRegisteredTables;

namespace swss {

WarmStartHelper::WarmStartHelper(RedisPipeline *pipeline,
                                 ProducerStateTable *syncTable,
                                 const std::string &syncTableName,
                                 const std::string &dockerName,
                                 const std::string &appName) :
    m_syncTableName(syncTableName)
{
    g_mockRefreshMaps.emplace(syncTableName, MockRefreshMap{});
    g_mockRegisteredTables[this].insert(syncTableName);
}

WarmStartHelper::~WarmStartHelper()
{
    g_mockRegisteredTables.erase(this);
}

void WarmStartHelper::setState(WarmStart::WarmStartState state)
{
    g_mockState = state;
}

WarmStart::WarmStartState WarmStartHelper::getState() const
{
    return g_mockState;
}

bool WarmStartHelper::checkAndStart()
{
    return false;
}

bool WarmStartHelper::isReconciled() const
{
    return (g_mockState == WarmStart::RECONCILED);
}

bool WarmStartHelper::inProgress() const
{
    // Match real implementation: return true when enabled and not reconciled
    return (g_mockEnabled && g_mockState != WarmStart::RECONCILED);
}

uint32_t WarmStartHelper::getRestartTimer() const
{
    return 0;
}

void WarmStartHelper::registerTable(RedisPipeline *pipeline,
                                    ProducerStateTable *syncTable,
                                    const std::string &syncTableName)
{
    g_mockRefreshMaps.emplace(syncTableName, MockRefreshMap{});
    // The real helper throws on a table it already has
    if (!g_mockRegisteredTables[this].insert(syncTableName).second)
    {
        ADD_FAILURE() << "registerTable() on already registered table " << syncTableName;
    }
}

bool WarmStartHelper::runRestoration()
{
    return false;
}

void WarmStartHelper::insertRefreshMap(const KeyOpFieldsValuesTuple &kfv)
{
    insertRefreshMap(m_syncTableName, kfv);
}

void WarmStartHelper::insertRefreshMap(const std::string &syncTableName,
                                       const KeyOpFieldsValuesTuple &kfv)
{
    // The real helper asserts on a table that was never registered
    if (g_mockRegisteredTables[this].count(syncTableName) == 0)
    {
        ADD_FAILURE() << "insertRefreshMap() on unregistered table " << syncTableName;
        return;
    }

    const std::string key = kfvKey(kfv);
    g_mockRefreshMaps[syncTableName][key] = kfv;
}

void WarmStartHelper::reconcile()
{
}

const std::string WarmStartHelper::printKFV(const std::string &key,
                                            const std::vector<FieldValueTuple> &fv)
{
    return "";
}

bool WarmStartHelper::compareAllFV(const std::vector<FieldValueTuple> &left,
                                   const std::vector<FieldValueTuple> &right)
{
    return false;
}

bool WarmStartHelper::compareOneFV(const std::string &v1, const std::string &v2)
{
    return false;
}

}

// Test utility function to reset mock state between tests
void resetMockWarmStartHelper()
{
    g_mockRefreshMaps.clear();
    g_mockState = swss::WarmStart::RECONCILED;  // Default to not in progress
    g_mockEnabled = true;
}

bool getMockWarmStartHelperRefreshEntry(const std::string &tableName,
                                        const std::string &key,
                                        swss::KeyOpFieldsValuesTuple &kfv)
{
    auto table = g_mockRefreshMaps.find(tableName);
    if (table == g_mockRefreshMaps.end())
    {
        return false;
    }

    auto entry = table->second.find(key);
    if (entry == table->second.end())
    {
        return false;
    }

    kfv = entry->second;
    return true;
}

size_t getMockWarmStartHelperRefreshMapSize(const std::string &tableName)
{
    return g_mockRefreshMaps[tableName].size();
}
