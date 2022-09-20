#ifndef FLEXCOUNTER_ORCH_H
#define FLEXCOUNTER_ORCH_H

#include "orch.h"
#include "port.h"
#include "producertable.h"
#include "table.h"

extern "C" {
#include "sai.h"
}

class FlexCounterOrch: public Orch
{
public:
    void doTask(Consumer &consumer);
    FlexCounterOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    virtual ~FlexCounterOrch(void);
    bool getPortCountersState() const;
    bool getPortBufferDropCountersState() const;
    bool getHostIfTrapCounterState() const {return m_hostif_trap_counter_enabled;}
    bool getRouteFlowCountersState() const {return m_route_flow_counter_enabled;}
    bool bake() override;

private:
    std::shared_ptr<swss::DBConnector> m_flexCounterDb = nullptr;
    std::shared_ptr<swss::ProducerTable> m_flexCounterGroupTable = nullptr;
    std::shared_ptr<swss::DBConnector> m_gbflexCounterDb = nullptr;
    shared_ptr<ProducerTable> m_gbflexCounterGroupTable = nullptr;
    bool m_port_counter_enabled = false;
    bool m_port_buffer_drop_counter_enabled = false;
    bool m_hostif_trap_counter_enabled = false;
    bool m_route_flow_counter_enabled = false;
    Table m_flexCounterConfigTable;
};

#endif
