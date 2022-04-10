#ifndef SWSS_MIRRORORCH_H
#define SWSS_MIRRORORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"
#include "neighorch.h"
#include "routeorch.h"
#include "fdborch.h"
#include "policerorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"

#include "table.h"

#include <map>
#include <inttypes.h>

#define MIRROR_RX_DIRECTION      "RX"
#define MIRROR_TX_DIRECTION      "TX"
#define MIRROR_BOTH_DIRECTION    "BOTH"
#define MIRROR_SESSION_SPAN      "SPAN"
#define MIRROR_SESSION_ERSPAN    "ERSPAN"

/*
 * Contains session data specified by user in config file
 * and data required for MAC address and port resolution
 */
struct MirrorEntry
{
    bool status;
    IpAddress srcIp;
    IpAddress dstIp;
    uint16_t greType;
    uint8_t dscp;
    uint8_t ttl;
    uint8_t queue;
    string policer;
    string dst_port;
    string src_port;
    string direction;
    string type;

    struct
    {
        IpPrefix prefix;
        NextHopKey nexthop;
    } nexthopInfo;

    struct
    {
        NeighborEntry neighbor;
        MacAddress mac;
        Port port;
        sai_object_id_t portId;
    } neighborInfo;

    sai_object_id_t sessionId;

    int64_t refCount;

    MirrorEntry(const string& platform);
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
    MirrorOrch(TableConnector appDbConnector, TableConnector confDbConnector,
               PortsOrch *portOrch, RouteOrch *routeOrch, NeighOrch *neighOrch, FdbOrch *fdbOrch, PolicerOrch *policerOrch);

    bool bake() override;
    void update(SubjectType, void *);
    bool sessionExists(const string&);
    bool getSessionStatus(const string&, bool&);
    bool getSessionOid(const string&, sai_object_id_t&);
    bool increaseRefCount(const string&);
    bool decreaseRefCount(const string&);

    using Orch::doTask;  // Allow access to the basic doTask

private:
    PortsOrch *m_portsOrch;
    RouteOrch *m_routeOrch;
    NeighOrch *m_neighOrch;
    FdbOrch *m_fdbOrch;
    PolicerOrch *m_policerOrch;
    // Maximum number of traffic classes starting at 0, thus queue can be 0 - m_maxNumTC-1
    uint8_t m_maxNumTC;

    Table m_mirrorTable;

    MirrorTable m_syncdMirrors;
    // session_name -> VLAN | monitor_port_alias | next_hop_ip
    map<string, string> m_recoverySessionMap;

    bool isHwResourcesAvailable();

    task_process_status createEntry(const string&, const vector<FieldValueTuple>&);
    task_process_status deleteEntry(const string&);

    bool activateSession(const string&, MirrorEntry&);
    bool deactivateSession(const string&, MirrorEntry&);
    bool updateSession(const string&, MirrorEntry&);
    bool updateSessionDstMac(const string&, MirrorEntry&);
    bool updateSessionDstPort(const string&, MirrorEntry&);
    bool updateSessionType(const string&, MirrorEntry&);

    /*
     * Store mirror session state in StateDB
     * attr is the field name will be stored, if empty then all fields will be stored
     */
    void setSessionState(const std::string& name, const MirrorEntry& session, const std::string& attr = "");
    void removeSessionState(const std::string& name);

    bool getNeighborInfo(const string&, MirrorEntry&);

    void updateNextHop(const NextHopUpdate&);
    void updateNeighbor(const NeighborUpdate&);
    void updateFdb(const FdbUpdate&);
    void updateLagMember(const LagMemberUpdate&);
    void updateVlanMember(const VlanMemberUpdate&);

    bool checkPortExistsInSrcPortList(const string& port, const string& srcPortList);
    bool validateSrcPortList(const string& srcPort);
    bool validateDstPort(const string& dstPort);
    bool setUnsetPortMirror(Port port, bool ingress, bool set,
                                    sai_object_id_t sessionId);
    bool configurePortMirrorSession(const string&, MirrorEntry&, bool enable);

    void doTask(Consumer& consumer);
};

#endif /* SWSS_MIRRORORCH_H */
