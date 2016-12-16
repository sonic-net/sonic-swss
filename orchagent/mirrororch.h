#ifndef SWSS_MIRRORORCH_H
#define SWSS_MIRRORORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"
#include "neighorch.h"
#include "routeorch.h"
#include "fdborch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"

#include "producerstatetable.h"

#include <map>
#include <inttypes.h>

/*
 * Contains session data specified by user in config file
 * and data required for MAC address and port resolution
 * */
struct MirrorEntry
{
    bool status;
    IpAddress srcIp;
    IpAddress dstIp;
    uint16_t greType;
    uint8_t dscp;
    uint8_t ttl;
    uint8_t queue;

    struct
    {
        bool resolved;
        IpAddress nexthop;
        IpPrefix prefix;
    } nexthopInfo;

    struct
    {
        bool resolved;
        NeighborEntry neighbor;
        MacAddress mac;
        Port port;
        sai_vlan_id_t vlanId;
        sai_object_id_t portId;
    } neighborInfo;

    sai_object_id_t sessionId;
    int64_t refCount;

    MirrorEntry() :
        status(false),
        greType(0),
        dscp(0),
        ttl(0),
        queue(0),
        sessionId(0),
        refCount(0)
    {
        nexthopInfo.resolved = false;
        neighborInfo.resolved = false;
    }
};

struct MirrorSessionUpdate
{
    string name;
    bool active;
};

/* MirrorTable: mirror session name, mirror session data */
typedef map<string, MirrorEntry> MirrorTable;

class MirrorOrch : public Orch, public Observer, public Subject
{
public:
    MirrorOrch(DBConnector *db, string tableName,
               PortsOrch *portOrch, RouteOrch *routeOrch, NeighOrch *neighOrch, FdbOrch *fdbOrch);

    void update(SubjectType, void *);
    bool sessionExists(const string&);
    bool getSessionState(const string&, bool&);
    bool getSessionOid(const string&, sai_object_id_t&);
    bool increaseRefCount(const string&);
    bool decreaseRefCount(const string&);

private:
    PortsOrch *m_portsOrch;
    RouteOrch *m_routeOrch;
    NeighOrch *m_neighOrch;
    FdbOrch *m_fdbOrch;

    ProducerStateTable m_mirrorTableProducer;

    MirrorTable m_syncdMirrors;

    void createEntry(const string&, const vector<FieldValueTuple>&);
    void deleteEntry(const string&);

    bool activateSession(const string&, MirrorEntry&);
    bool deactivateSession(const string&, MirrorEntry&);
    bool updateSessionDstMac(const string&, MirrorEntry&);
    bool updateSessionDstPort(const string&, MirrorEntry&);
    bool setSessionState(const string&, MirrorEntry&);
    bool getNeighborInfo(const string&, MirrorEntry&);
    bool getNeighborInfo(const string&, MirrorEntry&, const NeighborEntry&, const MacAddress&);

    void updateNextHop(const NextHopUpdate&);
    void updateNeighbor(const NeighborUpdate&);
    void updateFdb(const FdbUpdate&);
    void updateLagMember(const LagMemberUpdate&);
    void updateVlanMember(const VlanMemberUpdate&);

    void doTask(Consumer& consumer);
};

#endif /* SWSS_MIRRORORCH_H */
