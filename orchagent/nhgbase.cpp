#include "nhgbase.h"
#include "vector"
#include "rediscommand.h"

extern sai_object_id_t gSwitchId;

unsigned NhgBase::m_syncedCount = 0;

/*
 * Purpose: Destructor.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
NhgBase::~NhgBase()
{
    SWSS_LOG_ENTER();

    if (isSynced())
    {
        SWSS_LOG_ERROR("Destroying next hop group with SAI ID %lu which is still synced.", m_id);
        assert(false);
    }
}

/*
 * Purpose: Decrease the count of synced next hop group objects.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
void NhgBase::decSyncedCount()
{
    SWSS_LOG_ENTER();

    if (m_syncedCount == 0)
    {
        SWSS_LOG_ERROR("Decreasing next hop groups count while already 0");
        throw logic_error("Decreasing next hop groups count while already 0");
    }

    --m_syncedCount;
}
