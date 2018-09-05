#ifndef __NEIGHSYNC__
#define __NEIGHSYNC__

#include <unordered_map>
#include <string>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "selectabletimer.h"

#define DEFAULT_RECONCILE_TIMER 5

namespace swss {

/*
 * This class is to support application table reconciliation
 * For any application table which has entries with key -> vector<f1/v2, f2/v2..>
 */
class AppRestartAssist
{
public:
    AppRestartAssist(RedisPipeline *pipelineAppDB,
        const std::string &app_name, const std::string &docker_name,
        ProducerStateTable *ps_table, const uint32_t defaultWarmStartTimerValue = 0);
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
    typedef std::unordered_map<std::string, std::vector<swss::FieldValueTuple>> AppTableMap;
    AppTableMap appTableCacheMap;

    Table m_appTable;
    std::string m_dockerName;
    std::string m_appName;
    ProducerStateTable *m_psTable;
    std::string m_appTableName;

    bool m_warmStartInProgress;
    uint32_t m_reconcileTimer;
    SelectableTimer warmStartTimer;

    std::string joinVectorString(const std::vector<FieldValueTuple> &fv);
    void setCacheEntryState(std::vector<FieldValueTuple> &fvVector, cache_state_t state);
    cache_state_t getCacheEntryState(const std::vector<FieldValueTuple> &fvVector);
};

class NeighSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    NeighSync(RedisPipeline *pipelineAppDB);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    AppRestartAssist *getRestartAssist()
    {
        return &m_AppRestartAssist;
    }

private:
    ProducerStateTable m_neighTable;
    AppRestartAssist m_AppRestartAssist;
};

}

#endif
