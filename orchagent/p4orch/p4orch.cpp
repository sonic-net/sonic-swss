#include "p4orch.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "copporch.h"
#include "logger.h"
#include "orch.h"
#include "p4orch/acl_rule_manager.h"
#include "p4orch/acl_table_manager.h"
#include "p4orch/ext_tables_manager.h"
#include "p4orch/gre_tunnel_manager.h"
#include "p4orch/l3_admit_manager.h"
#include "p4orch/l3_multicast_manager.h"
#include "p4orch/neighbor_manager.h"
#include "p4orch/next_hop_manager.h"
#include "p4orch/p4orch_util.h"
#include "p4orch/route_manager.h"
#include "p4orch/router_interface_manager.h"
#include "p4orch/tables_definition_manager.h"
#include "portsorch.h"
#include "return_code.h"
#include "sai_serialize.h"
#include "timer.h"
#include "timestamp.h"

extern PortsOrch *gPortsOrch;
#define P4_ACL_COUNTERS_STATS_POLL_TIMER_NAME "P4_ACL_COUNTERS_STATS_POLL_TIMER"
#define P4_EXT_COUNTERS_STATS_POLL_TIMER_NAME "P4_EXT_COUNTERS_STATS_POLL_TIMER"
#define APP_P4RT_EXT_TABLES_MANAGER "EXT_TABLES_MANAGER"

P4Orch::P4Orch(swss::DBConnector *db, std::vector<std::string> tableNames, VRFOrch *vrfOrch, CoppOrch *coppOrch)
    : Orch(db, tableNames)
{
    SWSS_LOG_ENTER();

    m_tablesDefnManager = std::make_unique<TablesDefnManager>(&m_p4OidMapper, &m_publisher);
    m_routerIntfManager = std::make_unique<RouterInterfaceManager>(&m_p4OidMapper, &m_publisher);
    m_neighborManager = std::make_unique<NeighborManager>(&m_p4OidMapper, &m_publisher);
    m_greTunnelManager = std::make_unique<GreTunnelManager>(&m_p4OidMapper, &m_publisher);
    m_nextHopManager = std::make_unique<NextHopManager>(&m_p4OidMapper, &m_publisher);
    m_l3MulticastManager = std::make_unique<p4orch::L3MulticastManager>(
        &m_p4OidMapper, vrfOrch, &m_publisher);
    m_routeManager = std::make_unique<RouteManager>(&m_p4OidMapper, vrfOrch, &m_publisher);
    m_mirrorSessionManager = std::make_unique<p4orch::MirrorSessionManager>(&m_p4OidMapper, &m_publisher);
    m_aclTableManager = std::make_unique<p4orch::AclTableManager>(&m_p4OidMapper, &m_publisher);
    m_aclRuleManager = std::make_unique<p4orch::AclRuleManager>(&m_p4OidMapper, vrfOrch, coppOrch, &m_publisher);
    m_wcmpManager = std::make_unique<p4orch::WcmpManager>(&m_p4OidMapper, &m_publisher);
    m_l3AdmitManager = std::make_unique<L3AdmitManager>(&m_p4OidMapper, &m_publisher);
    m_extTablesManager = std::make_unique<ExtTablesManager>(&m_p4OidMapper, vrfOrch, &m_publisher);

    m_p4TableToManagerMap[APP_P4RT_TABLES_DEFINITION_TABLE_NAME] = m_tablesDefnManager.get();
    m_p4TableToManagerMap[APP_P4RT_ROUTER_INTERFACE_TABLE_NAME] = m_routerIntfManager.get();
    m_p4TableToManagerMap[APP_P4RT_NEIGHBOR_TABLE_NAME] = m_neighborManager.get();
    m_p4TableToManagerMap[APP_P4RT_TUNNEL_TABLE_NAME] = m_greTunnelManager.get();
    m_p4TableToManagerMap[APP_P4RT_NEXTHOP_TABLE_NAME] = m_nextHopManager.get();
    m_p4TableToManagerMap[APP_P4RT_IPV4_TABLE_NAME] = m_routeManager.get();
    m_p4TableToManagerMap[APP_P4RT_IPV6_TABLE_NAME] = m_routeManager.get();
    m_p4TableToManagerMap[APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME] =
        m_l3MulticastManager.get();
    m_p4TableToManagerMap[APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME] =
        m_l3MulticastManager.get();
    m_p4TableToManagerMap[APP_P4RT_MIRROR_SESSION_TABLE_NAME] = m_mirrorSessionManager.get();
    m_p4TableToManagerMap[APP_P4RT_ACL_TABLE_DEFINITION_NAME] = m_aclTableManager.get();
    m_p4TableToManagerMap[APP_P4RT_WCMP_GROUP_TABLE_NAME] = m_wcmpManager.get();
    m_p4TableToManagerMap[APP_P4RT_L3_ADMIT_TABLE_NAME] = m_l3AdmitManager.get();
    m_p4TableToManagerMap[APP_P4RT_EXT_TABLES_MANAGER] = m_extTablesManager.get();

    m_p4ManagerAddPrecedence.push_back(m_tablesDefnManager.get());
    m_p4ManagerAddPrecedence.push_back(m_routerIntfManager.get());
    m_p4ManagerAddPrecedence.push_back(m_neighborManager.get());
    m_p4ManagerAddPrecedence.push_back(m_greTunnelManager.get());
    m_p4ManagerAddPrecedence.push_back(m_nextHopManager.get());
    m_p4ManagerAddPrecedence.push_back(m_wcmpManager.get());
    m_p4ManagerAddPrecedence.push_back(m_routeManager.get());
    m_p4ManagerAddPrecedence.push_back(m_mirrorSessionManager.get());
    m_p4ManagerAddPrecedence.push_back(m_aclTableManager.get());
    m_p4ManagerAddPrecedence.push_back(m_aclRuleManager.get());
    m_p4ManagerAddPrecedence.push_back(m_l3AdmitManager.get());
    m_p4ManagerAddPrecedence.push_back(m_l3MulticastManager.get());
    m_p4ManagerAddPrecedence.push_back(m_extTablesManager.get());
    for (auto* manager : m_p4ManagerAddPrecedence) {
      m_p4ManagerDelPrecedence.insert(m_p4ManagerDelPrecedence.begin(), manager);
    }

    tablesinfo = nullptr;
    // Add timer executor to update ACL counters stats in COUNTERS_DB
    auto acl_interv = timespec{.tv_sec = P4_COUNTERS_READ_INTERVAL, .tv_nsec = 0};
    m_aclCounterStatsTimer = new swss::SelectableTimer(acl_interv);
    auto acl_executor = new swss::ExecutableTimer(m_aclCounterStatsTimer, this, P4_ACL_COUNTERS_STATS_POLL_TIMER_NAME);
    Orch::addExecutor(acl_executor);
    m_aclCounterStatsTimer->start();

    // Add timer executor to update EXT counters stats in COUNTERS_DB
    auto ext_interv = timespec{.tv_sec = P4_COUNTERS_READ_INTERVAL, .tv_nsec = 0};
    m_extCounterStatsTimer = new swss::SelectableTimer(ext_interv);
    auto ext_executor = new swss::ExecutableTimer(m_extCounterStatsTimer, this, P4_EXT_COUNTERS_STATS_POLL_TIMER_NAME);
    Orch::addExecutor(ext_executor);
    m_extCounterStatsTimer->start();

    // Add p4rt notification handling support
    swss::DBConnector notificationsDb("ASIC_DB", 0);

    m_p4rtNotificationConsumer =
        new swss::NotificationConsumer(&notificationsDb, APP_P4RT_TABLE_NAME);
    auto p4rtNotifier =
        new Notifier(m_p4rtNotificationConsumer, this, "P4RT_NOTIFICATIONS");
    Orch::addExecutor(p4rtNotifier);

    // Add port state change notification handling support
    m_portStatusNotificationConsumer = new swss::NotificationConsumer(&notificationsDb, "NOTIFICATIONS");
    auto portStatusNotifier = new Notifier(m_portStatusNotificationConsumer, this, "PORT_STATUS_NOTIFICATIONS");
    Orch::addExecutor(portStatusNotifier);
}

