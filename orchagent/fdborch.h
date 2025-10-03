#ifndef SWSS_FDBORCH_H
#define SWSS_FDBORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"

enum FdbOrigin
{
    FDB_ORIGIN_INVALID = 0,
    FDB_ORIGIN_LEARN = 1,
    FDB_ORIGIN_PROVISIONED = 2,
    FDB_ORIGIN_VXLAN_ADVERTIZED = 4,
    FDB_ORIGIN_MCLAG_ADVERTIZED = 8
};

struct FdbEntry
{
    MacAddress mac;
    sai_object_id_t bv_id;
    std::string port_name;

    bool operator<(const FdbEntry& other) const
    {
        return tie(mac, bv_id) < tie(other.mac, other.bv_id);
    }
    bool operator==(const FdbEntry& other) const
    {
        return tie(mac, bv_id) == tie(other.mac, other.bv_id);
    }
};

struct FdbUpdate
{
    FdbEntry entry;
    Port port;
    string type;
    bool add;
    sai_fdb_entry_type_t sai_fdb_type;
};

struct FdbFlushUpdate
{
    vector<FdbEntry> entries;
    Port port;
};

enum NEXT_HOP_VALUE_TYPE {
    UNKNOWN = 0,
    VTEP = 1,
    NEXTHOPGROUP = 2,
    IFNAME = 3
};

struct FdbData
{
    sai_object_id_t bridge_port_id;
    string type;
    FdbOrigin origin;
    /**
      {"dynamic", FDB_ORIGIN_LEARN} => dynamically learnt
      {"dynamic", FDB_ORIGIN_PROVISIONED} => provisioned dynamic with swssconfig in APPDB
      {"dynamic", FDB_ORIGIN_ADVERTIZED} => synced from remote device e.g. BGP MAC route
      {"static", FDB_ORIGIN_LEARN} => Invalid
      {"static", FDB_ORIGIN_PROVISIONED} => statically provisioned
      {"static", FDB_ORIGIN_ADVERTIZED} => sticky synced from remote device
    */
    bool is_flush_pending;

    NEXT_HOP_VALUE_TYPE dest_type;
    string dest_value;
    string    esi;
    unsigned int vni;
    sai_fdb_entry_type_t sai_fdb_type;
    string discard;
    bool allow_mac_move;
};

struct SavedFdbEntry
{
    MacAddress mac;
    unsigned short vlanId;
    FdbData fdbData;
    bool operator==(const SavedFdbEntry& other) const
    {
        return tie(mac, vlanId) == tie(other.mac, other.vlanId);
    }
};

typedef unordered_map<string, vector<SavedFdbEntry>> saved_fdb_entries_by_port_t;

/*
 * With the current structure, it is not possible to directory store the FdbData
 * as the information required to key it (MAC, VLAN) is not stored within.
 * This unfortunately introduces another level of indirection when iterating all
 * the entries for a given port.
 */
typedef unordered_map<string, vector<FdbEntry>> fdb_entries_by_port_t;

class FdbOrch: public Orch, public Subject, public Observer
{
public:

    FdbOrch(DBConnector* applDbConnector, vector<table_name_with_pri_t> appFdbTables,
                TableConnector stateDbFdbConnector, TableConnector stateDbMclagFdbConnector, PortsOrch *port);

    ~FdbOrch()
    {
        m_portsOrch->detach(this);
    }

    bool bake() override;
    void update(sai_fdb_event_t, const sai_fdb_entry_t *, sai_object_id_t, const sai_fdb_entry_type_t &);
    void update(SubjectType type, void *cntx);
    bool getPort(const MacAddress&, uint16_t, Port&);

    bool is_fdb_programmed_to_vxlan_tunnel(FdbEntry& entry);
    bool removeFdbEntry(const FdbEntry& entry, FdbOrigin origin=FDB_ORIGIN_PROVISIONED);

    static const int fdborch_pri;
    void flushFDBEntries(sai_object_id_t bridge_port_oid,
                         sai_object_id_t vlan_oid);
    void flushAllFDBEntries(sai_object_id_t bridge_port_oid,
                            sai_object_id_t vlan_oid);
    void flushFdbByVlan(const string &);
    void notifyObserversFDBFlush(Port &p, sai_object_id_t&);

private:
    PortsOrch *m_portsOrch;
    map<FdbEntry, FdbData> m_entries;
    fdb_entries_by_port_t m_entries_by_port;
    saved_fdb_entries_by_port_t saved_fdb_entries;
    vector<Table*> m_appTables;
    Table m_fdbStateTable;
    Table m_mclagFdbStateTable;
    NotificationConsumer* m_flushNotificationsConsumer;
    NotificationConsumer* m_fdbNotificationConsumer;
    shared_ptr<DBConnector> m_notificationsDb;

    map<NEXT_HOP_VALUE_TYPE, string> destTypeToString =
        { { UNKNOWN, "Unknown" },
          { VTEP, "Vtep"},
          { NEXTHOPGROUP, "NexthopGroup" },
          { IFNAME, "Ifname" } };

    void doTask(Consumer& consumer);
    void doTask(NotificationConsumer& consumer);

    void updateVlanMember(const VlanMemberUpdate&);
    void updatePortOperState(const PortOperStateUpdate&);

    bool addFdbEntry(const FdbEntry&, const string&, FdbData fdbData);
    void deleteFdbEntryFromSavedFDB(const MacAddress &mac, const unsigned short &vlanId, FdbOrigin origin, const string portName="");
    void removeFdbEntryFromPortCache(const FdbEntry& entry, const Port& port);

    bool storeFdbEntryState(const FdbUpdate& update);
    void notifyTunnelOrch(Port& port);

    void clearFdbEntry(const FdbEntry&, const FdbData&);
    void handleSyncdFlushNotif(const sai_object_id_t&, const sai_object_id_t&, const MacAddress&,
                               const sai_fdb_entry_type_t&);

    bool isDestinationSame(FdbData &oldFdbData, FdbData &newFdbData);
};

#endif /* SWSS_FDBORCH_H */
