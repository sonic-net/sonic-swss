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

#include "neighsync.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

const NeighRestartAssist::cache_state_map NeighRestartAssist::cacheStateMap =
{
    {STALE,     "STALE"},
    {SAME,      "SAME"},
    {NEW,       "NEW"},
    {DELETE,    "DELETE"},
    {UNKNOWN,   "UNKNOWN"}
};

NeighRestartAssist::NeighRestartAssist(RedisPipeline *pipelineAppDB,
    const std::string &app_name, const std::string &docker_name,
    ProducerStateTable *ps_table):
    m_appTable(pipelineAppDB, APP_NEIGH_TABLE_NAME, false),
    m_appName(app_name),
    m_dockerName(docker_name),
    m_psTable(ps_table)
{
    WarmStart::checkWarmStart(m_appName, m_dockerName);

    m_appTableName = m_appTable.getTableName();

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
        // Clear the producerstate table to make sure no pending data for the AppTable
        // See sonic-swss-common PR-227, will add the call here once the PR is done.
        //m_psTable->drop();

        WarmStart::setWarmStartState(m_appName, WarmStart::INIT);
    }
}

NeighRestartAssist::~NeighRestartAssist()
{
}

/* join the field-value strings for straight printing */
string NeighRestartAssist::joinVectorString(const vector<FieldValueTuple> &fv)
{
    string s;
    for (const auto &temps : fv )
    {
	   s += temps.first + ":" + temps.second + ", ";
    }
    return s;
}

void NeighRestartAssist::setCacheEntryState(std::vector<FieldValueTuple> &fvVector,
    cache_state_t state)
{
    fvVector.back().second = cacheStateMap.at(state);
}

NeighRestartAssist::cache_state_t NeighRestartAssist::getCacheEntryState(const std::vector<FieldValueTuple> &fvVector)
{
    for (auto &iter : cacheStateMap)
    {
        if (fvVector.back().second == iter.second)
        {
            return iter.first;
        }
    }
    assert("Cache Entry State is corrupted" == NULL);
    return UNKNOWN;
}

/* Read table from APPDB and append stale flag then insert to cachemap */
void NeighRestartAssist::readTableToMap()
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
        neighborCacheMap[key] = fv;
    }
    WarmStart::setWarmStartState(m_appName, WarmStart::RESTORED);
    SWSS_LOG_NOTICE("Restored appDB table to internal cache map");
    return;
}

void NeighRestartAssist::insertToMap(string key, vector<FieldValueTuple> fvVector, bool delete_key)
{
    string s;
    s = joinVectorString(fvVector);

    SWSS_LOG_INFO("Received message %s, key: %s, "
            "%s, delete = %d", m_appTableName.c_str(), key.c_str(), s.c_str(), delete_key);

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

    auto found = neighborCacheMap.find(key);

    if (delete_key)
    {
        SWSS_LOG_NOTICE("%s, delete key: %s, ", m_appTableName.c_str(), key.c_str());
        /* mark it as DELETE if exist, otherwise, no-op */
        if (found != neighborCacheMap.end())
        {
            setCacheEntryState(found->second, DELETE);
        }
    }
    else if (found != neighborCacheMap.end())
    {
        /* check only the original vector range (exclude cache-state field/value) */
        if(!equal(fvVector.begin(), fvVector.end(), found->second.begin()))
        {
            SWSS_LOG_NOTICE("%s, found key: %s, new value ", m_appTableName.c_str(), key.c_str());

            FieldValueTuple state(CACHE_STATE_FIELD, "");
            fvVector.push_back(state);

            //mark as NEW flag
            setCacheEntryState(fvVector, NEW);
            neighborCacheMap[key] = fvVector;
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
        neighborCacheMap[key] = fvVector;
    }

    return;
}

void NeighRestartAssist::reconcile()
{
    /*
       iterate throught the table
       if the entry has "SAME" flag, do nothing
       if has "STALE/DELETE" flag, delete it from appDB.
       else if "NEW" flag,  add it to appDB
       else, assert (should not happen)
    */
    SWSS_LOG_NOTICE("Hit reconcile function");
    for (auto iter = neighborCacheMap.begin(); iter != neighborCacheMap.end(); ++iter )
    {
        string s = "";
        s = joinVectorString(iter->second);
        if (getCacheEntryState(iter->second) == SAME)
        {
            SWSS_LOG_INFO("%s SAME, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());
            continue;
        }
        else if (getCacheEntryState(iter->second) == STALE ||
            getCacheEntryState(iter->second) == DELETE)
        {
            SWSS_LOG_NOTICE("%s STALE/DELETE, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());

            //delete from appDB
            m_psTable->del(iter->first);
        }
        else if (getCacheEntryState(iter->second) == NEW)
        {
            SWSS_LOG_NOTICE("%s NEW, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());

            //add to appDB, exclude the state
            iter->second.pop_back();
            m_psTable->set(iter->first, iter->second);
        }
        else
        {
            assert("unknown type cache state" == NULL);
        }
    }
    // clear the map
    neighborCacheMap.clear();
    WarmStart::setWarmStartState(m_appName, WarmStart::RECONCILED);
    m_warmStartInProgress = false;
    return;
}

void NeighRestartAssist::startReconcileTimer()
{
    m_startTime = time(NULL);
}

bool NeighRestartAssist::checkReconcileTimer()
{
    m_secondsPast =  difftime(time(NULL), m_startTime);
    SWSS_LOG_INFO("restart timer past: %f seconds", m_secondsPast);
    if (m_secondsPast >= m_reconcileTimer)
    {
        return true;
    }
    return false;
}

NeighSync::NeighSync(RedisPipeline *pipelineAppDB) :
    m_neighTable(pipelineAppDB, APP_NEIGH_TABLE_NAME),
    m_neighRestartAssist(pipelineAppDB, "neighsyncd", "swss", &m_neighTable)
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

    if (m_neighRestartAssist.isWarmStartInProgress())
    {
        m_neighRestartAssist.insertToMap(key, fvVector, delete_key);
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
