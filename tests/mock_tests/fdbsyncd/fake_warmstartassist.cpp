#include <map>
#include <tuple>
#include "warmRestartAssist.h"

using namespace std;
using namespace swss;

AppRestartAssist::AppRestartAssist(RedisPipeline *pipelineAppDB, const std::string &appName,
                                   const std::string &dockerName, const uint32_t defaultWarmStartTimerValue):
    m_warmStartTimer(timespec{0, 0}),
    m_warmStartInProgress(false)
{
}

AppRestartAssist::~AppRestartAssist()
{
}

std::map<std::string, bool> fake_registered_tables;

void AppRestartAssist::registerAppTable(const std::string &tableName, ProducerStateTable *psTable, bool replaceFields)
{
    fake_registered_tables[tableName] = replaceFields;
}

// join the field-value strings for straight printing.
string AppRestartAssist::joinVectorString(const vector<FieldValueTuple> &fv)
{
    return "";
}

void AppRestartAssist::setCacheEntryState(std::vector<FieldValueTuple> &fvVector,
    cache_state_t state)
{
}

AppRestartAssist::cache_state_t AppRestartAssist::getCacheEntryState(const std::vector<FieldValueTuple> &fvVector)
{
    return AppRestartAssist::cache_state_t::STALE;
}

void AppRestartAssist::appDataReplayed()
{
}

void AppRestartAssist::readTablesToMap()
{
}

void AppRestartAssist::insertToMap(string tableName, string key, vector<FieldValueTuple> fvVector, bool delete_key)
{
}


void AppRestartAssist::reconcile()
{
}

int fake_keep_stale_entries_calls;
std::vector<std::tuple<std::string, std::string, std::vector<FieldValueTuple>>> fake_kept_entries;

void AppRestartAssist::keepStaleEntries(const KeptEntryHandler &onKept)
{
    fake_keep_stale_entries_calls++;
    for (auto const &entry : fake_kept_entries)
    {
        if (onKept)
        {
            onKept(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
        }
    }
}

void AppRestartAssist::setReconcileInterval(uint32_t time)
{
}

void AppRestartAssist::startReconcileTimer(Select &s)
{
}

void AppRestartAssist::stopReconcileTimer(Select &s)
{
}

bool AppRestartAssist::checkReconcileTimer(Selectable *s)
{
    return false;
}

bool AppRestartAssist::contains(const std::vector<FieldValueTuple>& left,
              const std::vector<FieldValueTuple>& right)
{
    return false;
}
