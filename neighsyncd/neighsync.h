#ifndef __NEIGHSYNC__
#define __NEIGHSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include <unordered_map>
#include <string>

#define DEFAULT_RECONCILE_TIMER 5
#define SELECT_TIMEOUT 1000
#define CACHE_STATE_FIELD	"cache-state"

namespace swss {

/*
 * This class is to support neighbor table reconciliation
 * However the design supports any application table which has
 * entries with key -> vector<f1/v2, f2/v2..>
 */
class NeighRestartAssist
{
public:
    NeighRestartAssist(RedisPipeline *pipelineAppDB,
        const std::string &app_name, const std::string &docker_name,
        ProducerStateTable *ps_table);
    virtual ~NeighRestartAssist();

    enum cache_state_t
    {
        STALE	= 0,
        SAME 	= 1,
        NEW 	= 2,
        DELETE  = 3,
        UNKNOWN = 4
    };
    void startReconcileTimer(void);
    bool checkReconcileTimer(void);
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

    typedef std::unordered_map<std::string, std::vector<swss::FieldValueTuple>> AppTableMap;
    AppTableMap neighborCacheMap;

    Table m_appTable;
    std::string m_dockerName;
    std::string m_appName;
    ProducerStateTable *m_psTable;
    std::string m_appTableName;

    bool m_warmStartInProgress;
    uint32_t m_reconcileTimer = DEFAULT_RECONCILE_TIMER;
    time_t m_startTime;
    double m_secondsPast = 0;

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

    NeighRestartAssist *getRestartAssist()
    {
        return &m_neighRestartAssist;
    }

private:
    ProducerStateTable m_neighTable;
    NeighRestartAssist m_neighRestartAssist;
};

}

#endif
