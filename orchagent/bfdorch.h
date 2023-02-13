#ifndef SWSS_BFDORCH_H
#define SWSS_BFDORCH_H

#include "orch.h"
#include "observer.h"

struct BfdUpdate
{
    std::string peer;
    sai_bfd_session_state_t state;
};

class BfdOrch: public Orch, public Subject
{
public:
    void doTask(Consumer &consumer);
    void doTask(swss::NotificationConsumer &consumer);
    BfdOrch(swss::DBConnector *db, std::string tableName, TableConnector stateDbBfdSessionTable);
    virtual ~BfdOrch(void);

private:
    bool create_bfd_session(const std::string& key, const std::vector<swss::FieldValueTuple>& data);
    bool remove_bfd_session(const std::string& key);
    std::string get_state_db_key(const std::string& vrf_name, const std::string& alias, const swss::IpAddress& peer_address);

    uint32_t bfd_gen_id(void);
    uint32_t bfd_src_port(void);

    bool register_bfd_state_change_notification(void);
    void update_port_number(std::vector<sai_attribute_t> &attrs);
    sai_status_t retry_create_bfd_session(sai_object_id_t &bfd_session_id, vector<sai_attribute_t> attrs);

    std::map<std::string, sai_object_id_t> bfd_session_map;
    std::map<sai_object_id_t, BfdUpdate> bfd_session_lookup;

    swss::Table m_stateBfdSessionTable;

    swss::NotificationConsumer* m_bfdStateNotificationConsumer;
    bool register_state_change_notif;
};

#endif /* SWSS_BFDORCH_H */
