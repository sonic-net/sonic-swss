#include "orchagent/nhgbase.h"
#include "vector"
#include "rediscommand.h"

extern sai_object_id_t gSwitchId;

unsigned NhgBase::m_syncdCount = 0;

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

    /*
     * The group member should be removed from its group before destroying it.
     */
    assert(!isSynced());
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

    if (m_syncdCount == 0)
    {
        SWSS_LOG_ERROR("Decreasing next hop groups count while already 0");
        throw logic_error("Decreasing next hop groups count while already 0");
    }

    --m_syncdCount;
}
