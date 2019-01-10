#include <string>
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

AppRestartAssist::AppRestartAssist(RedisPipeline *pipeline,
    const std::string &appName, const std::string &dockerName,
    ProducerStateTable *psTable, const uint32_t defaultWarmStartTimerValue):
    m_appTable(pipeline, APP_NEIGH_TABLE_NAME, false),
    m_appName(appName),
    m_dockerName(dockerName),
    m_psTable(psTable),
    m_warmStartTimer(timespec{0, 0})
{
    WarmStart::initialize(m_appName, m_dockerName);
    WarmStart::checkWarmStart(m_appName, m_dockerName);

    m_appTableName = m_appTable.getTableName();

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

        // Clear the producerstate table to make sure no pending data for the AppTable
        m_psTable->clear();

        WarmStart::setWarmStartState(m_appName, WarmStart::INITIALIZED);
    }
}

AppRestartAssist::~AppRestartAssist()
{
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

// Read table from APPDB and append stale flag then insert to cachemap
void AppRestartAssist::readTableToMap()
{
    vector<string> keys;

    m_appTable.getKeys(keys);
    FieldValueTuple state(CACHE_STATE_FIELD, "");

    for (const auto &key: keys)
    {
        vector<FieldValueTuple> fv;

	    // if the fieldvalue is empty, skip
        if (!m_appTable.get(key, fv))
        {
            continue;
        }

        fv.push_back(state);
        setCacheEntryState(fv, STALE);

        string s = joinVectorString(fv);

        SWSS_LOG_INFO("write to cachemap: %s, key: %s, "
               "%s", m_appTableName.c_str(), key.c_str(), s.c_str());

        // insert to the cache map
        appTableCacheMap[key] = fv;
    }
    WarmStart::setWarmStartState(m_appName, WarmStart::RESTORED);
    SWSS_LOG_NOTICE("Restored appDB table to internal cache map");
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
void AppRestartAssist::insertToMap(string key, vector<FieldValueTuple> fvVector, bool delete_key)
{
    SWSS_LOG_INFO("Received message %s, key: %s, "
            "%s, delete = %d", m_appTableName.c_str(), key.c_str(), joinVectorString(fvVector).c_str(), delete_key);


    auto found = appTableCacheMap.find(key);

    if (delete_key)
    {
        SWSS_LOG_NOTICE("%s, delete key: %s, ", m_appTableName.c_str(), key.c_str());
        /* mark it as DELETE if exist, otherwise, no-op */
        if (found != appTableCacheMap.end())
        {
            setCacheEntryState(found->second, DELETE);
        }
    }
    else if (found != appTableCacheMap.end())
    {
        // check only the original vector range (exclude cache-state field/value)
        if(!equal(fvVector.begin(), fvVector.end(), found->second.begin()))
        {
            SWSS_LOG_NOTICE("%s, found key: %s, new value ", m_appTableName.c_str(), key.c_str());

            FieldValueTuple state(CACHE_STATE_FIELD, "");
            fvVector.push_back(state);

            // mark as NEW flag
            setCacheEntryState(fvVector, NEW);
            appTableCacheMap[key] = fvVector;
        }
        else
        {
            SWSS_LOG_INFO("%s, found key: %s, same value", m_appTableName.c_str(), key.c_str());

            // mark as SAME flag
            setCacheEntryState(found->second, SAME);
        }
    }
    else
    {
        // not found, mark the entry as NEW and insert to map
        SWSS_LOG_NOTICE("%s, not found key: %s, new", m_appTableName.c_str(), key.c_str());
        FieldValueTuple state(CACHE_STATE_FIELD, "");
        fvVector.push_back(state);
        setCacheEntryState(fvVector, NEW);
        appTableCacheMap[key] = fvVector;
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

    SWSS_LOG_ENTER();
    for (auto iter = appTableCacheMap.begin(); iter != appTableCacheMap.end(); ++iter )
    {
        string s = joinVectorString(iter->second);
        auto state = getCacheEntryState(iter->second);

        if (state == SAME)
        {
            SWSS_LOG_INFO("%s SAME, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());
            continue;
        }
        else if (state == STALE || state == DELETE)
        {
            SWSS_LOG_NOTICE("%s STALE/DELETE, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());

            //delete from appDB
            m_psTable->del(iter->first);
        }
        else if (state == NEW)
        {
            SWSS_LOG_NOTICE("%s NEW, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());

            //add to appDB, exclude the state
            iter->second.pop_back();
            m_psTable->set(iter->first, iter->second);
        }
        else
        {
            throw std::logic_error("cache entry state is invalid");
        }
    }
    // reconcile finished, clear the map, mark the warmstart state
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
