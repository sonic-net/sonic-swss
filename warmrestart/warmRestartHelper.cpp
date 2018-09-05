#include <cassert>
#include <sstream>

#include "warmRestartHelper.h"


using namespace swss;


WarmStartHelper::WarmStartHelper(RedisPipeline      *pipeline,
                                 ProducerStateTable *syncTable,
                                 const std::string  &syncTableName,
                                 const std::string  &dockerName,
                                 const std::string  &appName) :
    m_restorationTable(pipeline, syncTableName, false),
    m_syncTable(syncTable),
    m_syncTableName(syncTableName),
    m_dockName(dockerName),
    m_appName(appName)
{
    WarmStart::initialize(appName, dockerName);
}


WarmStartHelper::~WarmStartHelper()
{
}


void WarmStartHelper::setState(WarmStart::WarmStartState state)
{
    WarmStart::setWarmStartState(m_appName, state);

    /* Caching warm-restart FSM state in local member */
    m_state = state;
}


WarmStart::WarmStartState WarmStartHelper::getState(void) const
{
    return m_state;
}


/*
 * To be called by each application to obtain the active/inactive state of
 * warm-restart functionality, and proceed to initialize the FSM accordingly.
 */
bool WarmStartHelper::isEnabled(void)
{
    bool enabled = WarmStart::checkWarmStart(m_appName, m_dockName);

    /*
     * If warm-restart feature is enabled for this application, proceed to
     * initialize its FSM, and clean any pending state that could be potentially
     * held in ProducerState queues.
     */
    if (enabled)
    {
        SWSS_LOG_NOTICE("Initializing Warm-Restart cycle for %s application.",
                        m_appName.c_str());

        setState(WarmStart::INITIALIZED);
        m_syncTable->clear();
    }

    /* Keeping track of warm-reboot active/inactive state */
    m_enabled = enabled;

    return enabled;
}


bool WarmStartHelper::isReconciled(void) const
{
    return (m_state == WarmStart::RECONCILED);
}


bool WarmStartHelper::inProgress(void) const
{
    return (m_enabled && m_state != WarmStart::RECONCILED);
}


uint32_t WarmStartHelper::getRestartTimer(void) const
{
    return WarmStart::getWarmStartTimer(m_appName, m_dockName);
}


/*
 * Invoked by warmStartHelper clients during initialization. All interested parties
 * are expected to call this method to upload their associated redisDB state into
 * a temporary buffer, which will eventually serve to resolve any conflict between
 * 'old' and 'new' state.
 */
bool WarmStartHelper::runRestoration()
{
    bool state_available;

    SWSS_LOG_NOTICE("Warm-Restart: Initiating AppDB restoration process for %s "
                    "application.", m_appName.c_str());

    if (buildRestorationMap())
    {
        setState(WarmStart::RESTORED);
        state_available = true;
    }
    else
    {
        setState( WarmStart::RECONCILED);
        state_available = false;
    }

    SWSS_LOG_NOTICE("Warm-Restart: Completed AppDB restoration process for %s "
                    "application.", m_appName.c_str());

    return state_available;
}


bool WarmStartHelper::buildRestorationMap(void)
{
    std::vector<KeyOpFieldsValuesTuple> restorationVector;

    m_restorationTable.getContent(restorationVector);
    if (!restorationVector.size())
    {
        SWSS_LOG_NOTICE("Warm-Restart: No records received from AppDB for %s "
                        "application.", m_appName.c_str());
        return false;
    }
    SWSS_LOG_NOTICE("Warm-Restart: Received %d records from AppDB for %s "
                    "application.",
                    static_cast<int>(restorationVector.size()),
                    m_appName.c_str());

    /* Proceed to insert every restored element into the reconciliation buffer */
    for (auto &elem : restorationVector)
    {
        insertRestorationMap(elem, STALE);
    }

    return true;
}


/*
 * Method in charge of populating the restorationMap with old/new state. This state
 * can either come from southbound data-stores (old/existing state) or from any
 * of the applications (new state) interested in warm-reboot capabilities.
 */
