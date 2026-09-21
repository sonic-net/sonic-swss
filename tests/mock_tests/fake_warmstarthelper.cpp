#include "warmRestartHelper.h"

static swss::DBConnector gDb("APPL_DB", 0);

// Mock-specific static variables for testing warm restart state
using MockRefreshMap = std::unordered_map<std::string, swss::KeyOpFieldsValuesTuple>;
static std::unordered_map<std::string, MockRefreshMap> g_mockRefreshMaps;
static swss::WarmStart::WarmStartState g_mockState = swss::WarmStart::RECONCILED;
static bool g_mockEnabled = true;

namespace swss {

WarmStartHelper::WarmStartHelper(RedisPipeline *pipeline,
                                 ProducerStateTable *syncTable,
                                 const std::string &syncTableName,
                                 const std::string &dockerName,
                                 const std::string &appName) :
    m_syncTableName(syncTableName)
{
    g_mockRefreshMaps.emplace(syncTableName, MockRefreshMap{});
}

WarmStartHelper::~WarmStartHelper()
{
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
