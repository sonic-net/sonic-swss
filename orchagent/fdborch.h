#ifndef SWSS_FDBORCH_H
#define SWSS_FDBORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"

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
};

struct FdbData
{
    sai_object_id_t bridge_port_id;
    string type;
};

struct FdbFlushUpdate
{
    vector<FdbEntry> entries;
    Port port;
};

struct SavedFdbEntry
{
    MacAddress mac;
    unsigned short vlanId;
    string type;
    bool operator==(const SavedFdbEntry& other) const
    {
        return tie(mac, vlanId) == tie(other.mac, other.vlanId);
    }
};

typedef unordered_map<string, vector<SavedFdbEntry>> fdb_entries_by_port_t;

class FdbOrch: public Orch, public Subject, public Observer
{
public:

    FdbOrch(TableConnector applDbConnector, TableConnector stateDbConnector, PortsOrch *port);

    ~FdbOrch()
    {
        m_portsOrch->detach(this);
    }

    bool bake() override;
    void update(sai_fdb_event_t, const sai_fdb_entry_t *, sai_object_id_t);
    void update(SubjectType type, void *cntx);
    bool getPort(const MacAddress&, uint16_t, Port&);
    bool flushFdbByPortVlan(const string &, const string &, bool flush_static);
    bool flushFdbByVlan(const string &, bool flush_static);
    bool flushFdbByPort(const string &, bool flush_static);
    bool flushFdbAll(bool flush_static);
    bool removeFdbEntry(const FdbEntry&);
    void flushFDBEntries(sai_object_id_t bridge_port_oid,
                         sai_object_id_t vlan_oid);
    void notifyObserversFDBFlush(Port &p, sai_object_id_t&);

private:
    PortsOrch *m_portsOrch;
    map<FdbEntry, FdbData> m_entries;
    fdb_entries_by_port_t saved_fdb_entries;
    Table m_table;
    Table m_fdbStateTable;
    NotificationConsumer* m_flushNotificationsConsumer;
    NotificationConsumer* m_fdbNotificationConsumer;

    void doTask(Consumer& consumer);
    void doTask(NotificationConsumer& consumer);

    void updateVlanMember(const VlanMemberUpdate&);
    bool addFdbEntry(const FdbEntry&, const string&, const string&);
    void deleteFdbEntryFromSavedFDB(const MacAddress &mac, const unsigned short &vlanId, const string portName="");

    void updatePortOperState(const PortOperStateUpdate&);
    bool removeFdbEntry(const FdbEntry&);
    bool storeFdbEntryState(const FdbUpdate& update);
};

#endif /* SWSS_FDBORCH_H */
