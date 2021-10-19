#include "nhgorch.h"

extern PortsOrch *gPortsOrch;

NhgOrch::NhgOrch(DBConnector *db, const vector<string> &table_names) :
    Orch(db, table_names)
{
    SWSS_LOG_ENTER();
}

/*
 * Purpose:     Perform the operations requested by APPL_DB users.
 * Description: Iterate over the untreated operations list and resolve them.
 *              The operations supported are SET and DEL.  If an operation
 *              could not be resolved, it will either remain in the list, or be
 *              removed, depending on the case.
 * Params:      IN  consumer - The cosumer object.
 * Returns:     Nothing.
 */
void NhgOrch::doTask(Consumer& consumer)
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

/*
 * Purpose:     Increase the ref count for a next hop group.
 * Description: Increment the ref count for a next hop group by 1.
 * Params:      IN  index - The index of the next hop group.
 * Returns:     Nothing.
 */
void NhgOrch::incNhgRefCount(const std::string& index)
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
 * Purpose:     Decrease the ref count for a next hop group.
 * Description: Decrement the ref count for a next hop group by 1.
 * Params:      IN  index - The index of the next hop group.
 * Returns:     Nothing.
 */
void NhgOrch::decNhgRefCount(const std::string& index)
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