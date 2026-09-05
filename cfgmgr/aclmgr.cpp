#include <string.h>
#include <sstream>
#include "logger.h"
#include "tokenize.h"
#include "exec.h"
#include "shellcmd.h"
#include "converter.h"
#include "warm_restart.h"
#include "schema.h"
#include "aclmgr.h"

using namespace std;
using namespace swss;

/* TC command paths */
#define TC_CMD "/sbin/tc"

AclMgr::AclMgr(DBConnector *cfgDb, DBConnector *stateDb,
               const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    /* CONFIG_DB tables for reading saved state */
    m_cfgAclTable(cfgDb, CFG_ACL_TABLE_TABLE_NAME),
    m_cfgAclRuleTable(cfgDb, CFG_ACL_RULE_TABLE_NAME),
    /* STATE_DB tables for writing status */
    m_stateAclTable(stateDb, STATE_ACL_TABLE_TABLE_NAME),
    m_stateAclRuleTable(stateDb, STATE_ACL_RULE_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    if (WarmStart::isWarmStart())
    {
        /* Cache existing ACL config keys for warm restart reconciliation */
        vector<string> tableKeys, ruleKeys;
        m_cfgAclTable.getKeys(tableKeys);
        m_cfgAclRuleTable.getKeys(ruleKeys);

        for (auto &k : tableKeys)
        {
            m_programmedTables.insert(k);
        }

        WarmStart::setWarmStartState("aclmgrd", WarmStart::REPLAYED);
        SWSS_LOG_NOTICE("aclmgrd warmstart state set to REPLAYED");
        WarmStart::setWarmStartState("aclmgrd", WarmStart::RECONCILED);
        SWSS_LOG_NOTICE("aclmgrd warmstart state set to RECONCILED");
    }

    /* No global kernel init needed for ACLs (unlike vlanmgrd which creates a bridge).
     * ACL filters are created per-interface when rules are applied. */
    SWSS_LOG_NOTICE("AclMgr initialized, subscribed to %zu CONFIG_DB tables", tableNames.size());
}

void AclMgr::doTask(Consumer &consumer)
{
    string table_name = consumer.getTableName();

    SWSS_LOG_DEBUG("doTask: table=%s", table_name.c_str());

    if (table_name == CFG_ACL_TABLE_TABLE_NAME)
        doAclTableTask(consumer);
    else if (table_name == CFG_ACL_RULE_TABLE_NAME)
        doAclRuleTask(consumer);
    else if (table_name == CFG_ACL_TABLE_TYPE_TABLE_NAME)
    {
        /* ACL_TABLE_TYPE defines reusable match templates.
         * Phase 1: acknowledge but don't process. Just drain events. */
        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            SWSS_LOG_DEBUG("ACL_TABLE_TYPE event: key=%s op=%s (skipped in Phase 1)",
                           kfvKey(it->second).c_str(), kfvOp(it->second).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
    else
    {
        SWSS_LOG_ERROR("AclMgr doTask: unknown table '%s'", table_name.c_str());
        throw runtime_error("AclMgr doTask failure: unknown table " + table_name);
    }
}

void AclMgr::doAclTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string table_id = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("ACL_TABLE: key=%s op=%s", table_id.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            /* Extract fields from the operation */
            string table_type;
            string ports;
            string stage;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);

                if (field == ACL_TABLE_FIELD_TYPE)
                    table_type = value;
                else if (field == ACL_TABLE_FIELD_PORTS)
                    ports = value;
                else if (field == ACL_TABLE_FIELD_STAGE)
                    stage = value;
            }

            SWSS_LOG_NOTICE("ACL_TABLE SET: id=%s type=%s ports=%s stage=%s",
                            table_id.c_str(), table_type.c_str(),
                            ports.c_str(), stage.c_str());

            /* Phase 1: validate and track L3 ACL tables only */
            if (table_type == ACL_TYPE_L3 || table_type == ACL_TYPE_L3V6)
            {
                if (ports.empty())
                {
                    SWSS_LOG_WARN("ACL_TABLE %s: no ports specified, ACL not applied",
                                  table_id.c_str());
                }

                m_programmedTables.insert(table_id);

                /* Write success state to STATE_DB (field name "status" matches AclOrch format) */
                vector<FieldValueTuple> stateFvs;
                FieldValueTuple fv("status", "Active");
                stateFvs.push_back(fv);
                m_stateAclTable.set(table_id, stateFvs);

                SWSS_LOG_NOTICE("ACL_TABLE %s: accepted (type=%s)", table_id.c_str(), table_type.c_str());
            }
            else
            {
                SWSS_LOG_INFO("ACL_TABLE %s: type '%s' not supported in Phase 1, skipping",
                              table_id.c_str(), table_type.c_str());

                /* Still write to STATE_DB — mark as inactive */
                vector<FieldValueTuple> stateFvs;
                FieldValueTuple fv("status", "Inactive");
                stateFvs.push_back(fv);
                m_stateAclTable.set(table_id, stateFvs);
            }

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("ACL_TABLE DEL: id=%s", table_id.c_str());

            /* Remove from internal tracking */
            m_programmedTables.erase(table_id);

            /* Remove from STATE_DB */
            m_stateAclTable.del(table_id);

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("ACL_TABLE: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void AclMgr::doAclRuleTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        /* Key format: table_id|rule_id (separator from ConsumerStateTable) */
        string table_id;
        string rule_id;
        size_t sep_pos = key.find(consumer.getConsumerTable()->getTableNameSeparator());
        if (sep_pos != string::npos)
        {
            table_id = key.substr(0, sep_pos);
            rule_id = key.substr(sep_pos + 1);
        }
        else
        {
            /* Try alternative parsing: the key IS the rule name and table is embedded */
            table_id = "UNKNOWN";
            rule_id = key;
        }

        SWSS_LOG_INFO("ACL_RULE: key=%s table_id=%s rule_id=%s op=%s",
                       key.c_str(), table_id.c_str(), rule_id.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            /* Check if parent table is known */
            if (m_programmedTables.find(table_id) == m_programmedTables.end())
            {
                SWSS_LOG_WARN("ACL_RULE %s: parent table '%s' not yet programmed, deferring",
                              key.c_str(), table_id.c_str());
                it++;  /* Defer — don't erase, will retry on next doTask() */
                continue;
            }

            /* Extract match fields and action */
            string src_ip, dst_ip, l4_src_port, l4_dst_port;
            string ip_proto, tcp_flags, dscp, packet_action;
            string priority_str;
            uint32_t priority = 100;  /* Default tc filter priority */

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);

                if (field == ACL_RULE_FIELD_SRC_IP)        src_ip = value;
                else if (field == ACL_RULE_FIELD_DST_IP)   dst_ip = value;
                else if (field == ACL_RULE_FIELD_L4_SRC_PORT) l4_src_port = value;
                else if (field == ACL_RULE_FIELD_L4_DST_PORT) l4_dst_port = value;
                else if (field == ACL_RULE_FIELD_IP_PROTOCOL) ip_proto = value;
                else if (field == ACL_RULE_FIELD_TCP_FLAGS)   tcp_flags = value;
                else if (field == ACL_RULE_FIELD_DSCP)        dscp = value;
                else if (field == ACL_RULE_FIELD_PACKET_ACTION) packet_action = value;
                else if (field == ACL_RULE_FIELD_PRIORITY)    priority_str = value;
            }

            /* Parse priority if provided */
            if (!priority_str.empty())
            {
                try { priority = (uint32_t)stoul(priority_str); }
                catch (...) {priority = 100; }
            }

            /* Map PACKET_ACTION to tc flower action */
            string tc_action = "drop";  /* Default: drop */
            if (packet_action == "FORWARD")
                tc_action = "ok";
            else if (packet_action == "DROP")
                tc_action = "drop";
            else if (!packet_action.empty())
                SWSS_LOG_WARN("Unknown PACKET_ACTION '%s', defaulting to drop", packet_action.c_str());

            SWSS_LOG_NOTICE("ACL_RULE SET: table=%s rule=%s src_ip=%s dst_ip=%s "
                            "l4_sport=%s l4_dport=%s ip_proto=%s tcp_flags=%s dscp=%s "
                            "action=%s(tc=%s) prio=%u",
                            table_id.c_str(), rule_id.c_str(),
                            src_ip.c_str(), dst_ip.c_str(),
                            l4_src_port.c_str(), l4_dst_port.c_str(),
                            ip_proto.c_str(), tcp_flags.c_str(), dscp.c_str(),
                            packet_action.c_str(), tc_action.c_str(), priority);

            /* Program tc filters on each port in the parent table.
             * For Phase 1, we apply the filter to all switchdev interfaces.
             * The port list from ACL_TABLE is stored, and we resolve interfaces
             * via /sys/class/net/. */
            bool all_ok = true;
            vector<string> interfaces_to_program;

            /* Read port list from parent ACL_TABLE */
            {
                vector<FieldValueTuple> tableFvs;
                if (m_cfgAclTable.get(table_id, tableFvs))
                {
                    for (auto &fv : tableFvs)
                    {
                        if (fvField(fv) == ACL_TABLE_FIELD_PORTS)
                        {
                            /* Ports are comma-separated: "Ethernet0,Ethernet4" */
                            string ports_list = fvValue(fv);
                            interfaces_to_program = tokenize(ports_list, ',');
                        }
                    }
                }
            }

            /* If no ports specified, log warning — rule stored but not applied */
            if (interfaces_to_program.empty())
            {
                SWSS_LOG_WARN("ACL_RULE %s: no ports in parent table, rule stored but not applied",
                              key.c_str());
            }

            for (auto &iface : interfaces_to_program)
            {
                if (!addTcFlowerFilter(iface, priority,
                                       src_ip, dst_ip,
                                       l4_src_port, l4_dst_port,
                                       ip_proto, tcp_flags, dscp,
                                       tc_action))
                {
                    SWSS_LOG_ERROR("Failed to add tc filter on %s for rule %s",
                                   iface.c_str(), key.c_str());
                    all_ok = false;
                }
            }

            /* Track priority for deletion */
            m_priorities[key] = priority;

            /* Write STATE_DB (field name "status" matches AclOrch/show acl format) */
            vector<FieldValueTuple> stateFvs;
            FieldValueTuple fv("status", all_ok ? "Active" : "Inactive");
            stateFvs.push_back(fv);
            m_stateAclRuleTable.set(key, stateFvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("ACL_RULE DEL: key=%s", key.c_str());

            /* Remove tc filters from all ports */
            uint32_t prio = 100;
            auto prio_it = m_priorities.find(key);
            if (prio_it != m_priorities.end())
            {
                prio = prio_it->second;
                m_priorities.erase(prio_it);
            }

            /* Read port list from parent ACL_TABLE */
            vector<string> interfaces_to_clean;
            {
                vector<FieldValueTuple> tableFvs;
                if (m_cfgAclTable.get(table_id, tableFvs))
                {
                    for (auto &fv : tableFvs)
                    {
                        if (fvField(fv) == ACL_TABLE_FIELD_PORTS)
                        {
                            string ports_list = fvValue(fv);
                            interfaces_to_clean = tokenize(ports_list, ',');
                        }
                    }
                }
            }

            for (auto &iface : interfaces_to_clean)
            {
                removeTcFlowerFilter(iface, prio);
            }

            /* Remove from STATE_DB */
            m_stateAclRuleTable.del(key);

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("ACL_RULE: unknown operation '%s'", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/*
 * addTcFlowerFilter — program a tc flower filter on an interface.
 *
 * Builds and executes a tc filter command:
 *   tc filter add dev <iface> ingress prio <prio> flower
 *       [src_ip <ip>] [dst_ip <ip>]
 *       [ip_proto <proto>] [src_port <port>] [dst_port <port>]
 *       [tcp_flags <flags>] [ip_tos <tos>]
 *       skip_sw action <drop|ok>
 *
 * The "skip_sw" flag ensures the filter is ONLY offloaded to hardware (ASIC)
 * via switchdev, and is NOT processed in the kernel software datapath.
 */
bool AclMgr::addTcFlowerFilter(const string &iface, uint32_t prio,
                                const string &srcIp, const string &dstIp,
                                const string &l4SrcPort, const string &l4DstPort,
                                const string &ipProto, const string &tcpFlags,
                                const string &dscp, const string &action)
{
    SWSS_LOG_ENTER();

    /* Ensure ingress qdisc exists on the interface.
     * tc flower filters require the ingress qdisc as the attachment point.
     * If it doesn't exist, create it. This is idempotent — tc returns
     * "Error: Exclusivity flag on, cannot modify" if already present,
     * which we silently ignore. */
    ostringstream qdisc_cmd;
    qdisc_cmd << TC_CMD << " qdisc add dev " << iface << " ingress";
    string ignored;
    swss::exec(qdisc_cmd.str(), ignored);

    /* Build tc filter command.
     * Using shell commands (same pattern as vlanmgrd's IP_CMD/BRIDGE_CMD).
     *
     * tc flower syntax (iproute2 6.1):
     *   tc filter add dev <iface> ingress protocol ip prio <prio> flower
     *       [src_ip <prefix>] [dst_ip <prefix>]
     *       [ip_proto tcp|udp|sctp|icmp|icmpv6] [src_port <n>] [dst_port <n>]
     *       [tcp_flags <hex>] [ip_tos <tos>]
     *       skip_sw action <drop|ok|pass>
     */
    ostringstream cmd;

    /* protocol ip is REQUIRED for IPv4 flower filters in tc */
    cmd << TC_CMD << " filter add dev " << iface
        << " ingress protocol ip prio " << prio
        << " flower";

    if (!srcIp.empty())
        cmd << " src_ip " << srcIp;
    if (!dstIp.empty())
        cmd << " dst_ip " << dstIp;
    if (!ipProto.empty())
    {
        /* tc flower ip_proto expects protocol name strings (tcp, udp, etc.),
         * NOT numbers. CONFIG_DB stores "6" for TCP, "17" for UDP, etc.
         * Map numeric strings to names. */
        string proto_str = ipProto;
        if (proto_str == "6" || proto_str == "tcp" || proto_str == "TCP") proto_str = "tcp";
        else if (proto_str == "17" || proto_str == "udp" || proto_str == "UDP") proto_str = "udp";
        else if (proto_str == "1" || proto_str == "icmp" || proto_str == "ICMP") proto_str = "icmp";
        else if (proto_str == "58" || proto_str == "icmpv6" || proto_str == "ICMPV6") proto_str = "icmpv6";
        else if (proto_str == "132" || proto_str == "sctp" || proto_str == "SCTP") proto_str = "sctp";
        else proto_str = ipProto;  /* Pass through unknown strings */
        cmd << " ip_proto " << proto_str;
    }
    if (!l4SrcPort.empty())
        cmd << " src_port " << l4SrcPort;
    if (!l4DstPort.empty())
        cmd << " dst_port " << l4DstPort;
    if (!tcpFlags.empty())
    {
        /* TCP flags may be specified as hex (0x2/SYN) or decimal.
         * tc flower expects hex. If it starts with "0x", use as-is. */
        if (tcpFlags.find("0x") == 0 || tcpFlags.find("0X") == 0)
            cmd << " tcp_flags " << tcpFlags;
        else
            cmd << " tcp_flags 0x" << tcpFlags;
    }
    if (!dscp.empty())
    {
        /* DSCP → TOS: DSCP value is upper 6 bits of TOS byte.
         * tc flower ip_tos matches on the full TOS byte (8 bits).
         * Shift DSCP left by 2 to get TOS value. */
        try
        {
            uint8_t dscp_val = (uint8_t)stoi(dscp);
            uint32_t tos_val = dscp_val << 2;
            cmd << " ip_tos " << tos_val;
        }
        catch (...)
        {
            SWSS_LOG_WARN("Invalid DSCP value: %s, skipping ip_tos match", dscp.c_str());
        }
    }

    /* skip_sw: offload to HW only via switchdev.
     * In non-switchdev containers, skip_sw fails with "Operation not supported".
     * We try with skip_sw first, then fall back to software mode. */
    string cmd_hw = cmd.str() + " skip_sw action " + action;

    SWSS_LOG_NOTICE("Executing: %s", cmd_hw.c_str());

    string res;
    int ret = swss::exec(cmd_hw, res);

    if (ret != 0)
    {
        /* skip_sw not supported — fall back to software-only mode */
        SWSS_LOG_WARN("tc filter with skip_sw failed on %s (ret=%d), "
                       "falling back to software mode", iface.c_str(), ret);

        string cmd_sw = cmd.str() + " action " + action;
        SWSS_LOG_NOTICE("Executing (fallback): %s", cmd_sw.c_str());

        ret = swss::exec(cmd_sw, res);
        if (ret != 0)
        {
            SWSS_LOG_ERROR("tc filter add failed on %s (ret=%d): %s",
                           iface.c_str(), ret, res.c_str());
            return false;
        }
    }

    SWSS_LOG_INFO("tc filter added on %s", iface.c_str());
    return true;
}

/*
 * removeTcFlowerFilter — remove a tc flower filter by interface and priority.
 *
 * tc uses the "handle" to identify filters, but when adding with prio only,
 * we can delete by matching the priority:
 *   tc filter del dev <iface> ingress prio <prio> flower
 */
bool AclMgr::removeTcFlowerFilter(const string &iface, uint32_t prio)
{
    SWSS_LOG_ENTER();

    ostringstream cmd;
    cmd << TC_CMD << " filter del dev " << iface
        << " ingress protocol ip prio " << prio
        << " flower";

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);

    /* tc filter del returns 0 on success, non-zero if filter doesn't exist.
     * Don't treat "not found" as hard error — the filter may already be gone. */
    if (ret != 0)
    {
        SWSS_LOG_WARN("tc filter del on %s prio %u (ret=%d): %s",
                      iface.c_str(), prio, ret, res.c_str());
        /* Still return true — deletion is idempotent */
    }

    SWSS_LOG_INFO("tc filter removed on %s prio %u", iface.c_str(), prio);
    return true;
}
