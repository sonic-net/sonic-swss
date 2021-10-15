#pragma once

#include "cbfnhghandler.h"
#include "nhghandler.h"
#include "switchorch.h"
#include "vector"
#include "portsorch.h"

using namespace std;

extern PortsOrch *gPortsOrch;

class NhgOrch : public Orch
{
public:
    NhgOrch(DBConnector *db, const vector<string> &table_names) :
        Orch(db, table_names)
    {
        SWSS_LOG_ENTER();
    }

    /*
     * Get the maximum number of ECMP groups allowed by the switch.
     */
    static inline unsigned getMaxNhgCount()
                                    { SWSS_LOG_ENTER(); return m_maxNhgCount; }

    /*
     * Get the number of next hop groups that are synced.
     */
    static inline unsigned getSyncedNhgCount()
                        { SWSS_LOG_ENTER(); return NhgBase::getSyncedCount(); }

    /* Increase the number of synced next hop groups. */
    static void incSyncedNhgCount()
    {
        SWSS_LOG_ENTER();

        if (getSyncedNhgCount() >= m_maxNhgCount)
        {
            SWSS_LOG_ERROR("Incresing synced next hop group count beyond "
                            "switch's capabilities");
            throw logic_error("Next hop groups exceed switch's "
                                    "capabilities");
        }

        NhgBase::incSyncedCount();
    }

    /* Decrease the number of next hop groups. */
    static inline void decSyncedNhgCount()
                            { SWSS_LOG_ENTER(); NhgBase::decSyncedCount(); }

    /*
     * Check if the next hop group with the given index exists.
     */
    inline bool hasNhg(const string &index) const
    {
        SWSS_LOG_ENTER();
        return nhgHandler.hasNhg(index) || cbfNhgHandler.hasNhg(index);
    }

    /*
     * Get the next hop group with the given index.
     */
    const NhgBase& getNhg(const string &index) const
    {
        SWSS_LOG_ENTER();

        try
        {
            return nhgHandler.getNhg(index);
        }
        catch(const std::out_of_range &e)
        {
            return cbfNhgHandler.getNhg(index);
        }
    }

    /*
     * Increase the reference counter for the next hop group with the given
     * index.
     */
    void incNhgRefCount(const string &index)
    {
        SWSS_LOG_ENTER();

        if (nhgHandler.hasNhg(index))
        {
            nhgHandler.incNhgRefCount(index);
        }
        else
        {
            cbfNhgHandler.incNhgRefCount(index);
        }
    }

    /*
     * Decrease the reference counter for the next hop group with the given
     * index.
     */
    void decNhgRefCount(const string &index)
    {
        SWSS_LOG_ENTER();

        if (nhgHandler.hasNhg(index))
        {
            nhgHandler.decNhgRefCount(index);
        }
        else
        {
            cbfNhgHandler.decNhgRefCount(index);
        }
    }

    void doTask(Consumer &consumer) override
    {
        SWSS_LOG_ENTER();

        if (!gPortsOrch->allPortsReady())
        {
            return;
        }

        string table_name = consumer.getTableName();

        if (table_name == APP_NEXTHOP_GROUP_TABLE_NAME)
        {
            nhgHandler.doTask(consumer);
        }
        else if (table_name == APP_CLASS_BASED_NEXT_HOP_GROUP_TABLE_NAME)
        {
            cbfNhgHandler.doTask(consumer);
        }
    }

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

    /*
     * Switch's maximum number of next hop groups capacity.
     */
    static unsigned m_maxNhgCount;
};
