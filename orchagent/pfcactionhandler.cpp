#include "pfcactionhandler.h"
#include "logger.h"
#include "saiserialize.h"

#include <vector>

#define PFC_WD_QUEUE_STATUS             "PFC_WD_STATUS"
#define PFC_WD_QUEUE_STATUS_OPERATIONAL "operational"
#define PFC_WD_QUEUE_STATUS_STORMED     "stormed"

#define PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED"
#define PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED"

#define SAI_QUEUE_STAT_PACKETS_STR         "SAI_QUEUE_STAT_PACKETS"
#define SAI_QUEUE_STAT_DROPPED_PACKETS_STR "SAI_QUEUE_STAT_DROPPED_PACKETS"

extern sai_object_id_t gSwitchId;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_buffer_api_t *sai_buffer_api;

PfcWdActionHandler::PfcWdActionHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    m_port(port),
    m_queue(queue),
    m_queueId(queueId),
    m_countersTable(countersTable)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE(
            "PFC Watchdog detected PFC storm on queue 0x%lx port 0x%lx",
            m_queue,
            m_port);
    
    m_stats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));
    m_stats.detectCount++;
    m_stats.operational = false;

    updateWdCounters(sai_serialize_object_id(m_queue), m_stats);
}

PfcWdActionHandler::~PfcWdActionHandler(void)
{
    SWSS_LOG_ENTER();

    auto finalStats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));

    SWSS_LOG_NOTICE(
            "Queue 0x%lx port 0x%lx restored from PFC storm. Tx packets: %lu. Dropped packets: %lu",
            m_queue,
            m_port,
            finalStats.txPkt - m_stats.txPkt,
            finalStats.txDropPkt - m_stats.txDropPkt);

    finalStats.restoreCount++;
    finalStats.operational = true;

    updateWdCounters(sai_serialize_object_id(m_queue), finalStats);
}

PfcWdActionHandler::PfcWdQueueStats PfcWdActionHandler::getQueueStats(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    PfcWdQueueStats stats;
    vector<FieldValueTuple> fieldValues;

    if (!countersTable->get(queueIdStr, fieldValues))
    {
        return move(stats);
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED)
        {
            stats.detectCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED)
        {
            stats.restoreCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATUS)
        {
            stats.operational = value == PFC_WD_QUEUE_STATUS_OPERATIONAL ? true : false;
        }
        else if (field == SAI_QUEUE_STAT_PACKETS_STR)
        {
            stats.txPkt = stoul(value);
        }
        else if (field == SAI_QUEUE_STAT_DROPPED_PACKETS_STR)
        {
            stats.txDropPkt = stoul(value);
        }
    }

    return move(stats);
}

void PfcWdActionHandler::initWdCounters(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    auto stats = getQueueStats(countersTable, queueIdStr);

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, PFC_WD_QUEUE_STATUS_OPERATIONAL);

    countersTable->set(queueIdStr, resultFvValues);
}

void PfcWdActionHandler::updateWdCounters(const string& queueIdStr, const PfcWdQueueStats& stats)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, stats.operational ?
                                                     PFC_WD_QUEUE_STATUS_OPERATIONAL :
                                                     PFC_WD_QUEUE_STATUS_STORMED);

    m_countersTable->set(queueIdStr, resultFvValues);
}

PfcWdLossyHandler::PfcWdLossyHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdActionHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;

    sai_status_t status = sai_port_api->get_port_attribute(port, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%lx: %d", port, status);
    }

    uint8_t pfcMask = attr.value.u8;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;
    attr.value.u8 = pfcMask & ~(1 << queueId);

    status = sai_port_api->set_port_attribute(port, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%lx: %d", port, status);
    }
}

