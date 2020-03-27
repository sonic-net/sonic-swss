#include <string>
#include <algorithm>
#include "logger.h"
#include "schema.h"
#include "warm_restart.h"
#include "warmRestartAssist.h"

using namespace std;
using namespace swss;

// Translation map from enum to string for cache entry state.
const AppRestartAssist::cache_state_map AppRestartAssist::cacheStateMap =
{
    {STALE,     "STALE"},
    {SAME,      "SAME"},
    {NEW,       "NEW"},
    {DELETE,    "DELETE"}
};

AppRestartAssist::AppRestartAssist(RedisPipeline *pipelineAppDB, const std::string &appName,
                                   const std::string &dockerName, const uint32_t defaultWarmStartTimerValue):
    m_pipeLine(pipelineAppDB),
    m_appName(appName),
    m_dockerName(dockerName),
    m_warmStartTimer(timespec{0, 0})
{
    WarmStart::initialize(m_appName, m_dockerName);
    WarmStart::checkWarmStart(m_appName, m_dockerName);

    /*
     * set the default timer value.
     * If the application instance privides timer value, use it if valid.
     * Use the class default one if none is provided by application.
     */
    if (defaultWarmStartTimerValue > MAXIMUM_WARMRESTART_TIMER_VALUE)
    {
        throw std::invalid_argument("invalid timer value was provided");
    }
    else if (defaultWarmStartTimerValue != 0)
    {
        m_reconcileTimer = defaultWarmStartTimerValue;
    }
    else
    {
        m_reconcileTimer = DEFAULT_INTERNAL_TIMER_VALUE;
    }

    // If warm start enabled, it is marked as in progress initially.
    if (!WarmStart::isWarmStart())
    {
        m_warmStartInProgress = false;
    }
    else
    {
        // Use the configured timer if available and valid
        m_warmStartInProgress = true;
        uint32_t temp_value = WarmStart::getWarmStartTimer(m_appName, m_dockerName);
        if (temp_value != 0)
        {
            m_reconcileTimer = temp_value;
        }

        m_warmStartTimer.setInterval(timespec{m_reconcileTimer, 0});

        WarmStart::setWarmStartState(m_appName, WarmStart::INITIALIZED);
    }
}

AppRestartAssist::~AppRestartAssist()
{
    for (auto it = m_appTables.begin(); it != m_appTables.end(); it++)
    {
        delete (it->second);
    }
}

void AppRestartAssist::registerAppTable(const std::string &tableName, ProducerStateTable *psTable)
{
    m_psTables[tableName]  = psTable;

    // Clear the producerstate table to make sure no pending data for the AppTable
    psTable->clear();
    m_appTables[tableName] = new Table(m_pipeLine, tableName, false);
}

// join the field-value strings for straight printing.
string AppRestartAssist::joinVectorString(const vector<FieldValueTuple> &fv)
{
    string s;
    for (const auto &temps : fv )
    {
	   s += temps.first + ":" + temps.second + ", ";
    }
    return s;
}

// Set the cache entry state
void AppRestartAssist::setCacheEntryState(std::vector<FieldValueTuple> &fvVector,
    cache_state_t state)
{
    fvVector.back().second = cacheStateMap.at(state);
}

// Get the cache entry state
AppRestartAssist::cache_state_t AppRestartAssist::getCacheEntryState(const std::vector<FieldValueTuple> &fvVector)
{
    for (auto &iter : cacheStateMap)
    {
        if (fvVector.back().second == iter.second)
        {
            return iter.first;
        }
    }
    throw std::logic_error("cache entry state is invalid");
}

// Read table(s) from APPDB and append stale flag then insert to cachemap
void AppRestartAssist::readTablesToMap()
{
    vector<string> keys;

    for (auto it = m_appTables.begin(); it != m_appTables.end(); it++)
    {
        (it->second)->getKeys(keys);
        FieldValueTuple state(CACHE_STATE_FIELD, "");

        for (const auto &key: keys)
        {
            vector<FieldValueTuple> fv;

                // if the fieldvalue is empty, skip
            if (!(it->second)->get(key, fv))
            {
                continue;
            }

            fv.push_back(state);
            setCacheEntryState(fv, STALE);

            string s = joinVectorString(fv);

            SWSS_LOG_INFO("write to cachemap: %s, key: %s, "
                   "%s", (it->first).c_str(), key.c_str(), s.c_str());

            // insert to the cache map
            appTableCacheMap[it->first][key] = fv;
        }
        WarmStart::setWarmStartState(m_appName, WarmStart::RESTORED);
        SWSS_LOG_NOTICE("Restored appDB table to %s internal cache map", (it->first).c_str());
    }
    return;
}
 
/*
 * Check and insert to CacheMap Logic:
 * if delete_key:
 *  mark the entry as "DELETE";
 * else:
 *  if key exist {
 *    if it has different value: update with "NEW" flag.
 *    if same value:  mark it as "SAME";
 *  } else {
 *    insert with "NEW" flag.
 *   }
 */
