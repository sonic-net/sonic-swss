#include "warmrestart/warmRestartHelper.h"

static swss::DBConnector gDb("APPL_DB", 0);

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
}

WarmStart::WarmStartState WarmStartHelper::getState() const
{
    return WarmStart::WarmStartState::INITIALIZED;
}

bool WarmStartHelper::checkAndStart()
{
    return false;
}

bool WarmStartHelper::isReconciled() const
{
    return false;
}

bool WarmStartHelper::inProgress() const
{
    return false;
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
