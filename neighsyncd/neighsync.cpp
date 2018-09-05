#include <string>
#include <netinet/in.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "ipaddress.h"
#include "netmsg.h"
#include "linkcache.h"
#include "select.h"

#include "neighsync.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

const AppRestartAssist::cache_state_map AppRestartAssist::cacheStateMap =
{
    {STALE,     "STALE"},
    {SAME,      "SAME"},
    {NEW,       "NEW"},
    {DELETE,    "DELETE"},
    {UNKNOWN,   "UNKNOWN"}
};

AppRestartAssist::AppRestartAssist(RedisPipeline *pipelineAppDB,
    const std::string &app_name, const std::string &docker_name,
    ProducerStateTable *ps_table, const uint32_t defaultWarmStartTimerValue):
    m_appTable(pipelineAppDB, APP_NEIGH_TABLE_NAME, false),
    m_appName(app_name),
    m_dockerName(docker_name),
    m_psTable(ps_table),
    warmStartTimer(timespec{0, 0})
{
    WarmStart::checkWarmStart(m_appName, m_dockerName);

    m_appTableName = m_appTable.getTableName();

    // set the default timer value
    if (defaultWarmStartTimerValue != 0)
    {
        m_reconcileTimer = defaultWarmStartTimerValue;
    }

    if (!WarmStart::isWarmStart())
    {
        m_warmStartInProgress = false;
    }
    else
    {
        m_warmStartInProgress = true;
        uint32_t temp_value = WarmStart::getWarmStartTimer(m_appName, m_dockerName);
        if (temp_value != 0)
        {
            m_reconcileTimer = temp_value;
        }

        warmStartTimer.setInterval(timespec{m_reconcileTimer, 0});

        // Clear the producerstate table to make sure no pending data for the AppTable
        m_psTable->clear();

        WarmStart::setWarmStartState(m_appName, WarmStart::INIT);
    }
}

AppRestartAssist::~AppRestartAssist()
{
}

/* join the field-value strings for straight printing */
string AppRestartAssist::joinVectorString(const vector<FieldValueTuple> &fv)
{
    string s;
    for (const auto &temps : fv )
    {
	   s += temps.first + ":" + temps.second + ", ";
    }
    return s;
}

void AppRestartAssist::setCacheEntryState(std::vector<FieldValueTuple> &fvVector,
    cache_state_t state)
{
    fvVector.back().second = cacheStateMap.at(state);
}

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
    return UNKNOWN;
}

/* Read table from APPDB and append stale flag then insert to cachemap */
void AppRestartAssist::readTableToMap()
{
    vector<string> keys;

    m_appTable.getKeys(keys);
    FieldValueTuple state(CACHE_STATE_FIELD, "");

    for (const auto &key: keys)
    {
        vector<FieldValueTuple> fv;

	    /* if the fieldvalue is empty, skip */
        if (!m_appTable.get(key, fv))
        {
            continue;
        }

        fv.push_back(state);
        setCacheEntryState(fv, STALE);

        string s = "";
        s = joinVectorString(fv);

        SWSS_LOG_INFO("write to cachemap: %s, key: %s, "
               "%s", m_appTableName.c_str(), key.c_str(), s.c_str());

        // insert to the cache map
        appTableCacheMap[key] = fv;
    }
    WarmStart::setWarmStartState(m_appName, WarmStart::RESTORED);
    SWSS_LOG_NOTICE("Restored appDB table to internal cache map");
    return;
}

