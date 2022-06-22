#ifndef __ADVANCED_RESTART_ASSIST__
#define __ADVANCED_RESTART_ASSIST__

#include <unordered_map>
#include <string>
#include "dbconnector.h"
#include "table.h"
#include "producerstatetable.h"
#include "selectabletimer.h"
#include "select.h"

namespace swss {

/*
 * This class is to support application table reconciliation
 * For any application table which has entries with key -> vector<f1/v2, f2/v2..>
 * It is expect that the application owner keeps the order of f/v pairs
 * The application usually takes this class as composition, i.e. includes an instance of
 * this class in their classes.
 * A high level flow to use this class:
 * 1. Include this class in the application class: appClass.
 *
 * 2. Construct appClass along with this class with:
 *    docker name, application name, timer value etc
 *
 * 3. Define Select s;
 *    Check if advancedstart enabled, if so,read application table to cache and start timer:
 *      if (appClass.getRestartAssist()->isAdvancedStartInProgress())
 *      {
 *          appClass.getRestartAssist()->readTableToMap()
 *          appClass.getRestartAssist()->startReconcileTimer(s);
 *       }
 *
 * 4. Before the reconcile timer is expired, insert all requests into cache:
 *      if (m_AppRestartAssist.isAdvancedStartInProgress())
 *      {
 *          m_AppRestartAssist.insertToMap(key, fvVector, delete_key);
 *      }
 *
 * 5. In the select loop, check if the reconcile timer is expired, if so,
 *    stop timer and call the reconcilation function:
 *      Selectable *temps;
 *      s.select(&temps);
 *      if (appClass.getRestartAssist()->isAdvancedStartInProgress())
 *      {
 *          if (appClass.getRestartAssist()->checkReconcileTimer(temps))
 *          {
 *              appClass.getRestartAssist()->stopReconcileTimer(s);
 *              appClass.getRestartAssist()->reconcile();
 *          }
 *      }
 */
typedef std::map <std::string, Table *>              Tables;
typedef std::map <std::string, ProducerStateTable *> ProducerStateTables;

class AppRestartAssist
{
public:
    AppRestartAssist(RedisPipeline *pipeline, const std::string &appName,
                     const std::string &dockerName, const uint32_t defaultAdvancedStartTimerValue = 0);
    virtual ~AppRestartAssist();

    /*
     * cache entry state
     * STALE : Default in the cache, if no refresh for this entry,it is STALE
     * SAME  : Same entry was added to cache later
     * NEW   : New entry was added to cache later
     * DELETE: Entry was deleted later
     */
    enum cache_state_t
    {
        STALE	= 0,
        SAME 	= 1,
        NEW 	= 2,
        DELETE  = 3
    };
    // These functions were used as described in the class description
    void setReconcileInterval(uint32_t time);
    void startReconcileTimer(Select &s);
    void stopReconcileTimer(Select &s);
    bool checkReconcileTimer(Selectable *s);
    void readTablesToMap(void);
    void appDataReplayed(void);
    void advancedStartDisabled(void);
    void insertToMap(std::string tableName, std::string key, std::vector<FieldValueTuple> fvVector, bool delete_key);
    void reconcile(void);
    bool isAdvancedStartInProgress(void)
    {
        return m_advancedStartInProgress;
    }
    void registerAppTable(const std::string &tableName, ProducerStateTable *psTable);

private:
    typedef std::map<cache_state_t, std::string> cache_state_map;
    // Enum to string translation map
    static const cache_state_map cacheStateMap;
    const std::string CACHE_STATE_FIELD = "cache-state";

    /*
     * Default timer to be 5 seconds
     * Overwritten by application loading this class and configurations in configDB
     * Precedence ascent order: Default -> loading class with value -> configuration
     */
    static const uint32_t DEFAULT_INTERNAL_TIMER_VALUE = 5;
    typedef std::map<std::string, std::unordered_map<std::string, std::vector<swss::FieldValueTuple>>> AppTableMap;

    // cache map to store temporary application table
    AppTableMap appTableCacheMap;

    RedisPipeline      *m_pipeLine;
    Tables              m_appTables;  // app tables
    std::string         m_dockerName; // docker name of the application
    std::string         m_appName;    // application name
    ProducerStateTables m_psTables;   // producer state tables

    bool m_advancedStartInProgress;       // indicate if advanced start is in progress
    time_t m_reconcileTimer;          // reconcile timer value
    SelectableTimer m_advancedStartTimer; // reconcile timer

    // Set or get cache entry state
    std::string joinVectorString(const std::vector<FieldValueTuple> &fv);
    void setCacheEntryState(std::vector<FieldValueTuple> &fvVector, cache_state_t state);
    cache_state_t getCacheEntryState(const std::vector<FieldValueTuple> &fvVector);
    bool contains(const std::vector<FieldValueTuple>& left,
                  const std::vector<FieldValueTuple>& right);
};

}

#endif