void WarmStartHelper::insertRestorationMap(const KeyOpFieldsValuesTuple &kfv,
                                           fvState_t                     state)
{
    std::string key = kfvKey(kfv);
    std::vector<FieldValueTuple> fieldValues = kfvFieldsValues(kfv);

    fieldValuesTupleVoV fvVector;

    /*
     * Iterate through all the fieldValue-tuples present in this kfv entry to
     * split its values into separated tuples. Store these separated tuples in
     * a temporary fieldValue vector.
     *
     * Here we are simply converting from KFV format to a split-based layout
     * represented by the fvVector variable. Notice that the conversion for
     * applications with no special requirements (i.e one simple field-value per
     * key) is straightforward.
     *
     * Example 1 (fpmsyncd):
     *
     * input kfv: 1.1.1.1/30, vector{nexthop: 10.1.1.1, 10.1.1.2, ifname: eth1, eth2}
     *
     * output fvVector: vector{v1{nexthop: 10.1.1.1, ifname: eth1},
     *                         v2{nexthop: 10.1.1.2, ifname: eth2}}
     *
     * Example 2 (neighsyncd):
     *
     * input kfv: Ethernet0:1.1.1.1, vector{neigh: 00:00:00:00:00:01, family: IPv4}
     *
     * output fvVector: vector{v1{neigh: 00:00:00:00:00:01, family: IPv4}}
     *
     */
    for (auto &fv : fieldValues)
    {
        std::string field = fvField(fv);
        std::vector<std::string> splitValues = tokenize(fvValue(fv), ',');

        /*
         * Dealing with tuples with empty values. Example: directly connected
         * intfs will show up as [ nexthop = "" ]
         */
        if (!splitValues.size())
        {
            splitValues.push_back("");
        }

        for (int j = 0; j < static_cast<int>(splitValues.size()); ++j)
        {
            if (j < static_cast<int>(fvVector.size()))
            {
                fvVector[j].emplace_back(field, splitValues[j]);
            }
            else
            {
                fvVector.emplace_back(
                    std::vector<FieldValueTuple>{make_pair(field, splitValues[j])});
            }
        }
    }

    /*
     * Proceeding to insert/update the received fieldvalue entries into the
     * restorationMap.
     */
    fvRestorationMap fvMap;

    if (m_restorationMap.count(key))
    {
        fvMap = m_restorationMap[key];
    }

    /*
     * Let's now deal with transient (best-path) selections, which is only
     * required when we are receiving new/refreshed state from north-bound apps
     * (CLEAN flag).
     */
    if (state == CLEAN)
    {
        adjustRestorationMap(fvMap, fvVector, key);
    }

    /*
     * We will iterate through each of the fieldvalue-tuples in the fvVector to
     * either insert or update the corresponding entry in the map.
     */
    for (auto &elem : fvVector)
    {
        if (fvMap.find(elem) == fvMap.end())
        {
            if (state == STALE)
            {
                fvMap[elem] = STALE;
            }
            else if (state == CLEAN)
            {
                fvMap[elem] = NEW;
            }
        }
        else
        {
            fvMap[elem] = state;
        }
    }

    m_restorationMap[key] = fvMap;
}


/*
 * Method takes care of marking eliminated entries from the restorationMap buffer.
 */
void WarmStartHelper::removeRestorationMap(const KeyOpFieldsValuesTuple &kfv,
                                           fvState_t                     state)
{
    fvRestorationMap fvMap;

    /*
     * There's no point in processing state-withdrawal if an associated entry
     * doesn't exist in the restorationMap.
     */
    std::string key = kfvKey(kfv);
    if (!m_restorationMap.count(key))
    {
        return;
    }

    fvMap = m_restorationMap[key];

    /*
     * Iterate through all elements in the map and update the state of the
     * entries being withdrawn with the proper flag.
     */
    for (auto &fv : fvMap)
    {
        fv.second = state;
    }

    m_restorationMap[key] = fvMap;
}


/*
 * This method is currently required to deal with a specific limitation of quagga
 * and frr routing-stacks, which causes transient best-path selections to arrive
 * at fpmSyncd during bgp's initial peering establishments. In these scenarios we
 * must identify the 'transient' character of a routing-update and eliminate it
 * from the restorationMap whenever a better one is received.
 *
 * As this issue is only observed when interacting with the routing-stack, we can
 * safely avoid this call when collecting state from AppDB (restoration phase);
 * hence caller should invoke this method only if/when the state of the new entry
 * to add is set to CLEAN.
 */
void WarmStartHelper::adjustRestorationMap(fvRestorationMap          &fvMap,
                                           const fieldValuesTupleVoV &fvVector,
                                           const std::string         &key)
{
    /*
     * Iterate through all field-value entries in the fvMap and determine if there's
     * a matching entry in the fvVector. If that's not the case, and this entry has
     * been recently added by the north-bound app (NEW flag), then proceed to
     * eliminate it from the fvMap.
     *
     * Notice that even though this is an O(n^2) logic, 'n' here is small (number
     * of ecmp-paths per prefix), and this is only executed during restarting
     * events.
     */
    for (auto it = fvMap.begin(); it != fvMap.end(); )
    {
        bool found = false;
        /*
         * Transient best-path selections would only apply to entries marked as
         * NEW.
         */
        if (it->second != NEW)
        {
            it++;
            continue;
        }

        for (auto const &fv : fvVector)
        {
            if (it->first == fv)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            SWSS_LOG_INFO("Warm-Restart: Deleting transient best-path selection "
                          "for entry %s\n", key.c_str());
            it = fvMap.erase(it);
        }
        else
        {
            it++;
        }
    }
}


