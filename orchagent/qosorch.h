#pragma once

#include "portsorch.h"
#include "orch.h"
#include <map>

namespace swss {
class QosOrch : public Orch
{
public:
    
    QosOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *portsOrch) :
        Orch(db, tableNames), m_portsOrch(portsOrch) {};
private:
    virtual void doTask(Consumer& consumer_info);
    PortsOrch *m_portsOrch;
};

}