void P4Orch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    const std::string table_name = consumer.getTableName();
    if (table_name != APP_P4RT_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Incorrect table name %s (expected %s)", table_name.c_str(), APP_P4RT_TABLE_NAME);
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        enqueue(it->second);
        it = consumer.m_toSync.erase(it);
    }
    drain(SET_COMMAND);
    m_publisher.flush();
}

void P4Orch::doTask(swss::SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    if (&timer == m_aclCounterStatsTimer)
    {
        m_aclRuleManager->doAclCounterStatsTask();
    }
    else if (&timer == m_extCounterStatsTimer)
    {
        m_extTablesManager->doExtCounterStatsTask();
    }
    else
    {
        SWSS_LOG_NOTICE("Unrecognized timer passed in P4Orch::doTask(swss::SelectableTimer& "
                        "timer)");
    }
}

void P4Orch::enqueue(const swss::KeyOpFieldsValuesTuple& entry) {
  const std::string& key = kfvKey(entry);
  const std::vector<swss::FieldValueTuple>& values = kfvFieldsValues(entry);
  std::string table_name;
  std::string key_content;
  parseP4RTKey(key, &table_name, &key_content);
  if (table_name.empty()) {
    auto status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                  << "Table name cannot be empty, but was empty in key: "
                  << key;
    SWSS_LOG_ERROR("%s", status.message().c_str());
    m_publisher.publish(APP_P4RT_TABLE_NAME, key, values, status,
                        /*replace=*/true);
    return;
  }
  if (m_p4TableToManagerMap.find(table_name) != m_p4TableToManagerMap.end()) {
    m_p4TableToManagerMap[table_name]->enqueue(table_name, entry);
  } else {
    if (table_name.rfind(p4orch::kTablePrefixEXT, 0) != std::string::npos) {
      m_p4TableToManagerMap[APP_P4RT_EXT_TABLES_MANAGER]->enqueue(table_name,
                                                                  entry);
    } else {
      auto status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                    << "Failed to find P4Orch Manager for " << table_name
                    << " P4RT DB table";
      SWSS_LOG_ERROR("%s", status.message().c_str());
      m_publisher.publish(APP_P4RT_TABLE_NAME, kfvKey(entry),
                          kfvFieldsValues(entry), status, /*replace=*/true);
    }
  }
}

