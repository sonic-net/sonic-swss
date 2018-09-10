#ifndef SWSS_FDBORCH_H
#define SWSS_FDBORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"

struct FdbEntry
{
    MacAddress mac;
    sai_object_id_t bv_id;

    bool operator<(const FdbEntry& other) const
    {
        return tie(mac, bv_id) < tie(other.mac, other.bv_id);
    }
};

struct FdbUpdate
{
    FdbEntry entry;
    Port port;
    bool add;
};

struct SavedFdbEntry
{
    FdbEntry entry;
    string type;
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

    void update(sai_fdb_event_t, const sai_fdb_entry_t *, sai_object_id_t);
    void update(SubjectType type, void *cntx);
    bool getPort(const MacAddress&, uint16_t, Port&);
    void refreshFdbEntries();

private:
    PortsOrch *m_portsOrch;
    set<FdbEntry> m_entries;
    fdb_entries_by_port_t saved_fdb_entries;
    Table m_table;
    Table m_fdbStateTable;
    NotificationConsumer* m_flushNotificationsConsumer;
    NotificationConsumer* m_fdbNotificationConsumer;

    void doTask(Consumer& consumer);
    void doTask(NotificationConsumer& consumer);

    void updateVlanMember(const VlanMemberUpdate&);
    bool addFdbEntry(const FdbEntry&, const string&, const string&);
    bool removeFdbEntry(const FdbEntry&);
    bool createFdbEntry(const FdbEntry& entry, const Port& port, const string& type);
    void storeFdbEntry(const sai_fdb_event_notification_data_t *fdb);
};

#endif /* SWSS_FDBORCH_H */