void AppRestartAssist::insertToMap(string key, vector<FieldValueTuple> fvVector, bool delete_key)
{
    SWSS_LOG_INFO("Received message %s, key: %s, "
            "%s, delete = %d", m_appTableName.c_str(), key.c_str(), joinVectorString(fvVector).c_str(), delete_key);

    /*
     * Check and insert to CacheMap Logic:
     * if delete_key, mark the entry as "DELETE";
     * else:
     *  if key exist {
     *    if it has different value: update with "NEW" flag.
     *    if same value:  mark it as "SAME";
     *  } else {
     *    insert with "NEW" flag.
     *   }
     */

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
        /* check only the original vector range (exclude cache-state field/value) */
        if(!equal(fvVector.begin(), fvVector.end(), found->second.begin()))
        {
            SWSS_LOG_NOTICE("%s, found key: %s, new value ", m_appTableName.c_str(), key.c_str());

            FieldValueTuple state(CACHE_STATE_FIELD, "");
            fvVector.push_back(state);

            //mark as NEW flag
            setCacheEntryState(fvVector, NEW);
            appTableCacheMap[key] = fvVector;
        }
        else
        {
            SWSS_LOG_INFO("%s, found key: %s, same value", m_appTableName.c_str(), key.c_str());

            //mark as SAME flag
            setCacheEntryState(found->second, SAME);
        }
    }
    else
    {
        SWSS_LOG_NOTICE("%s, not found key: %s, new", m_appTableName.c_str(), key.c_str());
        FieldValueTuple state(CACHE_STATE_FIELD, "");
        fvVector.push_back(state);
        setCacheEntryState(fvVector, NEW);
        appTableCacheMap[key] = fvVector;
    }

    return;
}

void AppRestartAssist::reconcile()
{
    /*
       iterate throught the table
       if the entry has "SAME" flag, do nothing
       if has "STALE/DELETE" flag, delete it from appDB.
       else if "NEW" flag,  add it to appDB
       else, throw (should not happen)
    */
    SWSS_LOG_NOTICE("Hit reconcile function");
    for (auto iter = appTableCacheMap.begin(); iter != appTableCacheMap.end(); ++iter )
    {
        string s = "";
        s = joinVectorString(iter->second);
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
    // clear the map
    appTableCacheMap.clear();
    WarmStart::setWarmStartState(m_appName, WarmStart::RECONCILED);
    m_warmStartInProgress = false;
    return;
}

//start the timer, take Select class "s" to add the timer.
void AppRestartAssist::startReconcileTimer(Select &s)
{
    warmStartTimer.start();
    s.addSelectable(&warmStartTimer);
}

// stop the timer, take Select class "s" to remove the timer.
void AppRestartAssist::stopReconcileTimer(Select &s)
{
    warmStartTimer.stop();
    s.removeSelectable(&warmStartTimer);
}

// take Selectable class pointer "*s" to check if timer expired.
bool AppRestartAssist::checkReconcileTimer(Selectable *s)
{
    if(s == &warmStartTimer) {
        SWSS_LOG_INFO("warmstart timer expired");
        return true;
    }
    return false;
}

NeighSync::NeighSync(RedisPipeline *pipelineAppDB) :
    m_neighTable(pipelineAppDB, APP_NEIGH_TABLE_NAME),
    m_AppRestartAssist(pipelineAppDB, "neighsyncd", "swss", &m_neighTable, DEFAULT_RECONCILE_TIMER)
{
}

void NeighSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char ipStr[MAX_ADDR_SIZE + 1] = {0};
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_neigh *neigh = (struct rtnl_neigh *)obj;
    string key;
    string family;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
        return;

    if (rtnl_neigh_get_family(neigh) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_neigh_get_family(neigh) == AF_INET6)
        family = IPV6_NAME;
    else
        return;

    key+= LinkCache::getInstance().ifindexToName(rtnl_neigh_get_ifindex(neigh));
    key+= ":";

    nl_addr2str(rtnl_neigh_get_dst(neigh), ipStr, MAX_ADDR_SIZE);
    /* Ignore IPv6 link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
        return;
    /* Ignore IPv6 multicast link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_MC_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
        return;
    key+= ipStr;

    int state = rtnl_neigh_get_state(neigh);
    bool delete_key = false;
    if ((nlmsg_type == RTM_DELNEIGH) || (state == NUD_INCOMPLETE) ||
        (state == NUD_FAILED))
    {
	    delete_key = true;
    }

    nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("family", family);
    FieldValueTuple nh("neigh", macStr);
    fvVector.push_back(nh);
    fvVector.push_back(f);

    if (m_AppRestartAssist.isWarmStartInProgress())
    {
        m_AppRestartAssist.insertToMap(key, fvVector, delete_key);
    }
    else
    {
        if (delete_key == true)
        {
            m_neighTable.del(key);
            return;
        }
        m_neighTable.set(key, fvVector);
    }
}
