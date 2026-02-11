#ifndef FLEXCOUNTER_ORCH_H
#define FLEXCOUNTER_ORCH_H

#include "orch.h"
#include "port.h"
#include "producertable.h"
#include "selectabletimer.h"
#include "table.h"

extern "C" {
#include "sai.h"
}

// Delay in seconds before flex counter processing begins after orchagent startup.
// 
// This delay improves boot time by prioritizing data plane configuration over
// counter initialization. Systems with many ports, priority groups (PGs), and
// queues require significant time to generate counter maps, which is not
// immediately necessary during boot.
// Value of 0 will process flex counters immediately.
// 
// Configured via orchagent command line argument: -D <delay_sec>
// 
extern int gFlexCounterDelaySec;

const std::string createAllAvailableBuffersStr = "create_all_available_buffers";

class FlexCounterQueueStates
{
public:
    FlexCounterQueueStates(uint32_t maxQueueNumber);
    bool isQueueCounterEnabled(uint32_t index) const;
    void enableQueueCounters(uint32_t startIndex, uint32_t endIndex);
    void enableQueueCounter(uint32_t queueIndex);

private:
    std::vector<bool> m_queueStates{};
};

class FlexCounterPgStates
{
public:
    FlexCounterPgStates(uint32_t maxPgNumber);
    bool isPgCounterEnabled(uint32_t index) const;
    void enablePgCounters(uint32_t startIndex, uint32_t endIndex);
    void enablePgCounter(uint32_t pgIndex);

private:
    std::vector<bool> m_pgStates{};
};

class FlexCounterOrch: public Orch
{
public:
    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);
    FlexCounterOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    virtual ~FlexCounterOrch(void);
    bool getPortCountersState() const;
    bool getPortPhyAttrCounterState() const;
    bool getPortBufferDropCountersState() const;
    bool getQueueCountersState() const;
    bool getQueueWatermarkCountersState() const;
    bool getPgCountersState() const;
    bool getPgWatermarkCountersState() const;
    std::map<std::string, FlexCounterQueueStates> getQueueConfigurations();
    std::map<std::string, FlexCounterPgStates> getPgConfigurations();
    bool getHostIfTrapCounterState() const {return m_hostif_trap_counter_enabled;}
    bool getRouteFlowCountersState() const {return m_route_flow_counter_enabled;}
    bool getWredQueueCountersState() const;
    bool getWredPortCountersState() const;
    bool isCreateOnlyConfigDbBuffers() const;
    bool bake() override;

private:
    void handleDeviceMetadataTable(Consumer &consumer);
    bool m_port_counter_enabled = false;
    bool m_port_phy_attr_enabled = false;
    bool m_port_buffer_drop_counter_enabled = false;
    bool m_queue_enabled = false;
    bool m_queue_watermark_enabled = false;
    bool m_pg_enabled = false;
    bool m_pg_watermark_enabled = false;
    bool m_hostif_trap_counter_enabled = false;
    bool m_route_flow_counter_enabled = false;
    bool m_delayTimerExpired = false;
    bool m_wred_queue_counter_enabled = false;
    bool m_wred_port_counter_enabled = false;
    Table m_bufferQueueConfigTable;
    Table m_bufferPgConfigTable;
    Table m_deviceMetadataConfigTable;
    SelectableTimer* m_delayTimer;
    std::unordered_set<std::string> m_groupsWithBulkChunkSize;

    bool m_createOnlyConfigDbBuffers = false;
};

#endif
