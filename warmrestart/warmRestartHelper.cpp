#include <cassert>
#include <sstream>

#include "warmRestartHelper.h"


using namespace swss;


WarmStartHelper::WarmStartHelper(RedisPipeline      *pipeline,
                                 ProducerStateTable *syncTable,
                                 const std::string  &dockerName,
                                 const std::string  &appName) :
    m_recoveryTable(pipeline, APP_ROUTE_TABLE_NAME, false),
    m_syncTable(syncTable),
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

    m_state = state;
}


WarmStart::WarmStartState WarmStartHelper::getState(void) const
{
    return m_state;
}


bool WarmStartHelper::isEnabled(void) const
{
    return WarmStart::checkWarmStart(m_appName, m_dockName);
}


bool WarmStartHelper::isReconciled(void) const
{
    return (m_state == WarmStart::RECONCILED);
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
bool WarmStartHelper::runRecovery()
{
    bool state_available;

    SWSS_LOG_NOTICE("Initiating AppDB restoration process");

    if (buildRecoveryMap())
    {
        setState(WarmStart::RESTORED);
        state_available = true;
    }
    else
    {
        setState( WarmStart::RECONCILED);
        state_available = false;
    }

    SWSS_LOG_NOTICE("Completed AppDB restoration process");

    return state_available;
}


bool WarmStartHelper::buildRecoveryMap(void)
{
    std::vector<KeyOpFieldsValuesTuple> recoveryVector;

    m_recoveryTable.getContent(recoveryVector);
    if (!recoveryVector.size())
    {
        SWSS_LOG_NOTICE("Warm-Restart: No records received from AppDB\n");
        return false;
    }
    SWSS_LOG_NOTICE("Warm-Restart: Received %d records from AppDB\n",
                    static_cast<int>(recoveryVector.size()));

    /* Proceed to insert every recovered element into the reconciliation buffer */
    for (auto &elem : recoveryVector)
    {
        insertRecoveryMap(elem, STALE);
    }

    return true;
}


/*
 * Method in charge of populating the recoveryMap with old/new state. This state
 * can either come from southbound data-stores (old/existing state) or from any
 * of the applications (new state) interested in graceful-restart capabilities.
 */
void WarmStartHelper::insertRecoveryMap(const KeyOpFieldsValuesTuple &kfv,
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
     * represented by the fvVector variable.
     *
     * input kfv: 1.1.1.1/30, vector{nexthop: 10.1.1.1, 10.1.1.2, ifname: eth1, eth2}
     *
     * output fvVector: vector{v1{nexthop: 10.1.1.1, ifname: eth1},
     *                         v2{nexthop: 10.1.1.2, ifname: eth2}}
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
                fvVector.emplace_back(std::vector<FieldValueTuple>{make_pair(field, splitValues[j])});
            }
        }
    }

    /*
     * Now that we have a fvVector with separated fieldvalue-tuples, let's proceed
     * to insert/update its fieldvalue entries into our recoveryMap.
     */
    fvRecoveryMap fvMap;

    if (m_recoveryMap.count(key))
    {
        fvMap = m_recoveryMap[key];
    }

    /*
     * Let's now deal with transient best-path selections, which is only required
     * when we are receiving new/refreshed state from north-bound apps (CLEAN
     * flag).
     */
    if (state == CLEAN)
    {
        adjustRecoveryMap(fvMap, fvVector, key);
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

    m_recoveryMap[key] = fvMap;
}


/*
 * Method takes care of marking eliminated entries (e.g. route paths) within the
 * recoveryMap buffer.
 */
void WarmStartHelper::removeRecoveryMap(const KeyOpFieldsValuesTuple &kfv,
                                        fvState_t                     state)
{
    fvRecoveryMap fvMap;

    /*
     * Notice that there's no point in processing bgp-withdrawal if an associated
     * entry doesn't exist in the recoveryMap.
     */
    std::string key = kfvKey(kfv);
    if (!m_recoveryMap.count(key))
    {
        return;
    }

    fvMap = m_recoveryMap[key];

    /*
     * Iterate through all elements in the map and update the state of the
     * entries being withdrawwn (i.e. 'paths' in routing case) with the proper
     * flag.
     */
    for (auto &fv : fvMap)
    {
        fv.second = state;
    }

    m_recoveryMap[key] = fvMap;
}