void AppRestartAssist::insertToMap(string tableName, string key, vector<FieldValueTuple> fvVector, bool delete_key)
{
    SWSS_LOG_INFO("Received message %s, key: %s, "
            "%s, delete = %d", tableName.c_str(), key.c_str(), joinVectorString(fvVector).c_str(), delete_key);

    auto found = appTableCacheMap[tableName].find(key);

    if (delete_key)
    {
        SWSS_LOG_NOTICE("%s, delete key: %s, ", tableName.c_str(), key.c_str());
        /* mark it as DELETE if exist, otherwise, no-op */
        if (found != appTableCacheMap[tableName].end())
        {
            setCacheEntryState(found->second, DELETE);
        }
    }
    else if (found != appTableCacheMap[tableName].end())
    {
        // check only the original vector range (exclude cache-state field/value)
        if(! contains(found->second, fvVector))
        {
            SWSS_LOG_NOTICE("%s, found key: %s, new value ", tableName.c_str(), key.c_str());

            FieldValueTuple state(CACHE_STATE_FIELD, "");
            fvVector.push_back(state);

            // mark as NEW flag
            setCacheEntryState(fvVector, NEW);
            appTableCacheMap[tableName][key] = fvVector;
        }
        else
        {
            SWSS_LOG_INFO("%s, found key: %s, same value", tableName.c_str(), key.c_str());

            // mark as SAME flag
            setCacheEntryState(found->second, SAME);
        }
    }
    else
    {
        // not found, mark the entry as NEW and insert to map
        SWSS_LOG_NOTICE("%s, not found key: %s, new", tableName.c_str(), key.c_str());
        FieldValueTuple state(CACHE_STATE_FIELD, "");
        fvVector.push_back(state);
        setCacheEntryState(fvVector, NEW);
        appTableCacheMap[tableName][key] = fvVector;
    }
    return;
}

/*
 * Reconcile logic:
 *  iterate throught the cache map
 *  if the entry has "SAME" flag, do nothing
 *  if has "STALE/DELETE" flag, delete it from appDB.
 *  else if "NEW" flag,  add it to appDB
 *  else, throw (should never happen)
 */
void AppRestartAssist::reconcile()
{
    std::string tableName;

    SWSS_LOG_ENTER();
    for (auto tableIter = appTableCacheMap.begin(); tableIter != appTableCacheMap.end(); ++tableIter)
    {
        tableName = tableIter->first;
        for (auto it = (tableIter->second).begin(); it != (tableIter->second).end(); ++it)
        {
            string s = joinVectorString(it->second);
            auto state = getCacheEntryState(it->second);

            if (state == SAME)
            {
                SWSS_LOG_INFO("%s SAME, key: %s, %s",
                        tableName.c_str(), it->first.c_str(), s.c_str());
                continue;
            }
            else if (state == STALE || state == DELETE)
            {
                SWSS_LOG_NOTICE("%s STALE/DELETE, key: %s, %s",
                        tableName.c_str(), it->first.c_str(), s.c_str());

                //delete from appDB
                m_psTables[tableName]->del(it->first);
            }
            else if (state == NEW)
            {
                SWSS_LOG_NOTICE("%s NEW, key: %s, %s",
                        tableName.c_str(), it->first.c_str(), s.c_str());

                //add to appDB, exclude the state
                it->second.pop_back();
                m_psTables[tableName]->set(it->first, it->second);
            }
            else
            {
                throw std::logic_error("cache entry state is invalid");
            }
        }
        // reconcile finished, clear the map, mark the warmstart state
        appTableCacheMap[tableName].clear();
    }
    appTableCacheMap.clear();
    WarmStart::setWarmStartState(m_appName, WarmStart::RECONCILED);
    m_warmStartInProgress = false;
    return;
}

// start the timer, take Select class "s" to add the timer.
void AppRestartAssist::startReconcileTimer(Select &s)
{
    m_warmStartTimer.start();
    s.addSelectable(&m_warmStartTimer);
}

// stop the timer, take Select class "s" to remove the timer.
void AppRestartAssist::stopReconcileTimer(Select &s)
{
    m_warmStartTimer.stop();
    s.removeSelectable(&m_warmStartTimer);
}

// take Selectable class pointer "*s" to check if timer expired.
bool AppRestartAssist::checkReconcileTimer(Selectable *s)
{
    if(s == &m_warmStartTimer) {
        SWSS_LOG_INFO("warmstart timer expired");
        return true;
    }
    return false;
}

// check if left vector contains all elements of right vector
bool AppRestartAssist::contains(const std::vector<FieldValueTuple>& left,
              const std::vector<FieldValueTuple>& right)
{
    for (auto const& rv : right)
    {
        if (std::find(left.begin(), left.end(), rv) == left.end())
        {
            return false;
        }
    }

    return true;
}
