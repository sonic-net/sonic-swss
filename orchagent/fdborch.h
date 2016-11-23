#ifndef SWSS_FDBORCH_H
#define SWSS_FDBORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"

struct FdbEntry
{
    MacAddress mac;
    uint16_t vlan;
};

struct FdbUpdate
{
    FdbEntry entry;
    Port port;
    bool add;
};

class FdbOrch: public Subject
{
public:
    FdbOrch(PortsOrch *port) :
        m_portsOrch(port)
    {
    }

    void update(sai_fdb_event_t, const sai_fdb_entry_t *, sai_object_id_t);
    bool getPort(const MacAddress&, uint16_t, Port&);

private:
    PortsOrch *m_portsOrch;
};

#endif /* SWSS_FDBORCH_H */
