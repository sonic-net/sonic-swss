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
#include "p4orch/mirror_session_manager.h"
#include "p4orch/neighbor_manager.h"
#include "p4orch/next_hop_manager.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/route_manager.h"
#include "p4orch/router_interface_manager.h"
#include "p4orch/wcmp_manager.h"
#include "response_publisher.h"
#include "vrforch.h"

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

  private:
    void doTask(Consumer &consumer);
    void doTask(swss::SelectableTimer &timer);
    void doTask(swss::NotificationConsumer &consumer);
    void handlePortStatusChangeNotification(const std::string &op, const std::string &data);

    // m_p4TableToManagerMap: P4 APP DB table name, P4 Object Manager
    std::unordered_map<std::string, ObjectManagerInterface *> m_p4TableToManagerMap;
    // P4 object manager request processing order.
    std::vector<ObjectManagerInterface *> m_p4ManagerPrecedence;

    swss::SelectableTimer *m_aclCounterStatsTimer;
    P4OidMapper m_p4OidMapper;
    std::unique_ptr<RouterInterfaceManager> m_routerIntfManager;
    std::unique_ptr<NeighborManager> m_neighborManager;
    std::unique_ptr<NextHopManager> m_nextHopManager;
    std::unique_ptr<RouteManager> m_routeManager;
    std::unique_ptr<p4orch::MirrorSessionManager> m_mirrorSessionManager;
    std::unique_ptr<p4orch::AclTableManager> m_aclTableManager;
    std::unique_ptr<p4orch::AclRuleManager> m_aclRuleManager;
    std::unique_ptr<p4orch::WcmpManager> m_wcmpManager;

    // Notification consumer for port state change
    swss::NotificationConsumer *m_portStatusNotificationConsumer;

    friend class p4orch::test::WcmpManagerTest;
};
