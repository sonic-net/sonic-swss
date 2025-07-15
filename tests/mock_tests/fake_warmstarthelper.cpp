#include "warmRestartHelper.h"

static swss::DBConnector gDb("APPL_DB", 0);

// Mock-specific static variables for testing warm restart state
static std::unordered_map<std::string, swss::KeyOpFieldsValuesTuple> g_mockRefreshMap;
static swss::WarmStart::WarmStartState g_mockState = swss::WarmStart::RECONCILED;
static bool g_mockEnabled = true;

namespace swss {

WarmStartHelper::WarmStartHelper(RedisPipeline *pipeline,
                                 ProducerStateTable *syncTable,
                                 const std::string &syncTableName,
                                 const std::string &dockerName,
                                 const std::string &appName) :
    m_restorationTable(&gDb, "")
{
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

bool WarmStartHelper::runRestoration()
{
    return false;
}

void WarmStartHelper::insertRefreshMap(const KeyOpFieldsValuesTuple &kfv)
{
    // Store the entry - in real implementation this would be used during reconciliation
    const std::string key = kfvKey(kfv);
    g_mockRefreshMap[key] = kfv;
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
    g_mockRefreshMap.clear();
    g_mockState = swss::WarmStart::RECONCILED;  // Default to not in progress
    g_mockEnabled = true;
}
