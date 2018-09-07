#ifndef __WARM_RESTART_ASSIST__
#define __WARM_RESTART_ASSIST__

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
 */
class AppRestartAssist
{
public:
    AppRestartAssist(RedisPipeline *pipeline,
        const std::string &appName, const std::string &dockerName,
        ProducerStateTable *psTable, const uint32_t defaultWarmStartTimerValue = 0);
    virtual ~AppRestartAssist();

    enum cache_state_t
    {
        STALE	= 0,
        SAME 	= 1,
        NEW 	= 2,
        DELETE  = 3,
        UNKNOWN = 4
    };
    void startReconcileTimer(Select &s);
    void stopReconcileTimer(Select &s);
    bool checkReconcileTimer(Selectable *s);
    void readTableToMap(void);
    void insertToMap(std::string key, std::vector<FieldValueTuple> fvVector, bool delete_key);
    void reconcile(void);
    bool isWarmStartInProgress(void)
    {
        return m_warmStartInProgress;
    }

private:
    typedef std::map<cache_state_t, std::string> cache_state_map;
    static const cache_state_map cacheStateMap;
    const std::string CACHE_STATE_FIELD = "cache-state";
    static const uint32_t DEFAULT_INTERNAL_TIMER_VALUE = 5;
    typedef std::unordered_map<std::string, std::vector<swss::FieldValueTuple>> AppTableMap;
    AppTableMap appTableCacheMap;

    Table m_appTable;
    std::string m_dockerName;
    std::string m_appName;
    ProducerStateTable *m_psTable;
    std::string m_appTableName;

    bool m_warmStartInProgress;
    uint32_t m_reconcileTimer;
    SelectableTimer m_warmStartTimer;

    std::string joinVectorString(const std::vector<FieldValueTuple> &fv);
    void setCacheEntryState(std::vector<FieldValueTuple> &fvVector, cache_state_t state);
    cache_state_t getCacheEntryState(const std::vector<FieldValueTuple> &fvVector);
};

}

#endif
