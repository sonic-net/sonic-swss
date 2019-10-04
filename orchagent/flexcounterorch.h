#ifndef FLEXCOUNTER_ORCH_H
#define FLEXCOUNTER_ORCH_H

#include "orch.h"
#include "port.h"
#include "producertable.h"

extern "C" {
#include "sai.h"
}

class FlexCounterOrch: public Orch
{
public:
    void doTask(Consumer &consumer);
    FlexCounterOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    virtual ~FlexCounterOrch(void);
 
private:
    std::shared_ptr<swss::DBConnector> m_flexCounterDb = nullptr;
    std::shared_ptr<swss::ProducerTable> m_flexCounterGroupTable = nullptr;
};

#endif
