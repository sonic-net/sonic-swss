#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "copporch.h"
#include "notificationconsumer.h"
#include "notifier.h"
#include "orch.h"
#include "p4orch/acl_rule_manager.h"
#include "p4orch/acl_table_manager.h"
#include "p4orch/ext_tables_manager.h"
#include "p4orch/gre_tunnel_manager.h"
#include "p4orch/l3_admit_manager.h"
#include "p4orch/l3_multicast_manager.h"
#include "p4orch/mirror_session_manager.h"
#include "p4orch/neighbor_manager.h"
#include "p4orch/next_hop_manager.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/route_manager.h"
#include "p4orch/router_interface_manager.h"
#include "p4orch/tables_definition_manager.h"
#include "p4orch/wcmp_manager.h"
#include "response_publisher.h"
#include "return_code.h"
#include "vrforch.h"

static const std::map<std::string, std::string> FixedTablesMap = {
    {"router_interface_table", APP_P4RT_ROUTER_INTERFACE_TABLE_NAME},
    {"neighbor_table", APP_P4RT_NEIGHBOR_TABLE_NAME},
    {"nexthop_table", APP_P4RT_NEXTHOP_TABLE_NAME},
    {"wcmp_group_table", APP_P4RT_WCMP_GROUP_TABLE_NAME},
    {"ipv4_table", APP_P4RT_IPV4_TABLE_NAME},
    {"ipv6_table", APP_P4RT_IPV6_TABLE_NAME},
    {"mirror_session_table", APP_P4RT_MIRROR_SESSION_TABLE_NAME},
    {"l3_admit_table", APP_P4RT_L3_ADMIT_TABLE_NAME},
    {"tunnel_table", APP_P4RT_TUNNEL_TABLE_NAME}};

class P4Orch : public Orch
{
  public:
    P4Orch(swss::DBConnector *db, std::vector<std::string> tableNames, VRFOrch *vrfOrch, CoppOrch *coppOrch);
    // Add ACL table to ACLRuleManager mapping in P4Orch.
    bool addAclTableToManagerMapping(const std::string &acl_table_name);
    // Remove the ACL table name to AclRuleManager mapping in P4Orch
    bool removeAclTableToManagerMapping(const std::string &acl_table_name);
    p4orch::AclTableManager *getAclTableManager();
    p4orch::AclRuleManager *getAclRuleManager();
    p4orch::WcmpManager *getWcmpManager();
    GreTunnelManager *getGreTunnelManager();
    TablesInfo *tablesinfo = NULL;

    // m_p4TableToManagerMap: P4 APP DB table name, P4 Object Manager
    std::unordered_map<std::string, ObjectManagerInterface *> m_p4TableToManagerMap;

  private:
    void doTask(Consumer &consumer);
    void doTask(swss::SelectableTimer &timer);
    void doTask(swss::NotificationConsumer &consumer);
    void enqueue(const swss::KeyOpFieldsValuesTuple& entry);
    ReturnCode drain(const std::string& op);
    void handleP4rtNotification(const std::vector<swss::FieldValueTuple>& values);
    void handlePortStatusChangeNotification(const std::string &op, const std::string &data);

    // P4 object manager request processing order.
    std::vector<ObjectManagerInterface*> m_p4ManagerAddPrecedence;
    std::vector<ObjectManagerInterface*> m_p4ManagerDelPrecedence;

    swss::SelectableTimer *m_aclCounterStatsTimer;
    swss::SelectableTimer *m_extCounterStatsTimer;
    P4OidMapper m_p4OidMapper;
    std::unique_ptr<TablesDefnManager> m_tablesDefnManager;
    std::unique_ptr<RouterInterfaceManager> m_routerIntfManager;
    std::unique_ptr<GreTunnelManager> m_greTunnelManager;
    std::unique_ptr<NeighborManager> m_neighborManager;
    std::unique_ptr<NextHopManager> m_nextHopManager;
    std::unique_ptr<RouteManager> m_routeManager;
    std::unique_ptr<p4orch::L3MulticastManager> m_l3MulticastManager;
    std::unique_ptr<p4orch::MirrorSessionManager> m_mirrorSessionManager;
    std::unique_ptr<p4orch::AclTableManager> m_aclTableManager;
    std::unique_ptr<p4orch::AclRuleManager> m_aclRuleManager;
    std::unique_ptr<p4orch::WcmpManager> m_wcmpManager;
    std::unique_ptr<L3AdmitManager> m_l3AdmitManager;
    std::unique_ptr<ExtTablesManager> m_extTablesManager;

    // Notification consumer for port state change
    swss::NotificationConsumer* m_p4rtNotificationConsumer;
    swss::NotificationConsumer *m_portStatusNotificationConsumer;

    // Sepcial publisher that writes to APPL DB instead of APPL STATE DB.
    ResponsePublisher m_publisher{"APPL_DB", /*bool buffered=*/true, /*db_write_thread=*/true};

    friend class P4OrchTest;
    friend class p4orch::test::WcmpManagerTest;
};
