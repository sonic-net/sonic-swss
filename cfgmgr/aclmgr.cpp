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
#include "kernutil.h"

using namespace std;
using namespace swss;

/* TC command paths */
#define TC_CMD "/sbin/tc"

/* Table types aclmgrd can program in switchdev/tc. */
static const set<string> kSupportedTableTypes = {
    ACL_TYPE_L3, ACL_TYPE_L3V6, ACL_TYPE_L3V4V6,
    ACL_TYPE_MIRROR, ACL_TYPE_MIRRORV6, ACL_TYPE_MIRROR_DSCP,
    ACL_TYPE_CTRLPLANE, ACL_TYPE_DROP, ACL_TYPE_EGR_SET_DSCP,
};

AclMgr::AclMgr(DBConnector *cfgDb, DBConnector *stateDb,
               const vector<string> &tableNames) :
    Orch(cfgDb, stateDb, tableNames, {}),
    m_cfgDb(cfgDb),
    m_stateDb(stateDb),
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
        vector<string> tableKeys, ruleKeys;
        m_cfgAclTable.getKeys(tableKeys);
        m_cfgAclRuleTable.getKeys(ruleKeys);

        for (auto &k : tableKeys)
            m_programmedTables.insert(k);

        WarmStart::setWarmStartState("aclmgrd", WarmStart::REPLAYED);
        WarmStart::setWarmStartState("aclmgrd", WarmStart::RECONCILED);
    }

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
        /* ACL_TABLE_TYPE defines reusable match/action templates. AclOrch uses
         * them to validate rules against the table's declared capabilities.
         * tc flower is generic, so we acknowledge but do not strictly enforce
         * them yet. Drain events. */
        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            SWSS_LOG_DEBUG("ACL_TABLE_TYPE event: key=%s op=%s (not enforced)",
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
            string table_type, ports, stage;

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

            bool supported = (kSupportedTableTypes.find(table_type) != kSupportedTableTypes.end());

            if (supported)
            {
                if (ports.empty())
                    SWSS_LOG_WARN("ACL_TABLE %s: no ports specified, ACL not applied",
                                  table_id.c_str());

                m_programmedTables.insert(table_id);

                vector<FieldValueTuple> stateFvs;
                FieldValueTuple fv("status", "Active");
                stateFvs.push_back(fv);
                m_stateAclTable.set(table_id, stateFvs);

                SWSS_LOG_NOTICE("ACL_TABLE %s: accepted (type=%s)", table_id.c_str(), table_type.c_str());
            }
            else
            {
                SWSS_LOG_INFO("ACL_TABLE %s: type '%s' not supported, skipping",
                              table_id.c_str(), table_type.c_str());

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

            m_programmedTables.erase(table_id);
            m_stateAclTable.del(table_id);

            /* Remove tc filters for any rules still tracked under this table.
             * The CONFIG_DB ACL_RULE entries may remain (the CLI does not cascade
             * rule deletion), but their dataplane filters must go with the table. */
            string prefix = table_id + "|";
            for (auto rit = m_ruleState.begin(); rit != m_ruleState.end(); )
            {
                if (rit->first.compare(0, prefix.size(), prefix) == 0)
                {
                    for (auto &iface : rit->second.interfaces)
                        removeTcFlowerFilter(iface, rit->second);
                    /* Clean the STATE_DB rule entry too — otherwise removing a
                     * table leaves an orphaned ACL_RULE_TABLE|<table>|<rule>
                     * status key (CONFIG_DB rules are not cascaded by the CLI). */
                    m_stateAclRuleTable.del(rit->first);
                    rit = m_ruleState.erase(rit);
                }
                else
                {
                    ++rit;
                }
            }

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

        string table_id, rule_id;
        size_t sep_pos = key.find(consumer.getConsumerTable()->getTableNameSeparator());
        if (sep_pos != string::npos)
        {
            table_id = key.substr(0, sep_pos);
            rule_id = key.substr(sep_pos + 1);
        }
        else
        {
            table_id = "UNKNOWN";
            rule_id = key;
        }

        SWSS_LOG_INFO("ACL_RULE: key=%s table_id=%s rule_id=%s op=%s",
                       key.c_str(), table_id.c_str(), rule_id.c_str(), op.c_str());

        if (op == SET_COMMAND)
        {
            if (m_programmedTables.find(table_id) == m_programmedTables.end())
            {
                SWSS_LOG_WARN("ACL_RULE %s: parent table '%s' not yet programmed, deferring",
                              key.c_str(), table_id.c_str());
                it++;
                continue;
            }

            AclRuleFields fields;
            vector<string> unknown_fields;

            for (auto i : kfvFieldsValues(t))
            {
                string field = fvField(i);
                string value = fvValue(i);

                if (field == ACL_RULE_FIELD_SRC_IP)              fields.src_ip = value;
                else if (field == ACL_RULE_FIELD_DST_IP)         fields.dst_ip = value;
                else if (field == ACL_RULE_FIELD_SRC_IP_MASK)    fields.src_ip_mask = value;
                else if (field == ACL_RULE_FIELD_DST_IP_MASK)    fields.dst_ip_mask = value;
                else if (field == ACL_RULE_FIELD_SRC_IPV6)       fields.src_ipv6 = value;
                else if (field == ACL_RULE_FIELD_DST_IPV6)       fields.dst_ipv6 = value;
                else if (field == ACL_RULE_FIELD_L4_SRC_PORT)    fields.l4_src_port = value;
                else if (field == ACL_RULE_FIELD_L4_DST_PORT)    fields.l4_dst_port = value;
                else if (field == ACL_RULE_FIELD_L4_SRC_PORT_RANGE) fields.l4_src_port_range = value;
                else if (field == ACL_RULE_FIELD_L4_DST_PORT_RANGE) fields.l4_dst_port_range = value;
                else if (field == ACL_RULE_FIELD_IP_PROTOCOL)    fields.ip_proto = value;
                else if (field == ACL_RULE_FIELD_NEXT_HEADER)    fields.next_header = value;
                else if (field == ACL_RULE_FIELD_TCP_FLAGS)      fields.tcp_flags = value;
                else if (field == ACL_RULE_FIELD_DSCP)           fields.dscp = value;
                else if (field == ACL_RULE_FIELD_ETHER_TYPE)     fields.ether_type = value;
                else if (field == ACL_RULE_FIELD_VLAN_ID)        fields.vlan_id = value;
                else if (field == ACL_RULE_FIELD_IP_TYPE)        fields.ip_type = value;
                else if (field == ACL_RULE_FIELD_ICMP_TYPE)      fields.icmp_type = value;
                else if (field == ACL_RULE_FIELD_ICMP_CODE)      fields.icmp_code = value;
                else if (field == ACL_RULE_FIELD_ICMPV6_TYPE)    fields.icmpv6_type = value;
                else if (field == ACL_RULE_FIELD_ICMPV6_CODE)    fields.icmpv6_code = value;
                else if (field == ACL_RULE_FIELD_PACKET_ACTION)  fields.packet_action = value;
                else if (field == ACL_RULE_FIELD_REDIRECT_ACTION) fields.redirect_action = value;
                else if (field == ACL_RULE_FIELD_MIRROR_ACTION)  fields.mirror_action = value;
                else if (field == ACL_RULE_FIELD_MIRROR_INGRESS_ACTION) fields.mirror_ingress_action = value;
                else if (field == ACL_RULE_FIELD_MIRROR_EGRESS_ACTION)  fields.mirror_egress_action = value;
                else if (field == ACL_RULE_FIELD_POLICER_ACTION) fields.policer_action = value;
                else if (field == ACL_RULE_FIELD_DSCP_ACTION)    fields.dscp_action = value;
                else if (field == ACL_RULE_FIELD_PRIORITY)       fields.priority = (uint32_t)stoul(value);
                else                                            unknown_fields.push_back(field);
            }

            if (!unknown_fields.empty())
            {
                for (auto &uf : unknown_fields)
                    SWSS_LOG_WARN("ACL_RULE %s: unknown field '%s' not supported, marking inactive",
                                  key.c_str(), uf.c_str());
                vector<FieldValueTuple> fvs;
                fvs.emplace_back("status", "inactive");
                m_stateAclRuleTable.set(key, fvs);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Read parent table's ports + stage. */
            vector<string> interfaces_to_program;
            {
                vector<FieldValueTuple> tableFvs;
                if (m_cfgAclTable.get(table_id, tableFvs))
                {
                    for (auto &fv : tableFvs)
                    {
                        if (fvField(fv) == ACL_TABLE_FIELD_PORTS)
                            interfaces_to_program = tokenize(fvValue(fv), ',');
                        else if (fvField(fv) == ACL_TABLE_FIELD_STAGE)
                            fields.stage = fvValue(fv);
                    }
                }
            }

            SWSS_LOG_NOTICE("ACL_RULE SET: table=%s rule=%s src_ip=%s dst_ip=%s "
                            "v6=%s proto=%s ports=%s action=%s prio=%u stage=%s",
                            table_id.c_str(), rule_id.c_str(),
                            fields.src_ip.c_str(), fields.dst_ip.c_str(),
                            fields.src_ipv6.empty() ? fields.dst_ipv6.c_str() : fields.src_ipv6.c_str(),
                            fields.ip_proto.c_str(), fields.l4_src_port.c_str(),
                            fields.packet_action.c_str(), fields.priority, fields.stage.c_str());

            bool all_ok = true;
            vector<string> resolved_ifaces;
            for (auto &iface : interfaces_to_program)
            {
                string resolved = kernutil::resolveInterface(iface);
                if (!addTcFlowerFilter(resolved, fields))
                {
                    SWSS_LOG_ERROR("Failed to add tc filter on %s for rule %s",
                                   iface.c_str(), key.c_str());
                    all_ok = false;
                }
                resolved_ifaces.push_back(resolved);
            }

            AclRuleState state;
            state.priority = fields.priority;
            state.protocol = buildProtocol(fields);
            state.hook = (fields.stage == "egress") ? "egress" : "ingress";
            state.interfaces = resolved_ifaces;
            m_ruleState[key] = state;

            vector<FieldValueTuple> stateFvs;
            FieldValueTuple fv("status", all_ok ? "Active" : "Inactive");
            stateFvs.push_back(fv);
            m_stateAclRuleTable.set(key, stateFvs);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("ACL_RULE DEL: key=%s", key.c_str());

            auto state_it = m_ruleState.find(key);
            if (state_it != m_ruleState.end())
            {
                for (auto &iface : state_it->second.interfaces)
                    removeTcFlowerFilter(iface, state_it->second);
                m_ruleState.erase(state_it);
            }

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
 * getPolicerPoliceAction — resolve a referenced policer into a tc
 * "action police ..." string, mirroring mirrormgrd's resolution: the policer
 * must be active in STATE_DB POLICER_TABLE (validated by policermgrd).
 */
bool AclMgr::getPolicerPoliceAction(const string &policerName, string &policeAction)
{
    Table statePolicer(m_stateDb, "POLICER_TABLE");
    vector<FieldValueTuple> stateFvs;
    string status;
    if (statePolicer.get(policerName, stateFvs))
    {
        for (auto &fv : stateFvs)
            if (fvField(fv) == "status")
                status = fvValue(fv);
    }

    if (status != "active")
    {
        SWSS_LOG_WARN("POLICER %s not active in STATE_DB (status='%s')",
                      policerName.c_str(), status.c_str());
        return false;
    }

    Table cfgPolicer(m_cfgDb, "POLICER");
    vector<FieldValueTuple> cfgFvs;
    if (!cfgPolicer.get(policerName, cfgFvs))
    {
        SWSS_LOG_WARN("POLICER %s not found in CONFIG_DB", policerName.c_str());
        return false;
    }

    map<string, string> fields;
    for (auto &fv : cfgFvs)
        fields[fvField(fv)] = fvValue(fv);

    policeAction = kernutil::policerToTcPolice(fields);
    return !policeAction.empty();
}

/*
 * buildProtocol — determine the tc filter "protocol" (outer ethertype) for a
 * rule from its match fields.
 */
string AclMgr::buildProtocol(const AclRuleFields &f) const
{
    if (!f.ip_type.empty())
    {
        string t = kernutil::matchIpTypeToTc(f.ip_type);
        if (!t.empty())
            return t;
    }
    if (!f.src_ipv6.empty() || !f.dst_ipv6.empty() || !f.next_header.empty())
        return "ipv6";
    if (!f.vlan_id.empty())
        return "802.1Q";
    if (!f.src_ip.empty() || !f.dst_ip.empty() || !f.ip_proto.empty() ||
        !f.l4_src_port.empty() || !f.l4_dst_port.empty() || !f.dscp.empty() ||
        !f.icmp_type.empty() || !f.icmp_code.empty() || !f.tcp_flags.empty())
        return "ip";
    /* ETHER_TYPE is the outer ethertype; carry it as the filter protocol. */
    if (!f.ether_type.empty())
        return f.ether_type;
    return "all";
}

/*
 * addTcFlowerFilter — program a tc flower filter for one ACL rule.
 *
 *   tc filter add dev <iface> <hook> protocol <proto> prio <prio> flower
 *       <matches> skip_sw <actions>
 *
 * The action chain is built as police -> mirror/redirect/pedit -> packet action.
 */
bool AclMgr::addTcFlowerFilter(const string &iface, const AclRuleFields &fields)
{
    SWSS_LOG_ENTER();

    string hook = (fields.stage == "egress") ? "egress" : "ingress";
    string protocol = buildProtocol(fields);

    ostringstream qdisc_cmd;
    qdisc_cmd << TC_CMD << " qdisc add dev " << iface << " clsact";
    string ignored;
    swss::exec(qdisc_cmd.str(), ignored);

    /* delete-before-add: re-applying a rule (or updating it) must not leave a
     * duplicate filter at the same priority. */
    ostringstream del_cmd;
    del_cmd << TC_CMD << " filter del dev " << iface << " " << hook << " prio " << fields.priority;
    swss::exec(del_cmd.str(), ignored);

    /* --- match part --- */
    ostringstream match;
    match << TC_CMD << " filter add dev " << iface << " " << hook
          << " protocol " << protocol << " prio " << fields.priority << " flower";

    string srcIp = fields.src_ip;
    if (!fields.src_ip_mask.empty() && srcIp.find('/') == string::npos)
    {
        string plen = kernutil::maskToPrefixLen(fields.src_ip_mask);
        if (!plen.empty())
            srcIp += plen;
    }
    string dstIp = fields.dst_ip;
    if (!fields.dst_ip_mask.empty() && dstIp.find('/') == string::npos)
    {
        string plen = kernutil::maskToPrefixLen(fields.dst_ip_mask);
        if (!plen.empty())
            dstIp += plen;
    }

    if (!srcIp.empty())           match << " src_ip " << srcIp;
    if (!dstIp.empty())           match << " dst_ip " << dstIp;
    if (!fields.src_ipv6.empty()) match << " src_ip " << fields.src_ipv6;
    if (!fields.dst_ipv6.empty()) match << " dst_ip " << fields.dst_ipv6;
    if (!fields.vlan_id.empty())
    {
        match << " vlan_id " << fields.vlan_id;
        /* ETHER_TYPE on a VLAN-tagged rule is the inner ethertype. */
        if (!fields.ether_type.empty())
            match << " vlan_ethtype " << fields.ether_type;
    }

    string proto = !fields.ip_proto.empty() ? fields.ip_proto : fields.next_header;
    if (!proto.empty())
    {
        string proto_str = proto;
        if (proto_str == "6" || proto_str == "tcp" || proto_str == "TCP")       proto_str = "tcp";
        else if (proto_str == "17" || proto_str == "udp" || proto_str == "UDP") proto_str = "udp";
        else if (proto_str == "1" || proto_str == "icmp" || proto_str == "ICMP") proto_str = "icmp";
        else if (proto_str == "58" || proto_str == "icmpv6" || proto_str == "ICMPV6") proto_str = "icmpv6";
        else if (proto_str == "132" || proto_str == "sctp" || proto_str == "SCTP") proto_str = "sctp";
        match << " ip_proto " << proto_str;
    }

    if (!fields.l4_src_port.empty())       match << " src_port " << fields.l4_src_port;
    else if (!fields.l4_src_port_range.empty()) match << " src_port " << fields.l4_src_port_range;
    if (!fields.l4_dst_port.empty())       match << " dst_port " << fields.l4_dst_port;
    else if (!fields.l4_dst_port_range.empty()) match << " dst_port " << fields.l4_dst_port_range;

    if (!fields.tcp_flags.empty())
    {
        if (fields.tcp_flags.find("0x") == 0 || fields.tcp_flags.find("0X") == 0)
            match << " tcp_flags " << fields.tcp_flags;
        else
            match << " tcp_flags 0x" << fields.tcp_flags;
    }

    string tos = kernutil::dscpToTos(fields.dscp);
    if (!tos.empty())
        match << " ip_tos " << tos;

    string icmpType = !fields.icmp_type.empty() ? fields.icmp_type : fields.icmpv6_type;
    string icmpCode = !fields.icmp_code.empty() ? fields.icmp_code : fields.icmpv6_code;
    /* tc flower uses the bare "type"/"code" keywords for ICMP, not "icmp_type". */
    if (!icmpType.empty()) match << " type " << icmpType;
    if (!icmpCode.empty()) match << " code " << icmpCode;

    /* --- action chain --- */
    ostringstream actions;

    if (!fields.policer_action.empty())
    {
        string police;
        if (!getPolicerPoliceAction(fields.policer_action, police))
        {
            SWSS_LOG_ERROR("ACL rule: policer %s missing/inactive, skipping",
                           fields.policer_action.c_str());
            return false;
        }
        actions << police << " ";
    }

    string mirrorSession = !fields.mirror_ingress_action.empty() ? fields.mirror_ingress_action
                         : (!fields.mirror_action.empty() ? fields.mirror_action
                         : fields.mirror_egress_action);
    if (!mirrorSession.empty())
    {
        string monitor = kernutil::resolveMirrorMonitorPort(m_cfgDb, m_stateDb, mirrorSession);
        if (monitor.empty())
        {
            SWSS_LOG_ERROR("ACL rule: mirror session %s unresolved, skipping",
                           mirrorSession.c_str());
            return false;
        }
        actions << "action mirred egress mirror dev " << monitor << " ";
    }

    string redirectTarget;
    if (!fields.redirect_action.empty())
        redirectTarget = fields.redirect_action;
    else if (fields.packet_action.rfind(PACKET_ACTION_REDIRECT ":", 0) == 0)
        redirectTarget = fields.packet_action.substr(strlen(PACKET_ACTION_REDIRECT) + 1);

    if (!fields.dscp_action.empty())
    {
        string pedit = kernutil::peditSetDscpToTc(fields.dscp_action);
        if (!pedit.empty())
            actions << pedit << " ";
    }

    if (!redirectTarget.empty())
        actions << "action mirred egress redirect dev " << kernutil::resolveInterface(redirectTarget);
    else
        actions << "action " << kernutil::packetActionToTc(fields.packet_action);

    /* --- assemble + run (skip_sw first, then software fallback) --- */
    string cmd_hw = match.str() + " skip_sw " + actions.str();
    SWSS_LOG_NOTICE("Executing: %s", cmd_hw.c_str());

    string res;
    int ret = swss::exec(cmd_hw, res);
    if (ret != 0)
    {
        string cmd_sw = match.str() + " " + actions.str();
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
 * removeTcFlowerFilter — remove a tc flower filter by interface and programmed
 * state (protocol + hook + priority).
 */
bool AclMgr::removeTcFlowerFilter(const string &iface, const AclRuleState &state)
{
    SWSS_LOG_ENTER();

    ostringstream cmd;
    cmd << TC_CMD << " filter del dev " << iface
        << " " << state.hook << " protocol " << state.protocol
        << " prio " << state.priority << " flower";

    SWSS_LOG_NOTICE("Executing: %s", cmd.str().c_str());

    string res;
    int ret = swss::exec(cmd.str(), res);
    if (ret != 0)
        SWSS_LOG_WARN("tc filter del on %s prio %u (ret=%d): %s",
                      iface.c_str(), state.priority, ret, res.c_str());

    return true;
}
