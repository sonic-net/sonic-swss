#pragma once

#include "nhg/cbfnhghandler.h"
#include "nhg/nhghandler.h"
#include "vector"
#include "routeorch.h"

using namespace std;

extern RouteOrch *gRouteOrch;

/*
 * Next Hop Group Orchestrator class that handles NEXTHOP_GROUP_TABLE
 * and CLASS_BASED_NEXT_HOP_GROUP_TABLE updates.
 */
class NhgOrch : public Orch
{
public:
    /*
     * Constructor.
     */
    NhgOrch(DBConnector *db, const vector<string> &table_names);

    /* Check if the next hop group given by it's index exists. */
    inline bool hasNhg(const std::string& index) const
    {
        return nhgHandler.hasNhg(index) || cbfNhgHandler.hasNhg(index);
    }

    /*
     * Get the next hop group with the given index.
     */
    inline const NhgBase& getNhg(const std::string &index) const
    {
        try
        {
            return nhgHandler.getNhg(index);
        }
        catch(const std::out_of_range &e)
        {
            return cbfNhgHandler.getNhg(index);
        }
    }

    /* Getters / Setters. */
    static inline unsigned getSyncedNhgCount() { return NhgBase::getSyncedCount(); }

    /* Increase / Decrease the number of synced next hop groups. */
    inline void incSyncedNhgCount()
    {
        assert(gRouteOrch->getNhgCount() + NhgBase::getSyncedCount() < gRouteOrch->getMaxNhgCount());
        NhgBase::incSyncedCount();
    }
    inline void decSyncedNhgCount() { NhgBase::decSyncedCount(); }

    /* Increase / Decrease ref count for a NHG given by it's index. */
    void incNhgRefCount(const std::string& index);
    void decNhgRefCount(const std::string& index);

    /* Handling SAI status*/
    task_process_status handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context = nullptr)
        { return Orch::handleSaiCreateStatus(api, status, context); }
    task_process_status handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context = nullptr)
        { return Orch::handleSaiRemoveStatus(api, status, context); }
    bool parseHandleSaiStatusFailure(task_process_status status)
        { return Orch::parseHandleSaiStatusFailure(status); }

    /*
     * Handlers dealing with the (non) CBF operations.
     */
    NhgHandler nhgHandler;
    CbfNhgHandler cbfNhgHandler;
private:

    void doTask(Consumer& consumer);
};
