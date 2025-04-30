#ifndef DASHHAORCH_H
#define DASHHAORCH_H
#include <map>

#include "dbconnector.h"
#include "dashorch.h"
#include "zmqorch.h"
#include "zmqserver.h"
#include "saitypes.h"

#include "dash_api/ha_set.pb.h"
#include "dash_api/ha_scope.pb.h"

struct HaSetEntry
{
    sai_object_id_t ha_set_id;
    dash::ha_set::HaSet metadata;
};

struct HaScopeEntry
{
    sai_object_id_t ha_scope_id;
    dash::ha_scope::HaScope metadata;
};

typedef std::map<std::string, HaSetEntry> HaSetTable;
typedef std::map<std::string, HaScopeEntry> HaScopeTable;

class DashHaOrch : public ZmqOrch
{
public:
    DashHaOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, DashOrch *dash_orch, swss::ZmqServer *zmqServer);

private:
    HaSetTable m_ha_set_entries;
    HaScopeTable m_ha_scope_entries;

    DashOrch *m_dash_orch;

    void doTask(ConsumerBase &consumer);
    void doTaskEniTable(ConsumerBase &consumer);
    void doTaskHaSetTable(ConsumerBase &consumer);
    void doTaskHaScopeTable(ConsumerBase &consumer);

    bool addHaSetEntry(const std::string &key, const dash::ha_set::HaSet &entry);
    bool removeHaSetEntry(const std::string &key);
    bool addHaScopeEntry(const std::string &key, const dash::ha_scope::HaScope &entry);
    bool removeHaScopeEntry(const std::string &key);
    bool setHaScopeHaRole(const std::string &key, const dash::ha_scope::HaScope &entry);
    bool setHaScopeFlowReconcileRequest(const  std::string &key);
    bool setHaScopeActivateRoleRequest(const std::string &key);
    bool setEniHaScopeId(const sai_object_id_t eni_id, const sai_object_id_t ha_scope_id);

    sai_ip_address_t covertPbIpaddrToSaiIpaddr(const dash::types::IpAddress &ipaddr);

public:
    const HaSetTable& getHaSetEntries() const { return m_ha_set_entries; };
    const HaScopeTable& getHaScopeEntries() const { return m_ha_scope_entries; };
};

#endif // DASHHAORCH_H