/*
 * This method is currently required to deal with a specific limitation of quagga
 * and frr routing-stacks, which causes transient best-path selections to arrive
 * at fpmSyncd during bgp's initial peering establishments. In these scenarios we
 * must identify the 'transient' character of a routing-update and eliminate it
 * from the recoveryMap whenever a better one is received.
 *
 * As this issue is only observed when interacting with the routing-stack, we can
 * safely avoid this call when collecting state from AppDB (restoration phase);
 * hence caller should invoke this method only if/when the state of the new entry
 * to add is set to CLEAN.
 */
void WarmStartHelper::adjustRecoveryMap(fvRecoveryMap             &fvMap,
                                        const fieldValuesTupleVoV &fvVector,
                                        const std::string         &key)
{
    /*
     * Iterate through all field-value entries in the fvMap and determine if there's
     * matching entry in the fvVector. If that's not the case, and this entry has
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
 * - An element in the recoveryMap with all its entries in the fvRecMap showing
 *   as STALE, will be eliminated from AppDB.
 *
 * - An element in the recoveryMap with all its entries in the fvRecMap showing
 *   as CLEAN, will have a NO-OP associated with it -- no changes in AppDB.
 *
 * - An element in the recoveryMap with all its entries in the fvRecMap showing
 *   as NEW, will correspond to a brand-new state, and as such, will be pushed to
 *   AppDB.
 *
 * - An element in the recoveryMap with some of its entries in the fvRecMap
 *   showing as CLEAN, will have these CLEAN entries, along with any NEW one,
 *   being pushed down to AppDB.
 *
 * - An element in the recoveryMap with some of its entries in the fvRecMap
 *   showing as NEW, will have these new entries, along with any CLEAN one,
 *   being pushed to AppDB.
 *
 * - An element in the recoveryMap with some/all of its entries in the
 *   fvRecMap showing as DELETE, will have these entries being eliminated
 *   from AppDB.
 *
 */
void WarmStartHelper::reconciliate(void)
{
    SWSS_LOG_NOTICE("Initiating AppDB reconciliation process...");

    assert(getState() == WarmStart::RESTORED);

    /*
     * Iterate through all the entries in the recoveryMap and take note of the
     * attributes associated to each.
     */
    auto it = m_recoveryMap.begin();
    while (it != m_recoveryMap.end())
    {
        std::string key = it->first;
        fvRecoveryMap fvMap = it->second;

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
            SWSS_LOG_NOTICE("Route reconciliation: deleting stale prefix %s\n",
                            key.c_str());
        }
        else if (cleanRecElems == totalRecElems)
        {
            SWSS_LOG_NOTICE("Route reconciliation: no changes needed for existing"
                            " prefix %s\n", key.c_str());
        }
        else if (newRecElems == totalRecElems)
        {
            m_syncTable->set(key, fvVector);
            SWSS_LOG_NOTICE("Route reconciliation: creating new prefix %s\n",
                            key.c_str());
        }
        else if (cleanRecElems)
        {
            m_syncTable->set(key, fvVector);
            SWSS_LOG_NOTICE("Route reconciliation: updating attributes for prefix"
                            " %s\n", key.c_str());
        }
        else if (newRecElems)
        {
            m_syncTable->set(key, fvVector);
            SWSS_LOG_NOTICE("Route reconciliation: creating new attributes for "
                            "prefix %s\n", key.c_str());
        }
        else if (deleteRecElems)
        {
            m_syncTable->del(key);
            SWSS_LOG_NOTICE("Route reconciliation: deleting withdrawn prefix %s\n",
                            key.c_str());
        }

        it = m_recoveryMap.erase(it);
    }

    /* Recovery map should be entirely empty by now */
    assert(m_recoveryMap.size() == 0);

    setState(WarmStart::RECONCILED);
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