ReturnCode P4Orch::drain(const std::string& op) {
  ReturnCode status;
  if (op == SET_COMMAND) {
    for (const auto& manager : m_p4ManagerAddPrecedence) {
      if (status.ok()) {
        status = manager->drain();
      } else {
        manager->drainWithNotExecuted();
      }
    }
  } else {
    for (const auto& manager : m_p4ManagerDelPrecedence) {
      if (status.ok()) {
        status = manager->drain();
      } else {
        manager->drainWithNotExecuted();
      }
    }
  }
  return status;
}

void P4Orch::handleP4rtNotification(
    const std::vector<swss::FieldValueTuple>& values) {
  std::string prev_op = "";
  ReturnCode status;
  for (const auto& value : values) {
    std::string op = DEL_COMMAND;
    std::vector<swss::FieldValueTuple> vals;
    if (!fvValue(value).empty()) {
      op = SET_COMMAND;
      JSon::readJson(fvValue(value), vals);
    }
    if (prev_op.empty()) {
      prev_op = op;
    }
    swss::KeyOpFieldsValuesTuple key_op_fvs_tuple(fvField(value), op, vals);

    // Call drain after grouping the same type of requests together.
    if (op != prev_op && status.ok()) {
      status = drain(prev_op);
      prev_op = op;
    }

    // Stop enqueue if there is any failure.
    if (status.ok()) {
      enqueue(key_op_fvs_tuple);
    } else {
      m_publisher.publish(APP_P4RT_TABLE_NAME, fvField(value), vals,
                          ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED),
                          /*replace=*/true);
    }
  }
  if (!prev_op.empty() && status.ok()) {
    drain(prev_op);
  }
}

void P4Orch::handlePortStatusChangeNotification(const std::string &op, const std::string &data)
{
    if (op == "port_state_change")
    {
        uint32_t count;
        sai_port_oper_status_notification_t *port_oper_status = nullptr;
        sai_deserialize_port_oper_status_ntf(data, count, &port_oper_status);

        for (uint32_t i = 0; i < count; i++)
        {
            sai_object_id_t id = port_oper_status[i].port_id;
            sai_port_oper_status_t status = port_oper_status[i].port_state;

            Port port;
            if (!gPortsOrch->getPort(id, port))
            {
                SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, id);
                continue;
            }

            // Update port oper-status in local map
            m_wcmpManager->updatePortOperStatusMap(port.m_alias, status);

            if (status == SAI_PORT_OPER_STATUS_UP)
            {
                m_wcmpManager->restorePrunedNextHops(port.m_alias);
            }
            else
            {
                m_wcmpManager->pruneNextHops(port.m_alias);
            }
        }

        sai_deserialize_free_port_oper_status_ntf(count, port_oper_status);
    }
}

void P4Orch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    std::string op, data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);


    if (&consumer == m_p4rtNotificationConsumer) {
      handleP4rtNotification(values);
    } else if (&consumer == m_portStatusNotificationConsumer) {
        handlePortStatusChangeNotification(op, data);
    }
}

bool P4Orch::addAclTableToManagerMapping(const std::string &acl_table_name)
{
    SWSS_LOG_ENTER();
    if (m_p4TableToManagerMap.find(acl_table_name) != m_p4TableToManagerMap.end())
    {
        SWSS_LOG_NOTICE("Consumer for ACL table %s already exists in P4Orch", acl_table_name.c_str());
        return false;
    }
    m_p4TableToManagerMap[acl_table_name] = m_aclRuleManager.get();
    return true;
}

bool P4Orch::removeAclTableToManagerMapping(const std::string &acl_table_name)
{
    SWSS_LOG_ENTER();
    if (m_p4TableToManagerMap.find(acl_table_name) == m_p4TableToManagerMap.end())
    {
        SWSS_LOG_NOTICE("Consumer for ACL table %s does not exist in P4Orch", acl_table_name.c_str());
        return false;
    }
    m_p4TableToManagerMap.erase(acl_table_name);
    return true;
}

p4orch::AclTableManager *P4Orch::getAclTableManager()
{
    return m_aclTableManager.get();
}

p4orch::AclRuleManager *P4Orch::getAclRuleManager()
{
    return m_aclRuleManager.get();
}

p4orch::WcmpManager *P4Orch::getWcmpManager()
{
    return m_wcmpManager.get();
}

GreTunnelManager *P4Orch::getGreTunnelManager()
{
    return m_greTunnelManager.get();
}