PfcWdLossyHandler::~PfcWdLossyHandler(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;

    sai_status_t status = sai_port_api->get_port_attribute(getPort(), 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%lx: %d", getPort(), status);
        return;
    }

    uint8_t pfcMask = attr.value.u8;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;
    attr.value.u8 = pfcMask | (1 << getQueueId());

    status = sai_port_api->set_port_attribute(getPort(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC mask on port 0x%lx: %d", getPort(), status);
        return;
    }
}

PfcWdZeroBufferHandler::PfcWdZeroBufferHandler(sai_object_id_t port,
        sai_object_id_t queue, uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdLossyHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;

    // Get queue's buffer profile ID
    sai_status_t status = sai_queue_api->get_queue_attribute(queue, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get buffer profile ID on queue 0x%lx: %d", queue, status);
        return;
    }

    sai_object_id_t oldProfileId = attr.value.oid;
#if 0
    attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
    attr.value.u32 = 0;

    // Get threshold mode of buffer profile
    status = sai_buffer_api->get_buffer_profile_attribute(oldProfileId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get buffer profile threshold mode for 0x%lx: %d", queue, status);
        return;
    }

    sai_buffer_profile_threshold_mode_t threshold_mode = static_cast<sai_buffer_profile_threshold_mode_t>(attr.value.u32);
#endif

    // XXX: Remove when above is fixed
    sai_buffer_profile_threshold_mode_t threshold_mode = SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC;

    sai_object_id_t zeroBufferProfileId = SAI_NULL_OBJECT_ID;
    if (threshold_mode == SAI_BUFFER_PROFILE_THRESHOLD_MODE_STATIC)
    {
        zeroBufferProfileId = ZeroBufferProfile::getStaticProfile();
    }
    else if (threshold_mode == SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC)
    {
        zeroBufferProfileId = ZeroBufferProfile::getDynamicProfile();
    }
    else
    {
        SWSS_LOG_ERROR("Buffer mode in buffer 0x%lx is not supported", queue);
        return;
    }

    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = zeroBufferProfileId;

    // Set our zero buffer profile
    status = sai_queue_api->set_queue_attribute(queue, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%lx: %d", queue, status);
        return;
    }

    // Save original buffer profile
    m_originalBufferProfile = oldProfileId;
}

PfcWdZeroBufferHandler::~PfcWdZeroBufferHandler(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = m_originalBufferProfile;

    // Set our zero buffer profile
    sai_status_t status = sai_queue_api->set_queue_attribute(getQueue(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%lx: %d", getQueue(), status);
        return;
    }
}

PfcWdZeroBufferHandler::ZeroBufferProfile::ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();
}

PfcWdZeroBufferHandler::ZeroBufferProfile::~ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();

    destroyStaticProfile();
    destroyDynamicProfile();
}

PfcWdZeroBufferHandler::ZeroBufferProfile &PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance(void)
{
    SWSS_LOG_ENTER();

    static ZeroBufferProfile instance;

    return instance;
}

sai_object_id_t PfcWdZeroBufferHandler::ZeroBufferProfile::getStaticProfile(void)
{
    SWSS_LOG_ENTER();

    if (getInstance().m_zeroStaticBufferProfile == SAI_NULL_OBJECT_ID)
    {
        getInstance().createStaticProfile();
    }

    return getInstance().m_zeroStaticBufferProfile;
}

sai_object_id_t PfcWdZeroBufferHandler::ZeroBufferProfile::getDynamicProfile(void)
{
    SWSS_LOG_ENTER();

    if (getInstance().m_zeroDynamicBufferProfile == SAI_NULL_OBJECT_ID)
    {
        getInstance().createDynamicProfile();
    }

    return getInstance().m_zeroDynamicBufferProfile;
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::createStaticProfile(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attribs;

    // Create static zero pool
    attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
    attr.value.u32 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_TYPE;
    attr.value.u32 = SAI_BUFFER_POOL_TYPE_EGRESS;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_POOL_THRESHOLD_MODE_STATIC;
    attribs.push_back(attr);

    sai_status_t status = sai_buffer_api->create_buffer_pool(
            &m_zeroStaticBufferPool,
            gSwitchId,
            attribs.size(),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create static zero buffer pool for PFC WD: %d", status);
        return;
    }

    // Create static zero profile
    attribs.clear();

    attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
    attr.value.oid = m_zeroStaticBufferPool;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
    attr.value.u32 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_STATIC_TH;
    attr.value.u32 = 0;
    attribs.push_back(attr);

    status = sai_buffer_api->create_buffer_profile(
            &m_zeroStaticBufferProfile,
            gSwitchId,
            attribs.size(),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create static zero buffer profile for PFC WD: %d", status);
        return;
    }
}


void PfcWdZeroBufferHandler::ZeroBufferProfile::createDynamicProfile(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attribs;

    // Create dynamic zero pool
    attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
    attr.value.u32 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_TYPE;
    attr.value.u32 = SAI_BUFFER_POOL_TYPE_EGRESS;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_POOL_THRESHOLD_MODE_DYNAMIC;
    attribs.push_back(attr);

    sai_status_t status = sai_buffer_api->create_buffer_pool(
            &m_zeroDynamicBufferPool,
            gSwitchId,
            attribs.size(),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer pool for PFC WD: %d", status);
        return;
    }

    // Create dynamic zero profile
    attribs.clear();

    attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
    attr.value.oid = m_zeroDynamicBufferPool;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
    attr.value.u32 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
    attr.value.u32 = 1;
    attribs.push_back(attr);

    status = sai_buffer_api->create_buffer_profile(
            &m_zeroDynamicBufferProfile,
            gSwitchId,
            attribs.size(),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer profile for PFC WD: %d", status);
        return;
    }
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::destroyStaticProfile(void)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_buffer_api->remove_buffer_profile(m_zeroStaticBufferProfile);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer profile for PFC WD: %d", status);
        return;
    }

    status = sai_buffer_api->remove_buffer_pool(m_zeroStaticBufferPool);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer pool for PFC WD: %d", status);
    }
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::destroyDynamicProfile(void)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_buffer_api->remove_buffer_profile(m_zeroDynamicBufferProfile);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove dynamic zero buffer profile for PFC WD: %d", status);
        return;
    }

    status = sai_buffer_api->remove_buffer_pool(m_zeroDynamicBufferPool);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove dynamic zero buffer pool for PFC WD: %d", status);
    }
}
