#ifndef SWSS_BFDORCH_H
#define SWSS_BFDORCH_H

#include "orch.h"
#include "intfsorch.h"
#include "vrforch.h"

#define BFD_SRCPORTINIT 49152
#define BFD_SRCPORTMAX 65536

struct BfdUpdate
{
    std::string peer;
    sai_bfd_session_state_t state;
};

class BfdOrch: public Orch
{
public:
    void doTask(Consumer &consumer);
    void doTask(NotificationConsumer &consumer);
    BfdOrch(swss::DBConnector *db, std::string tableName);
    virtual ~BfdOrch(void);

private:
    bool create_bfd_session(const string& key, const vector<FieldValueTuple>& data);
    bool remove_bfd_session(const string& key);

    uint32_t bfd_gen_id(void);
    uint32_t bfd_src_port(void);

    std::map<std::string, sai_object_id_t> bfd_session_map;
    std::map<sai_object_id_t, BfdUpdate> bfd_session_lookup;

    shared_ptr<DBConnector> m_state_db;
    unique_ptr<Table> m_stateBfdSessionTable;

    NotificationConsumer* m_bfdStateNotificationConsumer;
};

#endif /* SWSS_BFDORCH_H */
