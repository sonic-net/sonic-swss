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
    FlexCounterOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~FlexCounterOrch(void);
 
private:
    shared_ptr<DBConnector> m_flexCounterDb = nullptr;
    shared_ptr<ProducerTable> m_flexCounterGroupTable = nullptr;
};

#endif