/*
 * Reconciliation process takes place here.
 *
 * In a nutshell, the process relies on the following basic guidelines:
 *
 * - An element in the restorationMap with all its entries in the fvResMap showing
 *   as STALE, will be eliminated from AppDB.
 *
 * - An element in the restorationMap with all its entries in the fvResMap showing
 *   as CLEAN, will have a NO-OP associated with it -- no changes in AppDB.
 *
 * - An element in the restorationMap with all its entries in the fvResMap showing
 *   as NEW, will correspond to a brand-new state, and as such, will be pushed to
 *   AppDB.
 *
 * - An element in the restorationMap with some of its entries in the fvResMap
 *   showing as CLEAN, will have these CLEAN entries, along with any NEW one,
 *   being pushed down to AppDB.
 *
 * - An element in the restorationMap with some of its entries in the fvResMap
 *   showing as NEW, will have these new entries, along with any CLEAN one,
 *   being pushed to AppDB.
 *
 * - An element in the restorationMap with some/all of its entries in the
 *   fvResMap showing as DELETE, will have these entries being eliminated
 *   from AppDB.
 *
 */
void WarmStartHelper::reconcile(void)
{
    SWSS_LOG_NOTICE("Warm-Restart: Initiating reconciliation process for %s "
                    "application.", m_appName.c_str());

    assert(getState() == WarmStart::RESTORED);

    /*
     * Iterate through all the entries in the restorationMap and take note of the
     * attributes associated to each.
     */
    auto it = m_restorationMap.begin();
    while (it != m_restorationMap.end())
    {
        std::string key = it->first;
        fvRestorationMap fvMap = it->second;

        int totalRecElems, staleRecElems, cleanRecElems, newRecElems, deleteRecElems;
        totalRecElems = staleRecElems = cleanRecElems = newRecElems = deleteRecElems = 0;

        std::vector<FieldValueTuple> fvVector;

        for (auto itMap = fvMap.begin(); itMap != fvMap.end(); )
        {
            totalRecElems++;

            auto recElem = itMap->first;
            auto recElemState = itMap->second;

            if (recElemState == STALE)
            {
                itMap = fvMap.erase(itMap);
                staleRecElems++;
            }
            else if (recElemState == CLEAN)
            {
                cleanRecElems++;
                transformKFV(recElem, fvVector);
                ++itMap;
            }
            else if (recElemState == NEW)
            {
                newRecElems++;
                transformKFV(recElem, fvVector);
                ++itMap;
            }
            else if (recElemState == DELETE)
            {
                deleteRecElems++;
                ++itMap;
            }
        }

        if (staleRecElems == totalRecElems)
        {
            m_syncTable->del(key);
            SWSS_LOG_NOTICE("Warm-Restart reconciliation: deleting stale entry %s",
                            key.c_str());
        }
        else if (cleanRecElems == totalRecElems)
        {
            SWSS_LOG_INFO("Warm-Restart reconciliation: no changes needed for "
                          "existing entry %s", key.c_str());
        }
        else if (newRecElems == totalRecElems)
        {
            m_syncTable->set(key, fvVector);
            SWSS_LOG_NOTICE("Warm-Restart reconciliation: creating new entry %s",
                            key.c_str());
        }
        else if (cleanRecElems)
        {
            m_syncTable->set(key, fvVector);
            SWSS_LOG_NOTICE("Warm-Restart reconciliation: updating attributes "
                            "for entry %s", key.c_str());
        }
        else if (newRecElems)
        {
            m_syncTable->set(key, fvVector);
            SWSS_LOG_NOTICE("Warm-Restart reconciliation: creating new attributes "
                            "for entry %s", key.c_str());
        }
        else if (deleteRecElems)
        {
            m_syncTable->del(key);
            SWSS_LOG_NOTICE("Warm-Restart reconciliation: deleting entry %s",
                            key.c_str());
        }

        it = m_restorationMap.erase(it);
    }

    /* Restoration map should be entirely empty by now */
    assert(m_restorationMap.size() == 0);

    setState(WarmStart::RECONCILED);

    SWSS_LOG_NOTICE("Warm-Restart: Concluded reconciliation process for %s "
                    "application.", m_appName.c_str());
}


/*
 * Method useful to transform fieldValueTuples from the split-format utilized
 * in warmStartHelper's reconciliation process to the regular format used
 * everywhere else.
 */
void WarmStartHelper::transformKFV(const std::vector<FieldValueTuple> &data,
                                   std::vector<FieldValueTuple>       &fvVector)
{
    bool emptyVector = false;

    /*
     * Both input vectors should contain the same number of elements, with the
     * exception of fvVector not being initialized yet.
     */
    if (data.size() != fvVector.size() && fvVector.size())
    {
        return;
    }
    else if (!fvVector.size())
    {
        emptyVector = true;
    }

    /* Define fields in fvVector result-parameter */
    for (int i = 0; i < static_cast<int>(data.size()); ++i)
    {
        if (emptyVector)
        {
            fvVector.push_back(data[i]);
            continue;
        }

        std::string newVal = fvValue(fvVector[i]) + "," + fvValue(data[i]);

        fvVector[i].second = newVal;
    }
}
