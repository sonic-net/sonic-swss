#ifndef FLEXCOUNTER_ORCH_H
#define FLEXCOUNTER_ORCH_H

#include "orch.h"
#include "port.h"
#include "producertable.h"
#include "table.h"

extern "C" {
#include "sai.h"
}

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
    FlexCounterOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    virtual ~FlexCounterOrch(void);
    bool getPortCountersState() const;
    bool getPortBufferDropCountersState() const;
    bool getPgWatermarkCountersState() const;
    bool getQueueCountersState() const;
    map<string, FlexCounterQueueStates> getQueueConfigurations();
    map<string, FlexCounterPgStates> getPgConfigurations();
    bool getHostIfTrapCounterState() const {return m_hostif_trap_counter_enabled;}
    bool getRouteFlowCountersState() const {return m_route_flow_counter_enabled;}
    bool bake() override;

private:
    std::shared_ptr<swss::DBConnector> m_flexCounterDb = nullptr;
    std::shared_ptr<swss::ProducerTable> m_flexCounterGroupTable = nullptr;
    bool m_port_counter_enabled = false;
    bool m_port_buffer_drop_counter_enabled = false;
    bool m_pg_watermark_enabled = false;
    bool m_queue_enabled = false;
    bool m_hostif_trap_counter_enabled = false;
    bool m_route_flow_counter_enabled = false;
    Table m_flexCounterConfigTable;
    Table m_bufferQueueConfigTable;
    Table m_bufferPgConfigTable;
};

#endif
