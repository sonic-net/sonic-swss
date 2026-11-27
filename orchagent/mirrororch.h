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
#include <set>
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
    bool sourceConfigured = false;
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
        sai_object_id_t portId = SAI_NULL_OBJECT_ID;
    } neighborInfo;

    struct Destination
    {
        int vlan = 0;
        MacAddress mac;
        sai_object_id_t portId = SAI_NULL_OBJECT_ID;
    } programmedDestination;
    Destination resolvedDestination() const
    {
        return {neighborInfo.port.m_type == Port::VLAN ? neighborInfo.port.m_vlan_info.vlan_id : 0,
                neighborInfo.mac, neighborInfo.portId};
    }

    sai_object_id_t waitingFdbVlan = SAI_NULL_OBJECT_ID;
    MacAddress waitingFdbMac;

    sai_object_id_t sessionId;

    int64_t refCount;

    MirrorEntry(const string& platform);
    bool isSampleExport() const { return type == MIRROR_SESSION_ERSPAN && !sourceConfigured && src_port.empty(); }
    bool isActiveSampleExport() const
    {
        return status && sessionId != SAI_NULL_OBJECT_ID && isSampleExport();
    }
};

struct MirrorSessionUpdate
{
    string name;
    bool active;
    bool recoveredInPlace = false;
};

/* MirrorTable: mirror session name, mirror session data */
typedef map<string, MirrorEntry> MirrorTable;

class MirrorOrch : public Orch, public Observer, public Subject
{
public:
    MirrorOrch(TableConnector appDbConnector, TableConnector confDbConnector,
               PortsOrch *portOrch, RouteOrch *routeOrch, NeighOrch *neighOrch, FdbOrch *fdbOrch, PolicerOrch *policerOrch, SwitchOrch *switchOrch);

    bool bake() override;
    void onWarmBootEnd() override;
    void update(SubjectType, void *);
    bool sessionExists(const string&);
    bool getSessionStatus(const string&, bool&);
    bool getSessionOid(const string&, sai_object_id_t&);
    bool increaseRefCount(const string&);
    bool decreaseRefCount(const string&);

    bool claimSflowMode();

    using Orch::doTask;
    void doTask() override;

private:
    PortsOrch *m_portsOrch;
    RouteOrch *m_routeOrch;
    NeighOrch *m_neighOrch;
    FdbOrch *m_fdbOrch;
    PolicerOrch *m_policerOrch;
    SwitchOrch *m_switchOrch;
    // Maximum number of traffic classes starting at 0, thus queue can be 0 - m_maxNumTC-1
    uint8_t m_maxNumTC;
    bool m_isWarmRestoreStage = false;

    Table m_mirrorTable;

    MirrorTable m_syncdMirrors;
    // session_name -> VLAN | monitor_port_alias | next_hop_ip
    map<string, string> m_recoverySessionMap;

    bool isHwResourcesAvailable();

    task_process_status createEntry(const string&, const vector<FieldValueTuple>&);
    task_process_status deleteEntry(const string&, bool replacingForNewBinding = false);

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
    bool setUnsetPortMirror(Port port, bool ingress, bool set, sai_object_id_t sessionId);
    bool configurePortMirrorSession(const string&, MirrorEntry&, bool enable);

    struct SampleAttachment
    {
        string alias;
        bool sampleSet = false;
        bool mirrorSet = false;
    };
    struct SampleBinding
    {
        string port;
        string session;
        bool ingress = true;
        bool enabled = false;
        bool deleting = false;
        bool cleanupPending = false;
        bool programmingRejected = false;
        bool unsupportedLogged = false;
        uint32_t rate = 0;
        uint32_t truncate = 0;
        sai_object_id_t samplepacketId = SAI_NULL_OBJECT_ID;
        map<sai_object_id_t, SampleAttachment> attachments;
    };
    Table m_sampleConfigTable;
    swss::DBConnector& m_configDb;
    enum class SamplingMode { NONE, SFLOW, SAMPLED_MIRROR, CONFLICT };
    SamplingMode m_samplingMode = SamplingMode::NONE;
    SamplingMode configuredSamplingMode() const;
    bool claimSamplingMode(SamplingMode requested);
    bool hasSampleBindingForSession(const string& name, bool includeConfig = true);
    bool hasConfiguredSampleBinding(const string& name, bool onlyNew = false);
    bool hasQueuedReplacement(const string& table, const string& key);
    map<string, SampleBinding> m_sampleBindings;
    std::set<string> m_sampleRetries;
    enum class ExportDeactivation { DETACH_BINDINGS, REMOVE_SESSION };
    map<string, ExportDeactivation> m_pendingExportDeactivations;
    bool isRemovingExport(const string& name) const;
    map<std::pair<sai_object_id_t, bool>, string> m_sampleOwners;
    using SamplePorts = map<sai_object_id_t, string>;
    SamplePorts resolveSamplePorts(const string& alias) const;
    bool samplePortsAvailable(const string& key, const SampleBinding& binding, const SamplePorts& ports) const;
    sai_status_t attachSamplePort(const string& key, SampleBinding& binding, sai_object_id_t oid, const string& alias,
                                  sai_object_id_t sessionId);
    bool reconcileSampleBinding(const string& key, SampleBinding& binding);
    void syncSampleBinding(const string& key, SampleBinding& binding);
    task_process_status setSampleBinding(const string&, const vector<FieldValueTuple>&);
    sai_status_t createSamplePacket(const string&, SampleBinding&);
    bool clearSampleAttachment(const string&, SampleBinding&, sai_object_id_t);
    bool clearSampleBinding(const string&, SampleBinding&);
    void syncSampleBindings();

    void doTask(Consumer& consumer);
};

#endif /* SWSS_MIRRORORCH_H */
