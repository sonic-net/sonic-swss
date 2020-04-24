/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include "logger.h"
#include "producerstatetable.h"
#include "macaddress.h"
#include "natmgr.h"
#include "exec.h"
#include "tokenize.h"
#include "converter.h"
#include "shellcmd.h"
#include "warm_restart.h"
#include "ipaddress.h"
#include "ipprefix.h"

using namespace std;
using namespace swss;

/* NatMgr Constructor */
NatMgr::NatMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateInterfaceTable(stateDb, STATE_INTERFACE_TABLE_NAME),
        m_appNatTableProducer(appDb, APP_NAT_TABLE_NAME),
        m_appNaptTableProducer(appDb, APP_NAPT_TABLE_NAME),
        m_appTwiceNatTableProducer(appDb, APP_NAT_TWICE_TABLE_NAME),
        m_appTwiceNaptTableProducer(appDb, APP_NAPT_TWICE_TABLE_NAME),
        m_appNatGlobalTableProducer(appDb, APP_NAT_GLOBAL_TABLE_NAME),
        m_appNaptPoolIpTable(appDb, APP_NAPT_POOL_IP_TABLE_NAME)
{
    SWSS_LOG_ENTER();
  
    SWSS_LOG_INFO("NatMgr Constructor ..!");

    /* Set the Admin mode to disabled */
    natAdminMode = DISABLED;

    /* Set NAT default timeout as 600 seconds */
    m_natTimeout = NAT_TIMEOUT_DEFAULT;
    
    /* Set NAT default tcp timeout as 86400 seconds (1 Day) */
    m_natTcpTimeout = NAT_TCP_TIMEOUT_DEFAULT;

    /* Set NAT default udp timeout as 300 seconds */
    m_natUdpTimeout = NAT_UDP_TIMEOUT_DEFAULT;

    /* Clean the NAT iptables */
    std::string res;
    const std::string cmds = std::string("") + IPTABLES_CMD + " -F -t nat ";
    if (swss::exec(cmds, res))
    {
        SWSS_LOG_ERROR("Command '%s' failed", cmds.c_str());
    }

    flushNotifier = std::make_shared<swss::NotificationProducer>(appDb, "FLUSHNATREQUEST");
}

/* To check the port init id done or not */
bool NatMgr::isPortInitDone(DBConnector *app_db)
{
    bool portInit = 0;
    long cnt = 0;

    while(!portInit) {
        Table portTable(app_db, APP_PORT_TABLE_NAME);
        std::vector<FieldValueTuple> tuples;
        portInit = portTable.get("PortInitDone", tuples);

        if(portInit)
            break;
        sleep(1);
        cnt++;
    }
    SWSS_LOG_NOTICE("PORT_INIT_DONE : %d %ld", portInit, cnt);
    return portInit;
}

/* To check the given port is State Ok or not */
bool NatMgr::isPortStateOk(const string &port)
{
    vector<FieldValueTuple> temp;

    if (!port.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(port, temp))
        {
            SWSS_LOG_INFO("Vlan %s is ready", port.c_str());
            return true;
        }
        SWSS_LOG_INFO("Vlan %s is not yet ready", port.c_str());
    }
    else if (!port.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(port, temp))
        {
            SWSS_LOG_INFO("Lag %s is ready", port.c_str());
            return true;
        }
        SWSS_LOG_INFO("Lag %s is not yet ready", port.c_str());
    }
    else if (!port.compare(0, strlen(ETHERNET_PREFIX), ETHERNET_PREFIX))
    {
        if (m_statePortTable.get(port, temp))
        {
            SWSS_LOG_INFO("Port %s is ready", port.c_str());
            return true;
        }
        SWSS_LOG_INFO("Port %s is not yet ready", port.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("Invalid Port %s ", port.c_str());
    }
    return false;
}

/* To check the give interface is State Ok or not */
bool NatMgr::isIntfStateOk(const string &interface)
{
    vector<FieldValueTuple> temp;

    if (m_stateInterfaceTable.get(interface, temp))
    {
        SWSS_LOG_INFO("Interface %s is ready", interface.c_str());
        return true;
    }

    SWSS_LOG_INFO("Interface %s is not yet ready", interface.c_str());
    return false;
}

/* To check the nat fetaure is enabled or not */
bool NatMgr::isNatEnabled(void)
{
    if (natAdminMode == ENABLED)
    {
        return true;
    }

    return false;
}

/* To check the give global_ip is withing the Prefix subnet or not */
bool NatMgr::isGlobalIpMatching(const string &prefix, const string &global_ip)
{
    IpAddress externalAddr(global_ip);
    IpPrefix ip_prefix(prefix);

    auto ea = externalAddr.getIp();
    auto ia = ip_prefix.getIp();
    auto ia2 = ia.getIp();
    auto ma = ip_prefix.getMask();
    auto ma2 = ma.getIp();

    /* Check global ip is within the given subnet */
    if ((ia2.ip_addr.ipv4_addr & ma2.ip_addr.ipv4_addr) == (ea.ip_addr.ipv4_addr & ma2.ip_addr.ipv4_addr))
    {
        return true;
    }

    return false;
}

/* To check the given pool_name is mapped to any binding or not */
bool NatMgr::isPoolMappedtoBinding(const string &pool_name, string &binding_name)
{
    /* Get all binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        /* Check is it matches with given pool name */
        if (pool_name == (*it).second.pool_name)
        {
            /* Pool is mapped to Binding, return now */
            binding_name = (*it).first;
            return true;
        }
    }

    return false;
}

/* To check the given Static NAT entry is matched with any Static NAPT entry */
bool NatMgr::isMatchesWithStaticNapt(const string &global_ip, string &local_ip)
{
    /* Get all Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<string> keys = tokenize((*it).first, config_db_key_delimiter);

        /* Ensure that interface is having some port, otherwise Entry is not yet added */
        if ((keys[0] == global_ip) && ((*it).second.local_ip == local_ip) &&
            ((*it).second.interface != NONE_STRING))
        {
            /* Matches with the Static NAPT entry */
            return true;
        }
    }
    return false;
}

/* To check the given Static NAPT entry is matched with any Static NAT entry */
bool NatMgr::isMatchesWithStaticNat(const string &global_ip, string &local_ip)
{
    /* Get all Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        /* Ensure that interface is having some port, otherwise Entry is not yet added */
        if (((*it).first == global_ip) && ((*it).second.local_ip == local_ip) &&
            ((*it).second.interface != NONE_STRING))
        {
            /* Matches with the Static NAT entry */
            return true;
        }
    }
    return false;
}

/* To get the configured interface for the given global ip address */
bool NatMgr::getIpEnabledIntf(const string &global_ip, string &interface)
{
    /* Get all Ports from Ip Interface Info */
    for (auto it = m_natIpInterfaceInfo.begin(); it != m_natIpInterfaceInfo.end(); it++)
    {
        /* Get all IpAddress from Ip Interface Values from the key (Port) */
        for (auto ipPrefix = m_natIpInterfaceInfo[(*it).first].begin(); ipPrefix != m_natIpInterfaceInfo[(*it).first].end(); ipPrefix++)
        {
            /* Check the global ip address is in subnet */
            if (isGlobalIpMatching(*ipPrefix, global_ip) == true)
            {
                /* Matched with this interface, return now */
                interface = (*it).first;
                return true;
            }
        }
    }

    return false;
}

/* This is ideally called on docker stop  */
void NatMgr::cleanupPoolIpTable(void)
{
    SWSS_LOG_INFO("Cleaning the NAPT Pool IP table from APP_DB");
    for (auto it = m_natPoolInfo.begin(); it != m_natPoolInfo.end(); it++)
    {
        /* Delete pool ip from APPL_DB */
        setNaptPoolIpTable(DELETE, ((*it).second).ip_range, ((*it).second).port_range);
    }
}

/* This is ideally called on docker stop  */
void NatMgr::cleanupMangleIpTables(void)
{
    SWSS_LOG_INFO("Cleaning the Mangle IpTables");
    for (auto it = m_natZoneInterfaceInfo.begin(); it != m_natZoneInterfaceInfo.end(); it++)
    {
        /* Delete the mangle iptables rules for non-loopback interface */
        if (strncmp((*it).first.c_str(), LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX)))
        {
            setMangleIptablesRules(DELETE, (*it).first, (*it).second);
        }
    }
}

/* To Add/Delete NAPT pool ip table to APPL_DB */
void NatMgr::setNaptPoolIpTable(const string &opCmd, const string &ip_range, const string &port_range)
{
    uint32_t ipv4_addr_low, ipv4_addr_high, ip, setIp;
    char ipAddr[INET_ADDRSTRLEN];
    std::vector<swss::FieldValueTuple> values;

    if (!port_range.empty() and (port_range != "NULL"))
    {
        swss::FieldValueTuple p("port_range", port_range);
        values.push_back(p);
        vector<string> nat_ip = tokenize(ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool is not valid");
            return;
        }
        else if (nat_ip.size() == 2)
        {
            inet_pton(AF_INET, nat_ip[1].c_str(), &ipv4_addr_high);
            ipv4_addr_high = ntohl(ipv4_addr_high);
            inet_pton(AF_INET, nat_ip[0].c_str(), &ipv4_addr_low);
            ipv4_addr_low = ntohl(ipv4_addr_low);
        }
        else
        {
            inet_pton(AF_INET, nat_ip[0].c_str(), &ipv4_addr_low);
            ipv4_addr_high = ntohl(ipv4_addr_low);
            ipv4_addr_low = ntohl(ipv4_addr_low);
        }

        for (ip = ipv4_addr_low; ip <= ipv4_addr_high; ip++)
        {
            setIp = htonl(ip);
            inet_ntop(AF_INET, &setIp, ipAddr, INET_ADDRSTRLEN);
            if (opCmd == ADD)
            {
                m_appNaptPoolIpTable.set(ipAddr, values);
            }
            else
            {
                m_appNaptPoolIpTable.del(ipAddr);
            }
        }
    }
}

/* To Add a dummy conntrack entry for the Static Single NAT entry in the kernel */
void NatMgr::addConntrackSingleNatEntry(const string &key)
{
    std::string res, cmds = std::string("") + CONNTRACK_CMD; 

    if (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE)
    {
        SWSS_LOG_INFO("Add static NAT conntrack entry with src-ip %s, timeout %d",
                      m_staticNatEntry[key].local_ip.c_str(), m_natTimeout);

        cmds += (" -I -n " + key + ":1 -g 127.0.0.1:127" + " -p udp -t " + to_string(m_natTimeout) +
                 " --src " + m_staticNatEntry[key].local_ip + " --sport 1 --dst 127.0.0.1 --dport 127 -u ASSURED ");
    }
    else if (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        SWSS_LOG_INFO("Add static NAT conntrack entry with src-ip %s, timeout %d",
                      key.c_str(), m_natTimeout);

        cmds += (" -I -n " + m_staticNatEntry[key].local_ip + ":1 -g 127.0.0.1:127" + " -p udp -t " + to_string(m_natTimeout) +
                 " --src " + key + " --sport 1 --dst 127.0.0.1 --dport 127 -u ASSURED ");
    }

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Added the static NAT conntrack entry");
    }
}

/* To Add a dummy conntrack entry for the Static Twice NAT entry in the kernel */
void NatMgr::addConntrackTwiceNatEntry(const string &snatKey, const string &dnatKey)
{
    std::string res, cmds = std::string("") + CONNTRACK_CMD;

    SWSS_LOG_INFO("Add static Twice NAT conntrack entry with src-ip %s, dst-ip %s, timeout %u",
                  snatKey.c_str(), dnatKey.c_str(), m_natTimeout);

    cmds += (" -I -n " + m_staticNatEntry[snatKey].local_ip + ":1" + " -g " + m_staticNatEntry[dnatKey].local_ip + ":1"
             +  " -p udp" + " -t " + to_string(m_natTimeout) + " --src " + snatKey + " --sport 1" + " --dst " + dnatKey
             +  " --dport 1" + " -u ASSURED ");

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Added the static Twice NAT conntrack entry");
    }
}

/* To Add a dummy conntrack entry for the Static NAPT entry in the kernel,
 * so that the port number is reserved and the same port is not allocated by the stack for any other dynamic entry */
void NatMgr::addConntrackSingleNaptEntry(const string &key)
{
    int timeout = 0;
    std::string res, prototype, state, cmds = std::string("") + CONNTRACK_CMD;
    vector<string> keys = tokenize(key, config_db_key_delimiter);

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
        timeout = m_natUdpTimeout;
        state = "";
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
        timeout = m_natTcpTimeout;
        state = " --state ESTABLISHED ";
    }

    if (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE)
    {

        SWSS_LOG_INFO("Add static NAPT conntrack entry with protocol %s, src-ip %s, src-port %s, timeout %d",
                      prototype.c_str(), m_staticNaptEntry[key].local_ip.c_str(), m_staticNaptEntry[key].local_port.c_str(), timeout);

        cmds += (" -I -n " + keys[0] + ":" + keys[2] + " -g 127.0.0.1:127" + " -p " + prototype + " -t " + to_string(timeout) +
                 " --src " + m_staticNaptEntry[key].local_ip + " --sport " + m_staticNaptEntry[key].local_port + " --dst 127.0.0.1 --dport 127 -u ASSURED " + state);
    }
    else if (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        SWSS_LOG_INFO("Add static NAPT conntrack entry with protocol %s, src-ip %s, src-port %s, timeout %d",
                      prototype.c_str(), keys[0].c_str(), keys[2].c_str(), timeout);

        cmds += (" -I -n " + m_staticNaptEntry[key].local_ip + ":" + m_staticNaptEntry[key].local_port + " -g 127.0.0.1:127" + " -p " + prototype + " -t " + to_string(timeout) +
                 " --src " + keys[0] + " --sport " + keys[2] + " --dst 127.0.0.1 --dport 127 -u ASSURED " +  state);
    }

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Added the static NAPT conntrack entry");
    }
}

/* To Add a dummy conntrack entry for the Static Twice NAPT entry in the kernel */
void NatMgr::addConntrackTwiceNaptEntry(const string &snatKey, const string &dnatKey)
{
    int timeout = 0;
    std::string res, prototype, state, cmds = std::string("") + CONNTRACK_CMD;
    vector<string> snatKeys = tokenize(snatKey, config_db_key_delimiter);
    vector<string> dnatKeys = tokenize(dnatKey, config_db_key_delimiter);

    if (snatKeys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
        timeout = m_natUdpTimeout;
        state = "";
    }
    else if (snatKeys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
        timeout = m_natTcpTimeout;
        state = " --state ESTABLISHED ";
    }

    SWSS_LOG_DEBUG("Add static Twice NAPT conntrack entry with protocol %s, src-ip %s, src-port %s, dst-ip %s, dst-port %s, timeout %u",
                   prototype.c_str(), snatKeys[0].c_str(), snatKeys[2].c_str(), dnatKeys[0].c_str(), dnatKeys[2].c_str(), timeout);

    cmds += (" -I -n " + m_staticNaptEntry[snatKey].local_ip + ":" + m_staticNaptEntry[snatKey].local_port + " -g " + m_staticNaptEntry[dnatKey].local_ip + ":"
             + m_staticNaptEntry[dnatKey].local_port +  " -p " + prototype + " -t " + to_string(timeout)
             + " --src " + snatKeys[0] + " --sport " + snatKeys[2] + " --dst " + dnatKeys[0] + " --dport " + dnatKeys[2] + " -u ASSURED " +  state);

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Added the static Twice NAPT conntrack entry");
    }
}

/* To Delete conntrack entry for Static Single NAT entry */
void NatMgr::deleteConntrackSingleNatEntry(const string &key)
{
    std::string res, cmds = std::string("") + CONNTRACK_CMD;

    if (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE)
    {
        SWSS_LOG_INFO("Delete static NAT conntrack entry with src-ip %s", m_staticNatEntry[key].local_ip.c_str());

        cmds += (" -D -s " + m_staticNatEntry[key].local_ip + " -p udp" + " &> /dev/null");
    }
    else if (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        SWSS_LOG_INFO("Delete static NAT conntrack entry with src-ip %s", key.c_str());

        cmds += (" -D -s " + key + " -p udp" + " &> /dev/null");
    }

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Deleted the Static NAT conntrack entry");
    }
}

/* To Delete conntrack entry for Static Twice NAT entry */
void NatMgr::deleteConntrackTwiceNatEntry(const string &snatKey, const string &dnatKey)
{
    std::string res, cmds = std::string("") + CONNTRACK_CMD;

    SWSS_LOG_INFO("Delete static Twice NAT conntrack entry with src-ip %s and dst-ip %s", snatKey.c_str(), dnatKey.c_str());

    cmds += (" -D -s " + snatKey + " -d " + dnatKey + " &> /dev/null");

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Deleted the Static Twice NAT conntrack entry");
    }
}

/* To Delete conntrack entry for Static Single NAPT entry */
void NatMgr::deleteConntrackSingleNaptEntry(const string &key)
{
    std::string res, prototype, cmds = std::string("") + CONNTRACK_CMD;
    vector<string> keys = tokenize(key, config_db_key_delimiter);

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    if (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE)
    {
        SWSS_LOG_INFO("Delete static NAPT conntrack entry with protocol %s, src-ip %s, src-port %s",
                      prototype.c_str(), m_staticNaptEntry[key].local_ip.c_str(), m_staticNaptEntry[key].local_port.c_str());

        cmds += (" -D -s " + m_staticNaptEntry[key].local_ip + " -p " + prototype + " --sport " + m_staticNaptEntry[key].local_port + " &> /dev/null");
    }
    else if (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        SWSS_LOG_INFO("Delete static NAPT conntrack entry with protocol %s, src-ip %s, src-port %s",
                      prototype.c_str(), keys[0].c_str(), keys[2].c_str());

        cmds += (" -D -s " + keys[0] + " -p " + prototype + " --sport " + keys[2] + " &> /dev/null");
    }

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Deleted the Static NAPT conntrack entry");
    }
}

/* To Delete conntrack entry for Static Twice NAPT entry */
void NatMgr::deleteConntrackTwiceNaptEntry(const string &snatKey, const string &dnatKey)
{
    std::string res, prototype, cmds = std::string("") + CONNTRACK_CMD;
    vector<string> snatKeys = tokenize(snatKey, config_db_key_delimiter);
    vector<string> dnatKeys = tokenize(dnatKey, config_db_key_delimiter);

    if (snatKeys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (snatKeys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    SWSS_LOG_INFO("Delete static Twice NAPT conntrack entry with protocol %s, src-ip %s, src-port %s, dst-ip %s, dst-port %s",
                  prototype.c_str(), snatKeys[0].c_str(), snatKeys[2].c_str(), dnatKeys[0].c_str(), dnatKeys[2].c_str());

    cmds += (" -D -s " + snatKeys[0] + " -p " + prototype + " --orig-port-src " + snatKeys[2] + " -d " + dnatKeys[0] + " --orig-port-dst " + dnatKeys[2] + " &> /dev/null");

    int ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }
    else
    {
        SWSS_LOG_INFO("Deleted the Static Twice NAPT conntrack entry");
    }
}

/* To Delete conntrack entries for matching Pool ip address */
void NatMgr::deleteConntrackDynamicEntries(const string &ip_range)
{
    std::string res, cmds;

    uint32_t ipv4_addr_low, ipv4_addr_high, ip, setIp;
    char ipAddr[INET_ADDRSTRLEN];

    vector<string> nat_ip = tokenize(ip_range, range_specifier);

    /* Check the pool is valid */
    if (nat_ip.empty())
    {
        SWSS_LOG_INFO("NAT pool is not valid");
        return;
    }
    else if (nat_ip.size() == 2)
    {
        inet_pton(AF_INET, nat_ip[1].c_str(), &ipv4_addr_high);
        ipv4_addr_high = ntohl(ipv4_addr_high);
        inet_pton(AF_INET, nat_ip[0].c_str(), &ipv4_addr_low);
        ipv4_addr_low = ntohl(ipv4_addr_low);
    }
    else
    {
        inet_pton(AF_INET, nat_ip[0].c_str(), &ipv4_addr_low);
        ipv4_addr_high = ntohl(ipv4_addr_low);
        ipv4_addr_low = ntohl(ipv4_addr_low);
    }

    for (ip = ipv4_addr_low; ip <= ipv4_addr_high; ip++)
    {
        setIp = htonl(ip);
        inet_ntop(AF_INET, &setIp, ipAddr, INET_ADDRSTRLEN);
        std::string ipAddrString(ipAddr);

        SWSS_LOG_INFO("Delete dynamic conntrack entry with translated-src-ip %s", ipAddr);

        cmds = (std::string("") + CONNTRACK_CMD + " -D -q " + ipAddrString + " &> /dev/null");

        int ret = swss::exec(cmds, res);

        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
        }
        else
        {
            SWSS_LOG_INFO("Deleted the dynamic conntrack entry");
        }
    }
}

/* Iptable rules are added in the mangles table, to support use of Loopback IP as NAT Public IP which is a typical use-case in DC scenarios. The way it works is that:
 *
 * *	The mangle table rules are processed first before the nat table rules.
 * *	Assign mark field on the packet using the mangles rules in the PREROUTING (ingress) and POSTROUTING (egress) stages.
 * *	The mark field is derived from the configured zone (zone + 1). Using 'mark'value of 0 has issues as that is implicit value for any packet traversing the kernel.
 * *	Match against the 'mark' value in the nat rules happens after the 'mangle' rule sets it.
 * *	Since packet doesn't go out of a Loopback interface, we configure zone value on the public interfaces same as on the Loopback interface (whose IP is used as NAT public IP).
 * *	So matching against the zone value is done while allocating NAT IPs.
 * *
 * * */
bool NatMgr::setMangleIptablesRules(const string &opCmd, const string &interface, const string &nat_zone)
{
    SWSS_LOG_ENTER();

    /* The command should be generated as:
     * iptables -t mangle -opCmd PREROUTING -i port -j MARK --set-mark nat_zone
     * iptables -t mangle -opCmd POSTROUTING -o port -j MARK --set-mark nat_zone
     */
    std::string res;
    int ret;

    const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t mangle " + "-" + opCmd + " PREROUTING -i " + interface + " -j MARK --set-mark " + nat_zone + " && "
          + IPTABLES_CMD + " -t mangle " + "-" + opCmd + " POSTROUTING -o " + interface + " -j MARK --set-mark " + nat_zone ;

    ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
        return false;
    }

    return true;
}

/* To Add arbitrary value for DNAT rule incase of fullcone */
bool NatMgr::setFullConeDnatIptablesRule(const string &opCmd)
{
    /* This rule in the PREROUTING chain should be the default rule at the end of the list
     * iptables -t nat -[A/D] PREROUTING -j DNAT --fullcone
     */
    std::string res;
    int ret;

    /* In case of fullcone, the --to-destination is ignored by the stack, giving an aribitrary value so that 
     * iptables doesn't fail for PREROUTING/DNAT rule */
    const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING " + " -j DNAT --to-destination 1.1.1.1 --fullcone";
        
    ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
        return false;
    }
    return true;
}

/* To Add or Delete the Iptables rules for Static NAT entry */
bool NatMgr::setStaticNatIptablesRules(const string &opCmd, const string &interface, const string &external_ip, const string &internal_ip, const string &nat_type)
{
    SWSS_LOG_ENTER();

    /* The command should be generated as:
     * iptables -t nat -opCmd PREROUTING -m mark --mark zone-value -j DNAT -d external_ip --to-destination internal_ip
     * iptables -t nat -opCmd POSTROUTING -m mark --mark zone-value -j SNAT -s internal_ip --to-source external_ip
     */
    std::string res;
    std::string markStr = std::string("");
    int ret;

    markStr = " -m mark --mark " + m_natZoneInterfaceInfo[interface];

    if (nat_type == DNAT_NAT_TYPE)
    {
        const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING " + markStr + " -j DNAT -d " + external_ip + " --to-destination " + internal_ip + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + markStr + " -j SNAT -s " + internal_ip + " --to-source " + external_ip ;
        
        ret = swss::exec(cmds, res);

        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
            return false;
        }
    }
    else
    {
        const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING" + " -j DNAT -d " + internal_ip + " --to-destination " + external_ip + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING" + " -j SNAT -s " + external_ip + " --to-source " + internal_ip ;

        ret = swss::exec(cmds, res);

        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
            return false;
        }
    }

    return true;
}

/* To Add or Delete the Iptables rules for Static NAPT entry */
bool NatMgr::setStaticNaptIptablesRules(const string &opCmd, const string &interface, const string &prototype, const string &external_ip, 
                                        const string &external_port, const string &internal_ip, const string &internal_port, const string &nat_type)
{
    SWSS_LOG_ENTER();

    /* The command should be generated as:
     * iptables -t nat -opCmd PREROUTING -m mark --mark zone-value -p prototype -j DNAT -d external_ip --dport external_port --to-destination internal_ip:internal_port
     * iptables -t nat -opCmd POSTROUTING -m mark --mark zone-value -p prototype -j SNAT -s internal_ip --sport internal_port --to-source external_ip:external_port
     */
    std::string res;
    std::string markStr = std::string("");
    int ret;

    markStr = " -m mark --mark " + m_natZoneInterfaceInfo[interface];

    if (nat_type == DNAT_NAT_TYPE)
    {
        const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING " + markStr + " -p " + prototype + " -j DNAT -d " + external_ip + " --dport " + external_port + " --to-destination " 
          + internal_ip + ":" + internal_port + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + markStr + " -p " + prototype + " -j SNAT -s " + internal_ip + " --sport " + internal_port + " --to-source " 
          + external_ip + ":" + external_port;

        ret = swss::exec(cmds, res);

        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
            return false;
        }
    }
    else
    {
        const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING" + " -p " + prototype + " -j DNAT -d " + internal_ip + " --dport " + internal_port + " --to-destination "
          + external_ip + ":" + external_port + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING" + " -p " + prototype + " -j SNAT -s " + external_ip + " --sport " + external_port + " --to-source "
          + internal_ip + ":" + internal_port;

        ret = swss::exec(cmds, res);

        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
            return false;
        }
    }

    return true;
}

/* To Add or Delete the Iptables rules for Static Twice NAT entry */
bool NatMgr::setStaticTwiceNatIptablesRules(const string &opCmd, const string &interface, const string &src_ip, const string &translated_src_ip,
                                            const string &dest_ip, const string &translated_dest_ip)
{
    SWSS_LOG_ENTER();

    /* The command should be generated as:
     * iptables -t nat -opCmd PREROUTING -j DNAT -d translated_src --to-destination src -s translated_dst   
     * iptables -t nat -opCmd PREROUTING -m mark --mark zone-value -j DNAT -d dst --to-destination translated_dst -s src
     *
     * iptables -t nat -opCmd POSTROUTING -j SNAT -s src --to-source translated_src -d translated_dst
     * iptables -t nat -opCmd POSTROUTING -m mark --mark zone-value -j SNAT -s translated_dst --to-source dst -d src 
     */

    std::string res;
    std::string markStr = std::string("");
    int ret;

    markStr = " -m mark --mark " + m_natZoneInterfaceInfo[interface];

    const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING -j DNAT -d " + translated_src_ip
          + " --to-destination " + src_ip + " -s " + translated_dest_ip + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING " + markStr + " -j DNAT -d " + dest_ip
          + " --to-destination " + translated_dest_ip + " -s " + src_ip + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -j SNAT -s " + src_ip
          + " --to-source " + translated_src_ip + " -d " + translated_dest_ip + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + markStr + " -j SNAT -s " + translated_dest_ip
          + " --to-source " + dest_ip + " -d " + src_ip;

    ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
        return false;
    }

    return true;
}

/* To Add or Delete the Iptables rules for Static Twice NAPT entry */
bool NatMgr::setStaticTwiceNaptIptablesRules(const string &opCmd, const string &interface, const string &prototype, const string &src_ip, const string &src_port,
                                             const string &translated_src_ip, const string &translated_src_port, const string &dest_ip, const string &dest_port,
                                             const string &translated_dest_ip, const string &translated_dest_port)
{
    SWSS_LOG_ENTER();

    /* The command should be generated as:
     * iptables -t nat -opCmd PREROUTING -j DNAT -p udp -d translated_src --dport translated_src_l4_port --to-destination src:src_l4_port
     * -s translated_dst --sport translated_dst_l4_port
     * iptables -t nat -opCmd PREROUTING -m mark --mark zone-value -j DNAT -p udp -d dst --dport dst_l4_port --to-destination translated_dst:translated_dst_l4_port
     * -s src --sport src_l4_port
     *
     * iptables -t nat -opCmd POSTROUTING -j SNAT -p udp -s src --sport src_l4_port --to-source translated_src:translated_src_l4_port
     * -d translated_dst --dport translated_dst_l4_port
     * iptables -t nat -opCmd POSTROUTING -m mark --mark zone-value -j SNAT -p udp -s translated_dst --sport translated_dst_l4_port --to-source dst:dst_l4_port
     * -d src --dport src_l4_port
     */

    std::string res;
    std::string markStr = std::string("");
    int ret;

    markStr = " -m mark --mark " + m_natZoneInterfaceInfo[interface];

    const std::string cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING -p " + prototype + " -j DNAT -d " + translated_src_ip + " --dport " + translated_src_port 
          + " --to-destination " + src_ip + ":" + src_port + " -s " + translated_dest_ip + " --sport " + translated_dest_port + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " PREROUTING " + markStr + " -p " + prototype + " -j DNAT -d " + dest_ip + " --dport " + dest_port
          + " --to-destination " + translated_dest_ip + ":" + translated_dest_port + " -s " + src_ip + " --sport " + src_port + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p " + prototype + " -j SNAT -s " + src_ip + " --sport " + src_port
          + " --to-source " + translated_src_ip + ":" + translated_src_port + " -d " + translated_dest_ip + " --dport " + translated_dest_port + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + markStr + " -p " + prototype + " -j SNAT -s " + translated_dest_ip + " --sport " + translated_dest_port
          + " --to-source " + dest_ip + ":" + dest_port + " -d " + src_ip + " --dport " +src_port;

    ret = swss::exec(cmds, res);

    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
        return false;
    }

    return true;
}

/* To Add or Delete the Iptables rules for Dynamic NAT/NAPT without ACLs */
bool NatMgr::setDynamicNatIptablesRulesWithoutAcl(const string &opCmd, const string &interface, const string &external_ip,
                                                  const string &external_port_range, const string &key)
{
    SWSS_LOG_ENTER();

    /* The command should be generated as:
     *
     * iptables -t nat -opCmd POSTROUTING -p tcp -j SNAT -m mark --mark zone-value --to-source external_ip:external_port_range --fullcone
     * iptables -t nat -opCmd POSTROUTING -p udp -j SNAT -m mark --mark zone-value --to-source external_ip:external_port_range --fullcone
     * iptables -t nat -opCmd POSTROUTING -p icmp -j SNAT -m mark --mark zone-value --to-source external_ip:external_port_range --fullcone
     */
    std::string res, cmd;
    std::string externalString = EMPTY_STRING;
    std::string fullcone = EMPTY_STRING;
    std::string prototype = EMPTY_STRING;
    std::string cmds = std::string("");
    std::string markStr = std::string("");

    markStr = " -m mark --mark " + m_natZoneInterfaceInfo[interface];

    if (external_port_range.empty())
    {
        externalString = external_ip;
    }
    else
    {
        externalString = external_ip + ":" + external_port_range;
        fullcone = " --fullcone";
    }

    /* Static Key empty means Single NAT */
    if (key.empty())
    {
        /* Rules for Single NAT */
        cmds = std::string("")
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p tcp -j SNAT " + markStr + " --to-source " 
          + externalString + fullcone + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p udp -j SNAT " + markStr + " --to-source " 
          + externalString + fullcone + " && "
          + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p icmp -j SNAT " + markStr + " --to-source " 
          + externalString + fullcone;
    }
    else
    {
        if (opCmd == ADD)
        {
            cmd = INSERT;
        }
        else
        {
            cmd = opCmd;
        }

        vector<string> keys = tokenize(key, config_db_key_delimiter);
        if (keys.size() > 1)
        {
            if (keys[1] == to_upper(IP_PROTOCOL_UDP))
            {
                prototype = " -p udp ";
            }
            else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
            {
                prototype = " -p tcp ";
            }

            /* Rules for Double NAT */
            cmds = std::string("")
              + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + prototype + " -j SNAT " + markStr + " --to-source "
              + externalString + " -d " + keys[0] + " --dport " + keys[2] + fullcone + " && "
              + IPTABLES_CMD + " -t nat " + "-" + cmd + " PREROUTING " + prototype + " -j DNAT -d " + m_staticNaptEntry[key].local_ip + " --dport "
              + m_staticNaptEntry[key].local_port + " --to-destination " + keys[0] + ":" + keys[2] + " && "
              + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + prototype + " -j SNAT -s " + keys[0] + " --sport "
              + keys[2] + " --to-source " + m_staticNaptEntry[key].local_ip + ":" + m_staticNaptEntry[key].local_port;
        }
        else
        {   
            /* Rules for Double NAT */ 
            cmds = std::string("")
              + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + prototype + " -j SNAT " + markStr + " --to-source "
              + externalString + " -d " + key + fullcone + " && "
              + IPTABLES_CMD + " -t nat " + "-" + cmd + " PREROUTING" + " -j DNAT -d " + m_staticNatEntry[key].local_ip + " --to-destination " + key + " && "
              + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING" + " -j SNAT -s " + key + " --to-source " + m_staticNatEntry[key].local_ip ;
        }
    }

    int ret = swss::exec(cmds, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
        return false;
    }

    return true;
}

/* To Add or Delete the Iptables rules for Dynamic NAT/NAPT with ACLs */
bool NatMgr::setDynamicNatIptablesRulesWithAcl(const string &opCmd, const string &interface, const string &external_ip,
                                               const string &external_port_range, natAclRule_t &natAclRuleId,
                                               const string &key)
{
    SWSS_LOG_ENTER();

    /* The command should be generated as: for example
     *
     * iptables -t nat -opCmd POSTROUTING -p tcp -s srcIpAddress -j RETURN
     * iptables -t nat -opCmd POSTROUTING -p udp -s srcIpAddress -j RETURN
     * iptables -t nat -opCmd POSTROUTING -p icmp -s srcIpAddress -j RETURN
     *
     * iptables -t nat -opCmd POSTROUTING -p tcp srcIpAddressString -j SNAT -m mark --mark zone-value --to-source external_ip:external_port_range --fullcone
     * iptables -t nat -opCmd POSTROUTING -p udp srcIpAddressString -j SNAT -m mark --mark zone-value --to-source external_ip:external_port_range --fullcone
     * iptables -t nat -opCmd POSTROUTING -p icmp srcIpAddressString -j SNAT -m mark --mark zone-value --to-source external_ip:external_port_range --fullcone
     */

    std::string res, cmd;
    std::string srcIpAddressString = EMPTY_STRING, dstIpAddressString = EMPTY_STRING;
    std::string srcPortString = EMPTY_STRING, dstPortString = EMPTY_STRING;
    std::string externalString = EMPTY_STRING, fullcone = EMPTY_STRING;
    std::string prototype = EMPTY_STRING;
    std::string cmds = std::string("");
    vector<string> keys;
    std::string markStr = std::string("");

    markStr = " -m mark --mark " + m_natZoneInterfaceInfo[interface];

    if (external_port_range.empty())
    {
        externalString = external_ip;
    }
    else
    {
        externalString = external_ip + ":" + external_port_range;
        fullcone = " --fullcone";
    }

    if (natAclRuleId.src_ip_range != "None")
    {
        srcIpAddressString = " -s " + natAclRuleId.src_ip_range;
    }

    if (natAclRuleId.dst_ip_range != "None")
    {
        dstIpAddressString = " -d " + natAclRuleId.dst_ip_range;
    }

    if (natAclRuleId.src_l4_port_range != "None")
    {
        srcPortString = " --sport " + natAclRuleId.src_l4_port_range;
    }

    if (natAclRuleId.dst_l4_port_range != "None")
    {
        dstPortString = " --dport " + natAclRuleId.dst_l4_port_range;
    }

    /* Static Key not empty means Double NAT */
    if (!key.empty())
    { 
        /* Destination IP/Port address from ACL Rule not valid case for Double NAT */
        if (!dstIpAddressString.empty() or !dstPortString.empty())
        {
            SWSS_LOG_WARN("Destination IP/Port is not valid for Twice NAT, skipped adding the ACL Rule");
            return true;
        }

        keys = tokenize(key, config_db_key_delimiter);
        if (keys.size() > 1)
        {
            /* Protocol from ACL Rule is not matching with static entry for Double NAT */
            if ((natAclRuleId.ip_protocol != "None") and (natAclRuleId.ip_protocol != keys[1]))
            {
                SWSS_LOG_WARN("Rule protocol %s is not matching with Static entry, skipped adding the ACL Rule", natAclRuleId.ip_protocol.c_str());
                return true;
            }

            if (keys[1] == to_upper(IP_PROTOCOL_UDP))
            {
                prototype = " -p udp ";
            }
            else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
            {
                prototype = " -p tcp ";
            }
        }
        if (opCmd == ADD)
        {
            cmd = INSERT;
        }
        else
        {
            cmd = opCmd;
        }
    }

    if (natAclRuleId.packet_action == PACKET_ACTION_DO_NOT_NAT)
    {
        /* Rule are for all ip protocols */
        if (natAclRuleId.ip_protocol == "None")
        {
            /* Static Key empty means Single NAT */
            if (key.empty())
            {
                /* Rules for Single NAT */
                cmds = std::string("")
                   + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p tcp" + srcIpAddressString + dstIpAddressString 
                   + srcPortString + dstPortString + " -j RETURN" + " && "
                   + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p udp" + srcIpAddressString + dstIpAddressString
                   + srcPortString + dstPortString + " -j RETURN" + " && "
                   + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p icmp" + srcIpAddressString + dstIpAddressString
                   + srcPortString + dstPortString + " -j RETURN";
            }
            else
            {
                /* Rules for Double NAT */
                if (keys.size() > 1)
                {
                    cmds = std::string("")
                       + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p tcp" + srcIpAddressString + " -d " + keys[0]
                       + srcPortString + " --dport " + keys[2] + " -j RETURN" + " && "
                       + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p udp" + srcIpAddressString + " -d " + keys[0]
                       + srcPortString + " --dport " + keys[2] + " -j RETURN" + " && "
                       + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p icmp" + srcIpAddressString + " -d " + keys[0]
                       + srcPortString + " --dport " + keys[2] + " -j RETURN";
                }
                else
                {
                    cmds = std::string("")
                       + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p tcp" + srcIpAddressString + " -d " + keys[0]
                       + srcPortString + " -j RETURN" + " && "
                       + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p udp" + srcIpAddressString + " -d " + keys[0]
                       + srcPortString + " -j RETURN" + " && "
                       + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p icmp" + srcIpAddressString + " -d " + keys[0]
                       + srcPortString + " -j RETURN";
                }

            }
        }
        else
        {
            /* Static Key empty means Single NAT */
            if (key.empty())
            {
                /* Rule for Single NAT */
                cmds = std::string("")
                  + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p " + natAclRuleId.ip_protocol + srcIpAddressString
                  + dstIpAddressString + srcPortString + dstPortString + " -j RETURN";
            }
            else
            {
                if (keys.size() > 1)
                {
                    /* Rules for Double NAT */
                    cmds = std::string("")
                      + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p " + natAclRuleId.ip_protocol + srcIpAddressString
                      + " -d " + keys[0] + srcPortString + " --dport " + keys[2] + " -j RETURN";
                }
                else
                {
                    /* Rules for Double NAT */
                    cmds = std::string("")
                      + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p " + natAclRuleId.ip_protocol + srcIpAddressString
                      + " -d " + keys[0] + srcPortString + " -j RETURN";
                }
            }
        }
    }
    else
    {
        /* Static Key empty means Single NAT */
        if (key.empty())
        {
            /* Rules for all ip protocols */
            if (natAclRuleId.ip_protocol == "None")
            {
                cmds = std::string("")
                   + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p tcp" + srcIpAddressString + dstIpAddressString + srcPortString + dstPortString 
                   + " -j SNAT " + markStr + " --to-source " + externalString + fullcone + " && "
                   + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p udp" + srcIpAddressString + dstIpAddressString + srcPortString + dstPortString
                   + " -j SNAT " + markStr + " --to-source " + externalString + fullcone + " && "
                   + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p icmp" + srcIpAddressString + dstIpAddressString + srcPortString + dstPortString 
                   + " -j SNAT " + markStr + " --to-source " + externalString + fullcone;
            }
            else
            {
                cmds = std::string("")
                  + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING -p " + natAclRuleId.ip_protocol + srcIpAddressString
                  + dstIpAddressString + srcPortString + dstPortString + " -j SNAT " + markStr + " --to-source " + externalString + fullcone;
            }
        }
        else
        {
            if (keys.size() > 1)
            {
                /* Rules for Double NAT */
                cmds = std::string("")
                  + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + prototype + " -j SNAT " + markStr + srcIpAddressString + srcPortString 
                  + " --to-source " + externalString + " -d " + keys[0] + " --dport " + keys[2] + fullcone + " && "
                  + IPTABLES_CMD + " -t nat " + "-" + cmd + " PREROUTING " + prototype + " -j DNAT -d " + m_staticNaptEntry[key].local_ip + " --dport "
                  + m_staticNaptEntry[key].local_port + srcIpAddressString + srcPortString + " --to-destination " + keys[0] + ":" + keys[2] + " && "
                  + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + prototype + " -j SNAT -s " + key[0] + " --sport "
                  + keys[2] + " --to-source " + m_staticNaptEntry[key].local_ip + ":" + m_staticNaptEntry[key].local_port;
            }
            else
            {
                /* Rules for Double NAT */
                cmds = std::string("")
                  + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING " + prototype + " -j SNAT " + markStr + srcIpAddressString 
                  + " --to-source " + externalString + " -d " + key + fullcone + " && "
                  + IPTABLES_CMD + " -t nat " + "-" + cmd + " PREROUTING" + " -j DNAT -d " + m_staticNatEntry[key].local_ip + srcIpAddressString
                  + " --to-destination " + key + " && "
                  + IPTABLES_CMD + " -t nat " + "-" + opCmd + " POSTROUTING" + " -j SNAT -s " + key + " --to-source " + m_staticNatEntry[key].local_ip ;
            }
        }
    }

    int ret = swss::exec(cmds, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
        return false;
    }

    return true;
}

/* To add Static NAT entry based on Static Key if all valid conditions are met */
void NatMgr::addStaticNatEntry(const string &key)
{
    /* Example: 
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1 
     */

    string interface = EMPTY_STRING;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    /* Get the matching Ip interface for dnat type, otherwise return */
    if ((m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf(key, interface)))
    {
        SWSS_LOG_INFO("L3 Interface is not yet enabled for %s, skipping NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    /* Check the Static NAT is conflicts with Static NAPT */
    if (isMatchesWithStaticNapt(key, m_staticNatEntry[key].local_ip))
    {
        SWSS_LOG_ERROR("Invalid : %s conflicts with Static NAPT, skipping NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    m_staticNatEntry[key].interface = interface;

    if (m_staticNatEntry[key].twice_nat_id.empty())
    {
        /* Add the new Static Single NAT entry */
        SWSS_LOG_INFO("Adding the Static Single NAT entry for %s", key.c_str());
        addStaticSingleNatEntry(key);
    }
    else
    {
        /* Add the new Static Twice NAT entry */
        SWSS_LOG_INFO("Adding the Static Twice NAT entry for %s", key.c_str());
        addStaticTwiceNatEntry(key);
    }
}

/* To add Static NAPT entry based on Static Key if all valid conditions are met */
void NatMgr::addStaticNaptEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string interface = EMPTY_STRING, prototype = EMPTY_STRING;
    vector<string> keys = tokenize(key, config_db_key_delimiter);

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s NAPT entry addition to APPL_DB", key.c_str());
        return;
    }

    /* Get the matching Ip interface for dnat type, otherwise return */
    if ((m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf(keys[0], interface)))
    {
        SWSS_LOG_INFO("L3 Interface is not yet enabled for %s, skipping NAPT entry addition to APPL_DB", key.c_str());
        return;
    }

    /* Check the Static NAPT is conflicts with Static NAT */
    if (isMatchesWithStaticNat(keys[0], m_staticNaptEntry[key].local_ip))
    {
        SWSS_LOG_ERROR("Invalid : %s conflicts with Static NAT, skipping NAPT entry addition to APPL_DB", key.c_str());
        return;
    }

    m_staticNaptEntry[key].interface = interface;

    if (m_staticNaptEntry[key].twice_nat_id.empty())
    {
        /* Add the new Static Single NAPT entry */
        SWSS_LOG_INFO("Adding the Static Single NAPT entry for %s", key.c_str());
        addStaticSingleNaptEntry(key);
    }
    else
    {
        /* Add the new Static Twice NAPT entry */
        SWSS_LOG_INFO("Adding the Static Twice NAPT entry for %s", key.c_str());
        addStaticTwiceNaptEntry(key);
    }
}

/* To delete Static NAT entry based on Static Key if all valid conditions are met */
void NatMgr::removeStaticNatEntry(const string &key)
{
    /* Example: 
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1 
     */

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s NAT entry deletion", key.c_str());
        return;
    }

    if (m_staticNatEntry[key].twice_nat_id.empty())
    {
        /* Remove the Static Single NAT Entry */
        SWSS_LOG_INFO("Deleting the Static Single NAT entry for %s", key.c_str());
        removeStaticSingleNatEntry(key);
    }
    else if ((m_staticNatEntry[key].twice_nat_added == true))
    {
        /* Remove the Static Twice NAT entry */
        SWSS_LOG_INFO("Deleting the Static Twice NAT entry for %s", key.c_str());
        removeStaticTwiceNatEntry(key);
    }
    else
    {
        SWSS_LOG_INFO("No Static Twice NAT entry to delete for %s", key.c_str());
        m_staticNatEntry[key].interface = NONE_STRING;
    }
}

/* To delete Static NAPT entry based on Static Key if all valid conditions are met */
void NatMgr::removeStaticNaptEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s NAPT entry deletion", key.c_str());
        return;
    }

    if (m_staticNaptEntry[key].twice_nat_id.empty())
    {
        /* Remove the Static Single NAPT Entry */
        SWSS_LOG_INFO("Deleting the Static Single NAPT entry for %s", key.c_str());
        removeStaticSingleNaptEntry(key);
    }
    else if ((m_staticNaptEntry[key].twice_nat_added == true))
    {
        /* Remove the Static Twice NAPT entry */
        SWSS_LOG_INFO("Deleting the Static Twice NAPT entry for %s", key.c_str());
        removeStaticTwiceNaptEntry(key);
    }
    else
    {
        SWSS_LOG_INFO("No Static Twice NAPT entry to delete for %s", key.c_str());
        m_staticNaptEntry[key].interface = NONE_STRING;
    }
}

/* To add Static NAT entries based on L3 Interface if all valid conditions are met */
void NatMgr::addStaticNatEntries(const string port, const string ipPrefix)
{
    /* Example:
     * Port is Ethernet1 and ipPrefix is 10.0.0.1/24
     */

    string prototype, interface;
    bool isEntryAdded = false;
 
    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAT entry addition to APPL_DB");
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        prototype = EMPTY_STRING, interface = EMPTY_STRING;

        /* Check interface is assigned, means Entry is already added, otherwise continue */
        if ((*it).second.interface != NONE_STRING)
        {
            continue;
        }

        if ((port == NONE_STRING) and (ipPrefix == NONE_STRING))
        {
            /* Get the matching Ip interface for dnat type, otherwise return */
            if (((*it).second.nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf((*it).first, interface)))
            {
                continue;
            }
        }
        else
        {
            /* Check Global ip is matching, otherwise continue */
            if (isGlobalIpMatching(ipPrefix, (*it).first) == false)
            {
                continue;
            }
            interface = port;
        }

        /* Check the Static NAT is conflicts with Static NAPT */
        if (isMatchesWithStaticNapt((*it).first, (*it).second.local_ip))
        {
            continue;
        }

        (*it).second.interface = interface;

        isEntryAdded = true;

        if ((*it).second.twice_nat_id.empty())
        {
            /* Add the new Static Single NAT entry */
            SWSS_LOG_INFO("Adding the Static Single NAT entry for %s", (*it).first.c_str());
            addStaticSingleNatEntry((*it).first);
        }
        else
        {
            /* Add the new Static Twice NAT entry */
            SWSS_LOG_INFO("Adding the Static Twice NAT entry for %s", (*it).first.c_str());
            addStaticTwiceNatEntry((*it).first);
        }
    }

    if (!isEntryAdded)
    {
        SWSS_LOG_INFO("No Static NAT entries to add");
    }
}

/* To add Static NAPT entries based on L3 Interface if all valid conditions are met */
void NatMgr::addStaticNaptEntries(const string port, const string ipPrefix)
{
    /* Example:
     * Port is Ethernet1 and ipPrefix is 10.0.0.1/24
     */

    string prototype, interface;
    bool isEntryAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAPT entry addition to APPL_DB");
        return;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<string> keys = tokenize((*it).first, config_db_key_delimiter);
        vector<FieldValueTuple> fvVectorSnat, fvVectorDnat;
        prototype = EMPTY_STRING, interface = EMPTY_STRING;
 
        /* Check interface is assigned, means Entry is already added otherwise continue*/
        if ((*it).second.interface != NONE_STRING)
        {
            continue;
        }

        if ((port == NONE_STRING) and (ipPrefix == NONE_STRING))
        {
            /* Get the matching Ip interface for dnat type, otherwise return */
            if (((*it).second.nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf(keys[0], interface)))
            {
                continue;
            }
        }
        else
        {
            /* Check Global ip is matching, otherwise continue */
            if (isGlobalIpMatching(ipPrefix, keys[0]) == false)
            {
                continue;
            }
            interface = port;
        }

        /* Check the Static NAPT is conflicts with Static NAT */
        if (isMatchesWithStaticNat(keys[0], (*it).second.local_ip))
        {
            continue;
        }

        (*it).second.interface = interface;

        isEntryAdded = true;

        if ((*it).second.twice_nat_id.empty())
        {
            /* Add the new Static Single NAPT entry */
            SWSS_LOG_INFO("Adding the Static Single NAPT entry for %s", (*it).first.c_str());
            addStaticSingleNaptEntry((*it).first);
        }
        else
        {
            /* Add the new Static Twice NAPT entry */
            SWSS_LOG_INFO("Adding the Static Twice NAPT entry for %s", (*it).first.c_str());
            addStaticTwiceNaptEntry((*it).first);
        }
    }

    if (!isEntryAdded)
    {
        SWSS_LOG_INFO("No Static NAPT entries to add");
    }
}

/* To delete Static NAT entries based on L3 Interface if all valid conditions are met */
void NatMgr::removeStaticNatEntries(const string port, const string ipPrefix)
{
    /* Example:
     * Port is Ethernet1 and ipPrefix is 10.0.0.1/24
     */

    string prototype, interface;
    bool isEntryDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAT entry deletion from APPL_DB");
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        prototype = EMPTY_STRING, interface = EMPTY_STRING;

        if ((port == NONE_STRING) and (ipPrefix == NONE_STRING))
        {
            /* Get the matching Ip interface for dnat type, otherwise return */
            if (((*it).second.nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf((*it).first, interface)))
            {
                continue;
            }

            /* Check interface is matching, otherwise continue */
            if ((*it).second.interface != interface)
            {
                continue;
            }
        }
        else
        {
            /* Check interface is matching, otherwise continue */
            if ((*it).second.interface != port)
            {
                continue;
            }

            /* Check Global ip is matching, otherwise continue */
            if (isGlobalIpMatching(ipPrefix, (*it).first) == false)
            {
                continue;
            }
            interface = port;
        }

        isEntryDeleted = true;

        /* Remove the Static NAT Entry */
        SWSS_LOG_INFO("Deleting the Static NAT entry for %s", (*it).first.c_str());
        removeStaticNatEntry((*it).first);
    }

    if (!isEntryDeleted)
    {
        SWSS_LOG_INFO("No Static NAT entries to delete");
    }
}

/* To delete Static NAPT entries based on L3 Interface if all valid conditions are met */
void NatMgr::removeStaticNaptEntries(const string port, const string ipPrefix)
{
    /* Example:
     * Port is Ethernet1 and ipPrefix is 10.0.0.1/24
     */

    string prototype, interface;
    bool isEntryDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAPT entry deletion from APPL_DB");
        return;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<string> keys = tokenize((*it).first, config_db_key_delimiter);
        prototype = EMPTY_STRING, interface = EMPTY_STRING;

        if ((port == NONE_STRING) and (ipPrefix == NONE_STRING))
        {
            /* Get the matching Ip interface for dnat type, otherwise return */
            if (((*it).second.nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf(keys[0], interface)))
            {
                continue;
            }

            /* Check interface is matching, otherwise continue */
            if ((*it).second.interface != interface)
            {
                continue;
            }
        }
        else
        {
            /* Check interface is matching, otherwise continue */
            if ((*it).second.interface != port)
            {
                continue;
            }

            /* Check Global ip is matching, otherwise continue */
            if (isGlobalIpMatching(ipPrefix, keys[0]) == false)
            {
                continue;
            }
            interface = port;
        }

        isEntryDeleted = true;

        /* Remove the Static NAPT Entry */
        SWSS_LOG_INFO("Deleting the Static NAPT entry for %s", (*it).first.c_str());
        removeStaticNaptEntry((*it).first);
    }

    if (!isEntryDeleted)
    {
        SWSS_LOG_INFO("No Static NAPT entries to delete");
    }
}

/* To add Static Single NAT entry based on Static Key */
void NatMgr::addStaticSingleNatEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string appKeyDnat = EMPTY_STRING, appKeySnat = EMPTY_STRING;
    string interface = m_staticNatEntry[key].interface;
    vector<FieldValueTuple> fvVectorDnat, fvVectorSnat;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Single NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    /* Create APPL_DB key and it's values */
    if (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE)
    {
        appKeyDnat += key;
        FieldValueTuple p(TRANSLATED_IP, m_staticNatEntry[key].local_ip);
        fvVectorDnat.push_back(p);

        appKeySnat += m_staticNatEntry[key].local_ip;
        FieldValueTuple q(TRANSLATED_IP, key);
        fvVectorSnat.push_back(q);
    }
    else if (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        appKeySnat += key;
        FieldValueTuple p(TRANSLATED_IP, m_staticNatEntry[key].local_ip);
        fvVectorSnat.push_back(p);

        appKeyDnat += m_staticNatEntry[key].local_ip;
        FieldValueTuple q(TRANSLATED_IP, key);
        fvVectorDnat.push_back(q);
    }

    FieldValueTuple r(NAT_TYPE, DNAT_NAT_TYPE);
    fvVectorDnat.push_back(r);
    FieldValueTuple s(NAT_TYPE, SNAT_NAT_TYPE);
    fvVectorSnat.push_back(s);

    FieldValueTuple t(ENTRY_TYPE, STATIC_ENTRY_TYPE);
    fvVectorDnat.push_back(t);
    fvVectorSnat.push_back(t);

    if (m_staticNatEntry[key].twice_nat_id != EMPTY_STRING)
    {
      FieldValueTuple t(TWICE_NAT_ID, m_staticNatEntry[key].twice_nat_id);
      fvVectorDnat.push_back(t);
      fvVectorSnat.push_back(t);
    }

    /* Add it to APPL_DB */
    m_appNatTableProducer.set(appKeyDnat, fvVectorDnat);
    m_appNatTableProducer.set(appKeySnat, fvVectorSnat);

    SWSS_LOG_INFO("Added Static NAT %s to APPL_DB", key.c_str());

    /* Add a dummy conntrack entry for Static NAT entry */
    addConntrackSingleNatEntry(key);

    /* Add Static NAT iptables rule */
    if (!setStaticNatIptablesRules(INSERT, interface, key, m_staticNatEntry[key].local_ip, m_staticNatEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to add Static NAT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Added Static NAT iptables rules for %s", key.c_str());
    }
}

/* To add Static Twice NAT entry based on Static Key if all valid conditions are met */
void NatMgr::addStaticTwiceNatEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string interface = EMPTY_STRING;
    string src, translated_src, dest, translated_dest;
    bool isEntryAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        vector<FieldValueTuple> fvVector, reversefvVector;

        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is not added, otherwise continue */
        if ((*it).second.twice_nat_added or m_staticNatEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check interface is assigned, otherwise continue */
        if (((*it).second.interface == NONE_STRING) or (m_staticNatEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = key;
            dest = (*it).first;
            translated_src = m_staticNatEntry[key].local_ip;
            translated_dest = (*it).second.local_ip;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = (*it).first;
            dest = key;
            translated_src = (*it).second.local_ip;
            translated_dest = m_staticNatEntry[key].local_ip;
            interface = m_staticNatEntry[key].interface;
        }

        /* Create APPL_DB key and it's values */
        string appKey = src + ":" + dest;
        string reverseAppKey = translated_dest + ":" + translated_src; 
        
        FieldValueTuple p(TRANSLATED_SRC_IP, translated_src);
        fvVector.push_back(p);
        FieldValueTuple q(TRANSLATED_DST_IP, translated_dest);
        fvVector.push_back(q);
        FieldValueTuple r(ENTRY_TYPE, STATIC_ENTRY_TYPE);
        fvVector.push_back(r);

        FieldValueTuple p1(TRANSLATED_SRC_IP, dest);
        reversefvVector.push_back(p1);
        FieldValueTuple q1(TRANSLATED_DST_IP, src);
        reversefvVector.push_back(q1);
        reversefvVector.push_back(r);

        (*it).second.twice_nat_added = true;
        m_staticNatEntry[key].twice_nat_added = true;

        /* Add it to APPL_DB */
        m_appTwiceNatTableProducer.set(appKey, fvVector);
        m_appTwiceNatTableProducer.set(reverseAppKey, reversefvVector);
        SWSS_LOG_INFO("Added Static Twice NAT for %s and %s to APPL_DB", key.c_str(), (*it).first.c_str());

        /* Add a dummy conntrack entry for Static Twice NAT entry */
        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            addConntrackTwiceNatEntry(key, (*it).first);
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            addConntrackTwiceNatEntry((*it).first, key);
        }

        /* Add Static NAT iptables rule */
        if (!setStaticTwiceNatIptablesRules(INSERT, interface, src, translated_src, dest, translated_dest))
        {
            SWSS_LOG_ERROR("Failed to add Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            isEntryAdded = true;
            SWSS_LOG_INFO("Added Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        break;
    }

    if (!isEntryAdded)
    {
        SWSS_LOG_INFO("No Static Twice NAT entries to add");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_name = (*it).second.pool_name;
        string port_range = EMPTY_STRING;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is not added, otherwise continue */
        if ((*it).second.twice_nat_added or m_staticNatEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check interface is assigned, means Entry is already added, otherwise continue */
        if (((*it).second.pool_interface == NONE_STRING) or (m_staticNatEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is not present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (!port_range.empty() and  (port_range != "NULL"))
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules addition for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        (*it).second.twice_nat_added = true;
        (*it).second.static_key = key;
        m_staticNatEntry[key].twice_nat_added = true;
        m_staticNatEntry[key].binding_key = (*it).first;
        isEntryAdded = true;

        setDynamicAllForwardOrAclbasedRules(ADD, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, (*it).second.acl_name,
                                            (*it).first);

        break;
    }

    if (!isEntryAdded)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAT entries to add");
    }

}

/* To add Static Single NAPT entry based on Static Key */
void NatMgr::addStaticSingleNaptEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string prototype = EMPTY_STRING, interface = m_staticNaptEntry[key].interface;;
    vector<string> keys = tokenize(key, config_db_key_delimiter);
    string appKeyDnat = EMPTY_STRING, appKeySnat = EMPTY_STRING;
    vector<FieldValueTuple> fvVectorDnat, fvVectorSnat;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    /* Create the APPL_DB key and it's values */
    if (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE)
    {
        appKeyDnat += (keys[1] + DEFAULT_KEY_SEPARATOR + keys[0] + DEFAULT_KEY_SEPARATOR + keys[2]);
        FieldValueTuple p(TRANSLATED_IP, m_staticNaptEntry[key].local_ip);
        FieldValueTuple q(TRANSLATED_L4_PORT, m_staticNaptEntry[key].local_port);
        fvVectorDnat.push_back(p);
        fvVectorDnat.push_back(q);

        appKeySnat += (keys[1] + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_ip + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_port);
        FieldValueTuple r(TRANSLATED_IP, keys[0]);
        FieldValueTuple s(TRANSLATED_L4_PORT, keys[2]);
        fvVectorSnat.push_back(r);
        fvVectorSnat.push_back(s);
    }
    else if (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        appKeySnat += (keys[1] + DEFAULT_KEY_SEPARATOR + keys[0] + DEFAULT_KEY_SEPARATOR + keys[2]);
        FieldValueTuple p(TRANSLATED_IP, m_staticNaptEntry[key].local_ip);
        FieldValueTuple q(TRANSLATED_L4_PORT, m_staticNaptEntry[key].local_port);
        fvVectorSnat.push_back(p);
        fvVectorSnat.push_back(q);

        appKeyDnat += (keys[1] + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_ip + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_port);
        FieldValueTuple r(TRANSLATED_IP, keys[0]);
        FieldValueTuple s(TRANSLATED_L4_PORT, keys[2]);
        fvVectorDnat.push_back(r);
        fvVectorDnat.push_back(s);
    }

    FieldValueTuple t(NAT_TYPE, DNAT_NAT_TYPE);
    fvVectorDnat.push_back(t);
    FieldValueTuple u(NAT_TYPE, SNAT_NAT_TYPE);
    fvVectorSnat.push_back(u);

    FieldValueTuple v(ENTRY_TYPE, STATIC_ENTRY_TYPE);
    fvVectorDnat.push_back(v);
    fvVectorSnat.push_back(v);

    /* Add it to APPL_DB */
    m_appNaptTableProducer.set(appKeyDnat, fvVectorDnat);
    m_appNaptTableProducer.set(appKeySnat, fvVectorSnat);

    SWSS_LOG_INFO("Added Static NAPT %s to APPL_DB", key.c_str());

    /* Delete any conntrack entry if exists */
    deleteConntrackSingleNaptEntry(key);

    /* Add a dummy conntrack entry for Static NAPT entry */
    addConntrackSingleNaptEntry(key);

    /* Add Static NAPT iptables rule */
    if (!setStaticNaptIptablesRules(INSERT, interface, prototype, keys[0], keys[2],
                                    m_staticNaptEntry[key].local_ip, m_staticNaptEntry[key].local_port,
                                    m_staticNaptEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to add Static NAPT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Added Static NAPT iptables rules for %s", key.c_str());
    }
}

/* To add Static Twice NAPT entry based on Static Key if all valid conditions are met */
void NatMgr::addStaticTwiceNaptEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string interface = EMPTY_STRING, prototype = EMPTY_STRING;
    vector<string> keys = tokenize(key, config_db_key_delimiter);
    string src, translated_src, dest, translated_dest;
    string src_port, translated_src_port, dest_port, translated_dest_port;
    bool isEntryAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<FieldValueTuple> fvVector, reversefvVector;
        vector<string> entry_keys = tokenize((*it).first, config_db_key_delimiter);

        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check for both protocols are same, otherwise continue */
        if (entry_keys[1] != keys[1])
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is not added, otherwise continue */
        if ((*it).second.twice_nat_added or m_staticNaptEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check interface is assigned, means Entry is already added, otherwise continue */
        if (((*it).second.interface == NONE_STRING) or (m_staticNaptEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = keys[0];
            src_port = keys[2];
            dest = entry_keys[0];
            dest_port = entry_keys[2];
            translated_src = m_staticNaptEntry[key].local_ip;
            translated_src_port = m_staticNaptEntry[key].local_port;
            translated_dest = (*it).second.local_ip;
            translated_dest_port = (*it).second.local_port;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = entry_keys[0];
            src_port = entry_keys[2];
            dest = keys[0];
            dest_port = keys[2];
            translated_src = (*it).second.local_ip;
            translated_src_port = (*it).second.local_port;
            translated_dest = m_staticNaptEntry[key].local_ip;
            translated_dest_port = m_staticNaptEntry[key].local_port;
            interface = m_staticNaptEntry[key].interface;
        }

        /* Create APPL_DB key and it's values */
        string appKey = keys[1] + ":" + src + ":" + src_port + ":" + dest + ":" + dest_port;
        string reverseAppKey = keys[1] + ":" + translated_dest + ":" + translated_dest_port + ":" + translated_src + ":" + translated_src_port;

        FieldValueTuple p(TRANSLATED_SRC_IP, translated_src);
        fvVector.push_back(p);
        FieldValueTuple q(TRANSLATED_SRC_L4_PORT, translated_src_port);
        fvVector.push_back(q);
        FieldValueTuple r(TRANSLATED_DST_IP, translated_dest);
        fvVector.push_back(r);
        FieldValueTuple s(TRANSLATED_DST_L4_PORT, translated_dest_port);
        fvVector.push_back(s);
        FieldValueTuple t(ENTRY_TYPE, STATIC_ENTRY_TYPE);
        fvVector.push_back(t);

        FieldValueTuple p1(TRANSLATED_SRC_IP, dest);
        reversefvVector.push_back(p1);
        FieldValueTuple q1(TRANSLATED_SRC_L4_PORT, dest_port);
        reversefvVector.push_back(q1);
        FieldValueTuple r1(TRANSLATED_DST_IP, src);
        reversefvVector.push_back(r1);
        FieldValueTuple s1(TRANSLATED_DST_L4_PORT, src_port);
        reversefvVector.push_back(s1);
        reversefvVector.push_back(t);

        (*it).second.twice_nat_added = true;
        m_staticNaptEntry[key].twice_nat_added = true;

        /* Add it to APPL_DB */
        m_appTwiceNaptTableProducer.set(appKey, fvVector);
        m_appTwiceNaptTableProducer.set(reverseAppKey, reversefvVector);

        SWSS_LOG_INFO("Added Static Twice NAPT for %s and %s to APPL_DB", key.c_str(), (*it).first.c_str());

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            /* Delete any conntrack entry if exists */
            deleteConntrackTwiceNaptEntry(key, (*it).first);

            /* Add a dummy conntrack entry for Static Twice NAPT entry */
            addConntrackTwiceNaptEntry(key, (*it).first);
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            /* Delete any conntrack entry if exists */
            deleteConntrackTwiceNaptEntry((*it).first, key);

            /* Add a dummy conntrack entry for Static Twice NAPT entry */
            addConntrackTwiceNaptEntry((*it).first, key);
        }

        /* Add Static NAPT iptables rule */
        if (!setStaticTwiceNaptIptablesRules(INSERT, interface, prototype, src, src_port, translated_src, translated_src_port,
            dest, dest_port, translated_dest, translated_dest_port))
        {
            SWSS_LOG_ERROR("Failed to add Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            isEntryAdded = true;
            SWSS_LOG_INFO("Added Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        break;
    }

    if (!isEntryAdded)
    {
        SWSS_LOG_INFO("No Static Twice NAPT entries to add");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_name = (*it).second.pool_name;
        string port_range = EMPTY_STRING;   

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }
       
        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is not added, otherwise continue */
        if ((*it).second.twice_nat_added or m_staticNaptEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check interface is assigned, means Entry is already added, otherwise continue */
        if (((*it).second.pool_interface == NONE_STRING) or (m_staticNaptEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (port_range.empty() or (port_range == "NULL"))
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules addition for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        (*it).second.twice_nat_added = true;
        (*it).second.static_key = key;
        m_staticNaptEntry[key].twice_nat_added = true;
        m_staticNaptEntry[key].binding_key = (*it).first;
        isEntryAdded = true;

        setDynamicAllForwardOrAclbasedRules(ADD, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, (*it).second.acl_name,
                                            (*it).first);

        break;
    }

    if (!isEntryAdded)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAPT entries to add");
    }
}

/* To delete Static Single NAT entry based on Static Key if all valid conditions are met */
void NatMgr::removeStaticSingleNatEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string interface = EMPTY_STRING;
    vector<FieldValueTuple> fvVectorDnat, fvVectorSnat;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s NAT entry deletion", key.c_str());
        return;
    }

    /* Get the matching Ip interface for dnat type, otherwise return */
    if ((m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf(key, interface)))
    {
        SWSS_LOG_INFO("L3 Interface is not yet enabled for %s, skipping NAT entry deletion", key.c_str());
        return;
    }

    /* Check for the key interface is matching with the saved one, otherwise return */
    if (m_staticNatEntry[key].interface != interface)
    {
        SWSS_LOG_INFO("Interface is not matching for %s, skipping NAT entry deletion", key.c_str());
        m_staticNatEntry.erase(key);
        return;
    }

    /* Create the APPL_DB key and it's values */
    string appKeyDnat = EMPTY_STRING, appKeySnat = EMPTY_STRING;

    if (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE)
    {
        appKeyDnat += key;
        appKeySnat += m_staticNatEntry[key].local_ip;
    }
    else if (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        appKeySnat += key;
        appKeyDnat += m_staticNatEntry[key].local_ip;
    }

    /* Delete conntrack entry */
    deleteConntrackSingleNatEntry(key);

    /* Delete it from APPL_DB */
    m_appNatTableProducer.del(appKeyDnat);
    m_appNatTableProducer.del(appKeySnat);

    SWSS_LOG_INFO("Deleted Static NAT %s from APPL_DB", key.c_str());

    /* Remove Static NAT iptables rule */
    if (!setStaticNatIptablesRules(DELETE, interface, key, m_staticNatEntry[key].local_ip, m_staticNatEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to delete Static NAT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Deleted Static NAT iptables rules for %s", key.c_str());
    }

    m_staticNatEntry[key].interface = NONE_STRING;

    /* Add any static NAPT conflict entry if present */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<string> keys = tokenize((*it).first, config_db_key_delimiter);
        if ((keys[0] == key) and ((*it).second.local_ip == m_staticNatEntry[key].local_ip) and
            ((*it).second.interface == NONE_STRING))
        {
            /* Add the new Static NAPT entry */
            SWSS_LOG_INFO("Adding the Static NAPT entry for %s", key.c_str());
            addStaticNaptEntry((*it).first);
            break;
        }
    }
}

/* To delete Static Twice NAT entry based on Static Key if all valid conditions are met */
void NatMgr::removeStaticTwiceNatEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string interface = EMPTY_STRING;
    string src, translated_src, dest, translated_dest;
    bool isEntryDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT entry deletion from APPL_DB", key.c_str());
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is not added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNatEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check interface is not assigned, means Entry is not added, otherwise continue */
        if (((*it).second.interface == NONE_STRING) or (m_staticNatEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = key;
            dest = (*it).first;
            translated_src = m_staticNatEntry[key].local_ip;
            translated_dest = (*it).second.local_ip;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = (*it).first;
            dest = key;
            translated_src = (*it).second.local_ip;
            translated_dest = m_staticNatEntry[key].local_ip;
            interface = m_staticNatEntry[key].interface;
        }

        string appKey = src + ":" + dest;
        string reverseAppKey = translated_dest + ":" + translated_src;

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            /* Delete any conntrack entry */
            deleteConntrackTwiceNatEntry(key, (*it).first);
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            /* Delete any conntrack entry */
            deleteConntrackTwiceNatEntry((*it).first, key);
        }

        /* Delete it from APPL_DB */
        m_appTwiceNatTableProducer.del(appKey);
        m_appTwiceNatTableProducer.del(reverseAppKey);

        (*it).second.twice_nat_added = false;
        m_staticNatEntry[key].twice_nat_added = false;

        SWSS_LOG_INFO("Deleted Static Twice NAT for %s and %s from APPL_DB", key.c_str(), (*it).first.c_str());

        /* Delete Static NAT iptables rule */
        if (!setStaticTwiceNatIptablesRules(DELETE, interface, src, translated_src, dest, translated_dest))
        {
            SWSS_LOG_ERROR("Failed to delete Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            SWSS_LOG_INFO("Deleted Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
            isEntryDeleted = true;
        }

        m_staticNatEntry[key].interface = NONE_STRING;

        return;
    }

    if (!isEntryDeleted)
    {
        SWSS_LOG_INFO("No Static Twice NAT entries to delete");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string port_range = EMPTY_STRING;
        string pool_name = (*it).second.pool_name;
        string acls_name = (*it).second.acl_name;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNatEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check interface is assigned, otherwise continue */
        if (((*it).second.pool_interface == NONE_STRING) or (m_staticNatEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is not present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (!port_range.empty() and (port_range != "NULL"))
        {
            continue;
        }

        /* Check the key is matching, otherwise continue */
        if (m_staticNatEntry[key].binding_key != (*it).first)
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules deletion for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        /* Delete Dynamic rules */
        setDynamicAllForwardOrAclbasedRules(DELETE, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, acls_name, (*it).first);

        (*it).second.twice_nat_added = false;
        (*it).second.static_key = EMPTY_STRING;
        m_staticNatEntry[key].twice_nat_added = false;
        m_staticNatEntry[key].binding_key = EMPTY_STRING;
        isEntryDeleted = true;
        break;
    }

    if (!isEntryDeleted)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAT entries to delete");
    }
}

/* To delete Static Single NAPT entry based on Static Key if all valid conditions are met */
void NatMgr::removeStaticSingleNaptEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string interface = EMPTY_STRING, prototype = EMPTY_STRING;
    vector<string> keys = tokenize(key, config_db_key_delimiter);

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s NAPT entry deletion", key.c_str());
        return;
    }

    /* Get the matching Ip interface for dnat type, otherwise return */
    if ((m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE) and (!getIpEnabledIntf(keys[0], interface)))
    {
        SWSS_LOG_INFO("L3 Interface is not yet enabled for %s, skipping NAPT entry deletion", key.c_str());
        return;
    }

    /* Check for the key interface is matching with the save one, otherwise return */
    if (m_staticNaptEntry[key].interface != interface)
    {
        SWSS_LOG_INFO("Interface is not matching for %s, skipping NAPT entry deletion", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    /* Create the APPL_DB key and it's values */
    string appKeyDnat = EMPTY_STRING, appKeySnat = EMPTY_STRING;

    if (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE)
    {
        appKeyDnat += (keys[1] + DEFAULT_KEY_SEPARATOR + keys[0] + DEFAULT_KEY_SEPARATOR + keys[2]);
        appKeySnat += (keys[1] + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_ip + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_port);
    }
    else if (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE)
    {
        appKeySnat += (keys[1] + DEFAULT_KEY_SEPARATOR + keys[0] + DEFAULT_KEY_SEPARATOR + keys[2]);
        appKeyDnat += (keys[1] + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_ip + DEFAULT_KEY_SEPARATOR + m_staticNaptEntry[key].local_port);
    }

    /* Delete conntrack entry */
    deleteConntrackSingleNaptEntry(key);

    /* Delete it from APPL_DB */
    m_appNaptTableProducer.del(appKeyDnat);
    m_appNaptTableProducer.del(appKeySnat);

    SWSS_LOG_INFO("Deleted Static NAPT %s from APPL_DB", key.c_str());

    /* Remove Static NAPT iptables rule */
    if (!setStaticNaptIptablesRules(DELETE, interface, prototype, keys[0], keys[2],
                                    m_staticNaptEntry[key].local_ip, m_staticNaptEntry[key].local_port,
                                    m_staticNaptEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to delete Static NAPT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Deleted Static NAPT iptables rules for %s", key.c_str());
    }

    m_staticNaptEntry[key].interface = NONE_STRING;

    /* Add any static NAT conflict entry if present */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        if (((*it).first == keys[0]) and ((*it).second.local_ip == m_staticNaptEntry[key].local_ip) and
            ((*it).second.interface == NONE_STRING))
        {
            /* Add the new Static NAT entry */
            SWSS_LOG_INFO("Adding the Static NAT entry for %s", key.c_str());
            addStaticNatEntry(keys[0]);
            break;
        }
    }
}

/* To delete Static Twice NAPT entry based on Static Key if all valid conditions are met */
void NatMgr::removeStaticTwiceNaptEntry(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string interface = EMPTY_STRING, prototype = EMPTY_STRING;
    vector<string> keys = tokenize(key, config_db_key_delimiter);
    string src, translated_src, dest, translated_dest;
    string src_port, translated_src_port, dest_port, translated_dest_port;
    bool isEntryDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAPT entry deletion from APPL_DB", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<string> entry_keys = tokenize((*it).first, config_db_key_delimiter);

        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check for both protocols are same, otherwise continue */
        if (entry_keys[1] != keys[1])
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is not added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNaptEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check interface is not assigned, means Entry is not added, otherwise continue */
        if (((*it).second.interface == NONE_STRING) or (m_staticNaptEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = keys[0];
            src_port = keys[2];
            dest = entry_keys[0];
            dest_port = entry_keys[2];
            translated_src = m_staticNaptEntry[key].local_ip;
            translated_src_port = m_staticNaptEntry[key].local_port;
            translated_dest = (*it).second.local_ip;
            translated_dest_port = (*it).second.local_port;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = entry_keys[0];
            src_port = entry_keys[2];
            dest = keys[0];
            dest_port = keys[2];
            translated_src = (*it).second.local_ip;
            translated_src_port = (*it).second.local_port;
            translated_dest = m_staticNaptEntry[key].local_ip;
            translated_dest_port = m_staticNaptEntry[key].local_port;
            interface = m_staticNaptEntry[key].interface;
        }

        string appKey = keys[1] + ":" + src + ":" + src_port + ":" + dest + ":" + dest_port;
        string reverseAppKey = keys[1] + ":" + translated_dest + ":" + translated_dest_port + ":" + translated_src + ":" + translated_src_port;

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            /* Delete any conntrack entry */
            deleteConntrackTwiceNaptEntry(key, (*it).first);
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            /* Delete any conntrack entry */
            deleteConntrackTwiceNaptEntry((*it).first, key);
        }
 
        /* Delete it from APPL_DB */
        m_appTwiceNaptTableProducer.del(appKey);
        m_appTwiceNaptTableProducer.del(reverseAppKey);

        (*it).second.twice_nat_added = false;
        m_staticNaptEntry[key].twice_nat_added = false;

        SWSS_LOG_INFO("Deleted Static Twice NAPT for %s and %s from APPL_DB", key.c_str(), (*it).first.c_str());

        /* Delete Static NAPT iptables rule */
        if (!setStaticTwiceNaptIptablesRules(DELETE, interface, prototype, src, src_port, translated_src, translated_src_port,
                                             dest, dest_port, translated_dest, translated_dest_port))
        {
            SWSS_LOG_ERROR("Failed to delete Static Twice NAPT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            SWSS_LOG_INFO("Deleted Static Twice NAPT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
            isEntryDeleted = true;
        }

        m_staticNaptEntry[key].interface = NONE_STRING;

        return;
    }

    if (!isEntryDeleted)
    {
        SWSS_LOG_INFO("No Static Twice NAPT entries to delete");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string port_range = EMPTY_STRING;
        string pool_name = (*it).second.pool_name;
        string acls_name = (*it).second.acl_name;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNaptEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check interface is assigned, otherwise continue */
        if (((*it).second.pool_interface == NONE_STRING) or (m_staticNaptEntry[key].interface == NONE_STRING))
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (port_range.empty() or (port_range == "NULL"))
        {
            continue;
        }

        /* Check the key is matching, otherwise continue */
        if (m_staticNaptEntry[key].binding_key != (*it).first)
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules deletion for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        /* Delete Dynamic rules */
        setDynamicAllForwardOrAclbasedRules(DELETE, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, acls_name, (*it).first);

        (*it).second.twice_nat_added = false;
        (*it).second.static_key = EMPTY_STRING;
        m_staticNatEntry[key].twice_nat_added = false;
        m_staticNatEntry[key].binding_key = EMPTY_STRING;
        isEntryDeleted = true;
        break;
    }

    if (!isEntryDeleted)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAPT entries to delete");
    }
}

/* To add Static NAT Iptables based on L3 Interface if all valid conditions are met */
void NatMgr::addStaticNatIptables(const string port)
{
    /* Example:
     * Port is Ethernet1
     */
    bool isRulesAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAT Iptables addition");
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        /* Check interface is same as given Port, otherwise continue */
        if ((*it).second.interface != port)
        {
            continue;
        }

        isRulesAdded = true;

        if ((*it).second.twice_nat_id.empty())
        {
            /* Add the new Static Single NAT Iptables */
            SWSS_LOG_INFO("Adding the Static Single NAT Iptables for %s", (*it).first.c_str());
            addStaticSingleNatIptables((*it).first);
        }
        else
        {
            /* Add the new Static Twice NAT Iptables */
            SWSS_LOG_INFO("Adding the Static Twice NAT Iptables for %s", (*it).first.c_str());
            addStaticTwiceNatIptables((*it).first);
        }
    }

    if (!isRulesAdded)
    {
        SWSS_LOG_INFO("No Static NAT iptables rules to add");
    }
}

/* To add Static Single NAT iptables based on Static Key */
void NatMgr::addStaticSingleNatIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string interface = m_staticNatEntry[key].interface;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Single NAT Iptables addition", key.c_str());
        return;
    }

    /* Add Static NAT iptables rule */
    if (!setStaticNatIptablesRules(INSERT, interface, key, m_staticNatEntry[key].local_ip, m_staticNatEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to add Static NAT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Added Static NAT iptables rules for %s", key.c_str());
    }
}

/* To add Static Twice NAT Iptables based on Static Key if all valid conditions are met */
void NatMgr::addStaticTwiceNatIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string interface = EMPTY_STRING;
    string src, translated_src, dest, translated_dest;
    string src_port, translated_src_port, dest_port, translated_dest_port;
    bool isRulesAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if (!(*it).second.twice_nat_added or !m_staticNatEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = key;
            dest = (*it).first;
            translated_src = m_staticNatEntry[key].local_ip;
            translated_dest = (*it).second.local_ip;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = (*it).first;
            dest = key;
            translated_src = (*it).second.local_ip;
            translated_dest = m_staticNatEntry[key].local_ip;
            interface = m_staticNatEntry[key].interface;
        }

        /* Add Static NAT iptables rule */
        if (!setStaticTwiceNatIptablesRules(INSERT, interface, src, translated_src, dest, translated_dest))
        {
            SWSS_LOG_ERROR("Failed to add Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            isRulesAdded = true;
            SWSS_LOG_INFO("Added Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        break;
    }

    if (!isRulesAdded)
    {
        SWSS_LOG_INFO("No Static Twice NAT iptables rules to add");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_name = (*it).second.pool_name;
        string port_range = EMPTY_STRING;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if (!(*it).second.twice_nat_added or !m_staticNatEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is not present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (!port_range.empty() and  (port_range != "NULL"))
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules addition for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        isRulesAdded = true;
        setDynamicAllForwardOrAclbasedRules(ADD, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, (*it).second.acl_name,
                                            (*it).first);
        break;
    }

    if (!isRulesAdded)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAT iptables rules to add");
    }
}

/* To add Static NAPT Iptables based on L3 Interface if all valid conditions are met */
void NatMgr::addStaticNaptIptables(const string port)
{
    /* Example:
     * Port is Ethernet1
     */

    bool isRulesAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAPT Iptables addition");
        return;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        /* Check interface is same as given Port, otherwise continue */
        if ((*it).second.interface != port)
        {
            continue;
        }

        isRulesAdded = true;

        if ((*it).second.twice_nat_id.empty())
        {
            /* Add the new Static Single NAPT iptables */
            SWSS_LOG_INFO("Adding the Static Single NAPT iptables for %s", (*it).first.c_str());
            addStaticSingleNaptIptables((*it).first);
        }
        else
        {
            /* Add the new Static Twice NAPT iptables */
            SWSS_LOG_INFO("Adding the Static Twice NAPT iptables for %s", (*it).first.c_str());
            addStaticTwiceNaptIptables((*it).first);
        }
    }

    if (!isRulesAdded)
    {
        SWSS_LOG_INFO("No Static NAPT iptables rules to add");
    }
}

/* To add Static Single NAPT Iptables based on Static Key */
void NatMgr::addStaticSingleNaptIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string prototype = EMPTY_STRING, interface = m_staticNaptEntry[key].interface;;
    vector<string> keys = tokenize(key, config_db_key_delimiter);

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT Iptables addition ", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    /* Add Static NAPT iptables rule */
    if (!setStaticNaptIptablesRules(INSERT, interface, prototype, keys[0], keys[2],
                                    m_staticNaptEntry[key].local_ip, m_staticNaptEntry[key].local_port,
                                    m_staticNaptEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to add Static NAPT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Added Static NAPT iptables rules for %s", key.c_str());
    }
}

/* To add Static Twice NAPT Iptables based on Static Key if all valid conditions are met */
void NatMgr::addStaticTwiceNaptIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string interface = EMPTY_STRING, prototype = EMPTY_STRING;
    vector<string> keys = tokenize(key, config_db_key_delimiter);
    string src, translated_src, dest, translated_dest;
    string src_port, translated_src_port, dest_port, translated_dest_port;
    bool isRulesAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT entry addition to APPL_DB", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<string> entry_keys = tokenize((*it).first, config_db_key_delimiter);

        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check for both protocols are same, otherwise continue */
        if (entry_keys[1] != keys[1])
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if (!(*it).second.twice_nat_added or !m_staticNaptEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = keys[0];
            src_port = keys[2];
            dest = entry_keys[0];
            dest_port = entry_keys[2];
            translated_src = m_staticNaptEntry[key].local_ip;
            translated_src_port = m_staticNaptEntry[key].local_port;
            translated_dest = (*it).second.local_ip;
            translated_dest_port = (*it).second.local_port;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = entry_keys[0];
            src_port = entry_keys[2];
            dest = keys[0];
            dest_port = keys[2];
            translated_src = (*it).second.local_ip;
            translated_src_port = (*it).second.local_port;
            translated_dest = m_staticNaptEntry[key].local_ip;
            translated_dest_port = m_staticNaptEntry[key].local_port;
            interface = m_staticNaptEntry[key].interface;
        }

        /* Add Static NAPT iptables rule */
        if (!setStaticTwiceNaptIptablesRules(INSERT, interface, prototype, src, src_port, translated_src, translated_src_port,
            dest, dest_port, translated_dest, translated_dest_port))
        {
            SWSS_LOG_ERROR("Failed to add Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            isRulesAdded = true;
            SWSS_LOG_INFO("Added Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        break;
    }

    if (!isRulesAdded)
    {
        SWSS_LOG_INFO("No Static Twice NAPT iptables rules to add");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_name = (*it).second.pool_name;
        string port_range = EMPTY_STRING;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if (!(*it).second.twice_nat_added or !m_staticNaptEntry[key].twice_nat_added)
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (port_range.empty() or (port_range == "NULL"))
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules addition for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        isRulesAdded = true;
        setDynamicAllForwardOrAclbasedRules(ADD, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, (*it).second.acl_name,
                                            (*it).first);
        break;
    }

    if (!isRulesAdded)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAPT iptables rules to add");
    }
}

/* To delete Static NAT Iptables based on L3 Interface if all valid conditions are met */
void NatMgr::removeStaticNatIptables(const string port)
{
    /* Example:
     * Port is Ethernet1
     */

    bool isRulesDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAT iptables deletion");
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        /* Check interface is matching, otherwise continue */
        if ((*it).second.interface != port)
        {
            continue;
        }

        isRulesDeleted = true;

        if ((*it).second.twice_nat_id.empty())
        {
            /* Remove the Static Single NAT Iptables */
            SWSS_LOG_INFO("Deleting the Static Single NAT Iptables for %s", (*it).first.c_str());
            removeStaticSingleNatIptables((*it).first);
        }
        else if (((*it).second.twice_nat_added == true))
        {
            /* Remove the Static Twice NAT Iptables */
            SWSS_LOG_INFO("Deleting the Static Twice NAT Iptables for %s", (*it).first.c_str());
            removeStaticTwiceNatIptables((*it).first);
        }
    }

    if (!isRulesDeleted)
    {
        SWSS_LOG_INFO("No Static NAT iptables rules to delete");
    }
}

/* To delete Static Single NAT Iptables based on Static Key if all valid conditions are met */
void NatMgr::removeStaticSingleNatIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string interface = m_staticNatEntry[key].interface;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Static NAT iptables deletion", key.c_str());
        return;
    }
    
    /* Remove Static NAT iptables rule */
    if (!setStaticNatIptablesRules(DELETE, interface, key, m_staticNatEntry[key].local_ip, m_staticNatEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to delete Static NAT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Deleted Static NAT iptables rules for %s", key.c_str());
    }
}

/* To delete Static Twice NAT Iptables based on Static Key if all valid conditions are met */
void NatMgr::removeStaticTwiceNatIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAT|65.55.42.1 and key is 65.55.42.1
     */

    string interface = EMPTY_STRING;
    string src, translated_src, dest, translated_dest;
    bool isRulesDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAT iptables deletion", key.c_str());
        return;
    }

    /* Get all the Static NAT entries */
    for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
    {
        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNatEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNatEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = key;
            dest = (*it).first;
            translated_src = m_staticNatEntry[key].local_ip;
            translated_dest = (*it).second.local_ip;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNatEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = (*it).first;
            dest = key;
            translated_src = (*it).second.local_ip;
            translated_dest = m_staticNatEntry[key].local_ip;
            interface = m_staticNatEntry[key].interface;
        }

        /* Delete Static NAT iptables rule */
        if (!setStaticTwiceNatIptablesRules(DELETE, interface, src, translated_src, dest, translated_dest))
        {
            SWSS_LOG_ERROR("Failed to delete Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            isRulesDeleted = true;
            SWSS_LOG_INFO("Deleted Static Twice NAT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        break;
    }

    if (!isRulesDeleted)
    {
        SWSS_LOG_INFO("No Static Twice NAT iptables rules to delete");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string port_range = EMPTY_STRING;
        string pool_name = (*it).second.pool_name;
        string acls_name = (*it).second.acl_name;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNatEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNatEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNatEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is not present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (!port_range.empty() and (port_range != "NULL"))
        {
            continue;
        }

        /* Check the key is matching, otherwise continue */
        if (m_staticNatEntry[key].binding_key != (*it).first)
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules deletion for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        /* Delete Dynamic rules */
        isRulesDeleted = true;
        setDynamicAllForwardOrAclbasedRules(DELETE, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, acls_name, (*it).first);
        break;
    }

    if (!isRulesDeleted)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAT iptables rules to delete");
    }
}

/* To delete Static NAPT iptables based on L3 Interface if all valid conditions are met */
void NatMgr::removeStaticNaptIptables(const string port)
{
    /* Example:
     * Port is Ethernet1
     */

    bool isRulesDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping NAPT iptables deletion");
        return;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        /* Check interface is matching, otherwise continue */
        if ((*it).second.interface != port)
        {
            continue;
        }

        isRulesDeleted = true;

        if ((*it).second.twice_nat_id.empty())
        {
            /* Remove the Static Single NAPT Iptables */
            SWSS_LOG_INFO("Deleting the Static Single NAPT Iptables for %s", (*it).first.c_str());
            removeStaticSingleNaptIptables((*it).first);
        }
        else if (((*it).second.twice_nat_added == true))
        {
            /* Remove the Static Twice NAPT Iptables */
            SWSS_LOG_INFO("Deleting the Static Twice NAPT iptables for %s", (*it).first.c_str());
            removeStaticTwiceNaptIptables((*it).first);
        }
    }

    if (!isRulesDeleted)
    {
        SWSS_LOG_INFO("No Static NAPT iptables rules to delete");
    }
}

/* To delete Static Single NAPT Iptables based on Static Key if all valid conditions are met */
void NatMgr::removeStaticSingleNaptIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string interface = EMPTY_STRING, prototype = EMPTY_STRING;
    vector<string> keys = tokenize(key, config_db_key_delimiter);

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Single NAPT iptables deletion", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    interface = m_staticNaptEntry[key].interface;

    /* Remove Static NAPT iptables rule */
    if (!setStaticNaptIptablesRules(DELETE, interface, prototype, keys[0], keys[2],
                                    m_staticNaptEntry[key].local_ip, m_staticNaptEntry[key].local_port,
                                    m_staticNaptEntry[key].nat_type))
    {
        SWSS_LOG_ERROR("Failed to delete Static NAPT iptables rules for %s", key.c_str());
    }
    else
    {
        SWSS_LOG_INFO("Deleted Static NAPT iptables rules for %s", key.c_str());
    }
}

/* To delete Static Twice NAPT Iptables based on Static Key if all valid conditions are met */
void NatMgr::removeStaticTwiceNaptIptables(const string &key)
{
    /* Example:
     * Entry is STATIC_NAPT|65.55.42.1|TCP|1024 and key is 65.55.42.1|TCP|1024
     */

    string interface = EMPTY_STRING, prototype = EMPTY_STRING;
    vector<string> keys = tokenize(key, config_db_key_delimiter);
    string src, translated_src, dest, translated_dest;
    string src_port, translated_src_port, dest_port, translated_dest_port;
    bool isRulesDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping %s Twice NAPT iptables deletion", key.c_str());
        return;
    }

    if (keys[1] == to_upper(IP_PROTOCOL_UDP))
    {
        prototype = IP_PROTOCOL_UDP;
    }
    else if (keys[1] == to_upper(IP_PROTOCOL_TCP))
    {
        prototype = IP_PROTOCOL_TCP;
    }

    /* Get all the Static NAPT entries */
    for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
    {
        vector<string> entry_keys = tokenize((*it).first, config_db_key_delimiter);

        /* Check for other entries, otherwise continue */
        if ((*it).first == key)
        {
            continue;
        }

        /* Check for both protocols are same, otherwise continue */
        if (entry_keys[1] != keys[1])
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNaptEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check the nat type is different, otherwise continue */
        if ((*it).second.nat_type == m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        if (((*it).second.nat_type == DNAT_NAT_TYPE) and
            (m_staticNaptEntry[key].nat_type == SNAT_NAT_TYPE))
        {
            src = keys[0];
            src_port = keys[2];
            dest = entry_keys[0];
            dest_port = entry_keys[2];
            translated_src = m_staticNaptEntry[key].local_ip;
            translated_src_port = m_staticNaptEntry[key].local_port;
            translated_dest = (*it).second.local_ip;
            translated_dest_port = (*it).second.local_port;
            interface = (*it).second.interface;
        }
        else if (((*it).second.nat_type == SNAT_NAT_TYPE) and
                 (m_staticNaptEntry[key].nat_type == DNAT_NAT_TYPE))
        {
            src = entry_keys[0];
            src_port = entry_keys[2];
            dest = keys[0];
            dest_port = keys[2];
            translated_src = (*it).second.local_ip;
            translated_src_port = (*it).second.local_port;
            translated_dest = m_staticNaptEntry[key].local_ip;
            translated_dest_port = m_staticNaptEntry[key].local_port;
            interface = m_staticNaptEntry[key].interface;
        }

        /* Delete Static NAPT iptables rule */
        if (!setStaticTwiceNaptIptablesRules(DELETE, interface, prototype, src, src_port, translated_src, translated_src_port,
                                             dest, dest_port, translated_dest, translated_dest_port))
        {
            SWSS_LOG_ERROR("Failed to delete Static Twice NAPT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        else
        {
            isRulesDeleted = true;
            SWSS_LOG_INFO("Deleted Static Twice NAPT iptables rules for %s and %s", key.c_str(), (*it).first.c_str());
        }
        break;
    }

    if (!isRulesDeleted)
    {
        SWSS_LOG_INFO("No Static Twice NAPT iptables rules to delete");
    }
    else
    {
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string port_range = EMPTY_STRING;
        string pool_name = (*it).second.pool_name;
        string acls_name = (*it).second.acl_name;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check the twice_nat_id is matched, otherwise continue */
        if ((*it).second.twice_nat_id != m_staticNaptEntry[key].twice_nat_id)
        {
            continue;
        }

        /* Check the twice NAT is added, otherwise continue */
        if ((!(*it).second.twice_nat_added) or (!m_staticNaptEntry[key].twice_nat_added))
        {
            continue;
        }

        /* Check the nat type is same, otherwise continue */
        if ((*it).second.nat_type != m_staticNaptEntry[key].nat_type)
        {
            continue;
        }

        /* Check the port_range is present, otherwise continue */
        port_range = m_natPoolInfo[pool_name].port_range;
        if (port_range.empty() or (port_range == "NULL"))
        {
            continue;
        }

        /* Check the key is matching, otherwise continue */
        if (m_staticNaptEntry[key].binding_key != (*it).first)
        {
            continue;
        }
        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic twice nat iptables rules deletion for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        /* Delete Dynamic rules */
        isRulesDeleted = true;
        setDynamicAllForwardOrAclbasedRules(DELETE, (*it).second.pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, acls_name, (*it).first);
        break;
    }

    if (!isRulesDeleted)
    {
        SWSS_LOG_INFO("No Static-Dynamic Twice NAPT iptables rules to delete");
    }
}

/* To Add or Delete Dynamic NAT/NAPT iptables rules if all valid conditions are met */
void NatMgr::setDynamicAllForwardOrAclbasedRules(const string &opCmd, const string &pool_interface, const string &ip_range,
                                                 const string &port_range, const string &aclsName, 
                                                 const string &dynamicKey)
{
    vector<string> access_list;
    bool setAllForwardRules = true;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping Dynamic Iptables setting");
        return;
    }

    /* Add/Delete ACLs based only when aclName string is not empty */
    if (((opCmd == DELETE) and (m_natBindingInfo[dynamicKey].acl_interface != NONE_STRING) and (aclsName != EMPTY_STRING)) or
        ((opCmd == ADD) and (aclsName != EMPTY_STRING)))
    {
        access_list = tokenize(aclsName, comma);

        /* Loop for all ACL Ids */
        for (string aclId : access_list)
        {
            /* Check the Acl-id is enabled otherwise continue */
            if (m_natAclTableInfo.find(aclId) == m_natAclTableInfo.end())
            {
                SWSS_LOG_INFO("Acl-id %s is not yet enabled, skipping it", aclId.c_str());
                continue;
            }

            SWSS_LOG_INFO("Acl-id %s is enabled", aclId.c_str());

            bool isRuleSet = false;

            /* Get all ACL Rule Info */
            for (auto it = m_natAclRuleInfo.begin(); it != m_natAclRuleInfo.end(); it++)
            {
                vector<string> aclRuleKeys = tokenize((*it).first, config_db_key_delimiter);

                /* Check aclId is matching, otherwise continue */
                if (aclRuleKeys[0] != aclId)
                {
                    continue;
                }

                SWSS_LOG_INFO("Rule-id %s is mapped to Acl-id %s", aclRuleKeys[1].c_str(), aclId.c_str());

                /* Set pool ip to APPL_DB */
                setNaptPoolIpTable(opCmd, ip_range, port_range);

                /* Set dynamic iptables rule with acls*/
                if (!setDynamicNatIptablesRulesWithAcl(opCmd, pool_interface, ip_range, port_range, (*it).second, m_natBindingInfo[dynamicKey].static_key))
                {
                    SWSS_LOG_ERROR("Failed to %s dynamic iptables acl rules for Rule id %s for Table %s", opCmd == ADD ? "add" : "delete",
                                   aclRuleKeys[1].c_str(), aclId.c_str());
                }
                else
                {
                    isRuleSet = true;
                    SWSS_LOG_INFO("%s dynamic iptables acl rules for Rule id %s for Table %s", opCmd == ADD ? "Added" : "Deleted",
                                  aclRuleKeys[1].c_str(), aclId.c_str());
                }

                setAllForwardRules = false;
            }

            /* If rule is set, save the port in the binding cache */
            if (isRuleSet and (opCmd == ADD))
            {
                if (m_natBindingInfo[dynamicKey].acl_interface == NONE_STRING)
                {
                    m_natBindingInfo[dynamicKey].acl_interface = m_natAclTableInfo[aclId];
                }
                else
                {
                    m_natBindingInfo[dynamicKey].acl_interface += (comma + m_natAclTableInfo[aclId]);
                }
            }
        }
      
        /* After deletion, set acl_interface to None */  
        if (opCmd == DELETE)
        {
            m_natBindingInfo[dynamicKey].acl_interface == NONE_STRING;
        }
    }

    /* To set all forward rules */
    if (setAllForwardRules)
    {
        /* Set pool ip to APPL_DB */
        setNaptPoolIpTable(opCmd, ip_range, port_range);

        /* Set dynamic iptables rule without acls*/
        if (!setDynamicNatIptablesRulesWithoutAcl(opCmd, pool_interface, ip_range, port_range, m_natBindingInfo[dynamicKey].static_key))
        {
            SWSS_LOG_ERROR("Failed to %s dynamic iptables rules for %s", opCmd == ADD ? "add" : "delete", dynamicKey.c_str());
        }
        else
        {
            SWSS_LOG_INFO("%s dynamic iptables rules for %s", opCmd == ADD ? "Added" : "Deleted", dynamicKey.c_str());
        }
    }
}

/* To Add Dynamic NAT rules based on Binding Key if all valid conditions are met */
void NatMgr::addDynamicNatRule(const string &key)
{
    /* Example:
     * Entry is NAT_BINDINGS|BindingName and key is BindingName
     */

    string pool_interface = EMPTY_STRING;
    string pool_name = m_natBindingInfo[key].pool_name;
    string acls_name = m_natBindingInfo[key].acl_name;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic nat rules addition for %s", key.c_str());
        return;
    }

    /* Check the pool is present in cache, otherwise return */
    if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
    {
        SWSS_LOG_INFO("Pool %s is not yet enabled, skipping dynamic nat rules addition for %s", pool_name.c_str(), key.c_str());
        return;
    }

    vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

    /* Check the pool is valid */
    if (nat_ip.empty())
    {
        SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic nat rules addition for %s", pool_name.c_str(), key.c_str());
        return;
    }

    /* Get the matching Ip interface otherwise return */
    if (!getIpEnabledIntf(nat_ip[0], pool_interface))
    {
        SWSS_LOG_INFO("L3 Interface is not yet enabled for %s, skipping dynamic nat rules addition", key.c_str());
        return;
    }

    m_natBindingInfo[key].pool_interface = pool_interface;

    if (m_natBindingInfo[key].twice_nat_id.empty())
    {
        /* Add Dynamic rules for Single NAT */
        SWSS_LOG_INFO("Adding dynamic single nat rules for %s", key.c_str());   
        setDynamicAllForwardOrAclbasedRules(ADD, pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, acls_name, key);
    }
    else
    {
        /* Add Dynamic rules for Twice NAT */
        SWSS_LOG_INFO("Adding dynamic twice nat rules for %s", key.c_str());
        addDynamicTwiceNatRule(key);
    }
}

/* To delete Dynamic NAT/NAPT iptables rules based on Binding Key if all valid conditions are met */
void NatMgr::removeDynamicNatRule(const string &key)
{
    /* Example:
     * Entry is NAT_BINDINGS|BindingName and key is BindingName
     */

    string pool_interface = m_natBindingInfo[key].pool_interface;
    string pool_name = m_natBindingInfo[key].pool_name;
    string acls_name = m_natBindingInfo[key].acl_name;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic nat rules deletion for %s", key.c_str());
        return;
    }

    /* Check the pool is present in cache otherwise return */
    if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
    {
        SWSS_LOG_INFO("Pool %s is not yet enabled, skipping dynamic nat rules deletion for %s", pool_name.c_str(), key.c_str());
        return;
    }

    /* Check pool interface is valid, otherwise return */
    if (m_natBindingInfo[key].pool_interface == NONE_STRING)
    {
        SWSS_LOG_INFO("Pool interface is not enabled, skipping dynamic nat rules deletion for %s", key.c_str());
        return;
    }

    vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

    /* Check the pool is valid */
    if (nat_ip.empty())
    {
        SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic nat rules deletion for %s", pool_name.c_str(), key.c_str());
        return;
    }

    if (m_natBindingInfo[key].twice_nat_id.empty())
    {
        /* Delete Dynamic rules for Single NAT */
        SWSS_LOG_INFO("Deleting dynamic single nat rules for %s", key.c_str());
        setDynamicAllForwardOrAclbasedRules(DELETE, pool_interface, m_natPoolInfo[pool_name].ip_range,
                                            m_natPoolInfo[pool_name].port_range, acls_name, key);
    }
    else
    {
        /* Delete Dynamic rules for Twice NAT */
        SWSS_LOG_INFO("Deleting dynamic twice nat rules for %s", key.c_str());
        deleteDynamicTwiceNatRule(key);
    }

    m_natBindingInfo[key].pool_interface = NONE_STRING;
    m_natBindingInfo[key].acl_interface = NONE_STRING;
}

/* To Add Dynamic NAT/NAPT iptables rules based on ACLs if all valid conditions are met */
void NatMgr::addDynamicNatRuleByAcl(const string &aclKey, bool isRuleId)
{
    /* Example:
     * Key : (AclId | AclRuleId) or (AclId)
     */

    string aclTableId, aclRuleId;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic iptables rules addition for %s", aclKey.c_str());
        return;
    }

    /* Get the aclTable and aclRule */
    if (isRuleId == true)
    {
        vector<string> keys = tokenize(aclKey, config_db_key_delimiter); 
        aclTableId = keys[0];
        aclRuleId = keys[1];   
    }
    else
    {
        aclTableId = aclKey;
    }

    /* Get all binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_name = (*it).second.pool_name;
        string acls_name = (*it).second.acl_name;
        string poolInterface, aclInterface;
        string port_range, ip_range;
        bool isRuleSet = false;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check if pool interface is valid, otherwise continue */
        if ((*it).second.pool_interface == NONE_STRING)
        {
            continue;
        }

        /* Check the ACL Ids are configured, otherwise continue */
        if ((*it).second.acl_name == EMPTY_STRING)
        {
            continue;
        }

        /* Check if the twice nat id is configured and not added */
        if ((!(*it).second.twice_nat_id.empty()) and ((*it).second.twice_nat_added))
        {
            continue;
        }

        poolInterface = (*it).second.pool_interface;
        aclInterface = (*it).second.acl_interface;
        ip_range = m_natPoolInfo[pool_name].ip_range;
        port_range = m_natPoolInfo[pool_name].port_range;

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic iptables rules addition for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        vector<string> access_list = tokenize(acls_name, comma);

        /* Get all aclIds */
        for (string aclId : access_list)
        {
            /* Check the Acl-id is matching with given one, otherwise continue */
            if (aclTableId != aclId)
            {
                continue;
            }

            /* isRuleId is true means we have both AclTableId and AclRuleId */
            if (isRuleId == true)
            {
                /* Check the Acl-id is enabled otherwise continue */
                if (m_natAclTableInfo.find(aclTableId) == m_natAclTableInfo.end())
                {
                    SWSS_LOG_INFO("Acl-id %s is not yet enabled", aclTableId.c_str());
                    return;
                }

                /* aclInterface is None means delete all forward rule first, otherwise nothing */
                if (aclInterface == NONE_STRING)
                {
                    /* Set pool ip to APPL_DB */
                    setNaptPoolIpTable(DELETE, ip_range, port_range);

                    /* Set dynamic iptables rule without acl */
                    if (!setDynamicNatIptablesRulesWithoutAcl(DELETE, poolInterface, ip_range, port_range, (*it).second.static_key))
                    {
                        SWSS_LOG_ERROR("Failed to remove dynamic iptables rules for %s", aclKey.c_str());
                    }
                    else
                    {
                        SWSS_LOG_INFO("Deleted dynamic iptables rules for %s", aclKey.c_str());
                    }

                    (*it).second.acl_interface = m_natAclTableInfo[aclTableId];                    
                }
                else
                {
                    (*it).second.acl_interface = comma + m_natAclTableInfo[aclTableId];
                }

                /* Set pool ip to APPL_DB */
                setNaptPoolIpTable(ADD, ip_range, port_range);

                /* Set dynamic iptables rule with acls*/
                if (!setDynamicNatIptablesRulesWithAcl(ADD, poolInterface, ip_range, port_range, m_natAclRuleInfo[aclKey], (*it).second.static_key))
                {
                    SWSS_LOG_ERROR("Failed to add dynamic iptables acl rules for Rule id %s for Table %s", aclRuleId.c_str(), aclTableId.c_str());
                }
                else
                {
                    SWSS_LOG_INFO("Added dynamic iptables acl rules for Rule id %s for Table %s", aclRuleId.c_str(), aclTableId.c_str());
                }
                return;
            }
            else
            {
                /* Get all AclRule Info */
                for (auto it2 = m_natAclRuleInfo.begin(); it2 != m_natAclRuleInfo.end(); it2++)
                {
                    vector<string> aclRuleKeys = tokenize((*it2).first, config_db_key_delimiter);

                    /* Check the matching aclTableId, otherwise continue */
                    if (aclRuleKeys[0] != aclTableId)
                    {
                        continue;
                    }

                    SWSS_LOG_INFO("Rule-id %s is mapped to Acl-id %s", aclRuleKeys[1].c_str(), aclTableId.c_str());

                    /* Set pool ip to APPL_DB */
                    setNaptPoolIpTable(ADD, ip_range, port_range);

                    /* Add dynamic iptables rule with acls */
                    if (!setDynamicNatIptablesRulesWithAcl(ADD, poolInterface, ip_range, port_range, (*it2).second, (*it).second.static_key))
                    {
                        SWSS_LOG_ERROR("Failed to add dynamic iptables acl rules for Rule id %s for Table %s", aclRuleKeys[1].c_str(), aclTableId.c_str());
                    }
                    else
                    {
                        isRuleSet = true;
                        SWSS_LOG_INFO("Added dynamic iptables acl rules for Rule id %s for Table %s", aclRuleKeys[1].c_str(), aclTableId.c_str());
                    }
                }
      
                /* aclInterface is None means have to delete the All forward rules */
                if ((aclInterface == NONE_STRING) and (isRuleSet == true))
                {
                    /* Set pool ip to APPL_DB */
                    setNaptPoolIpTable(DELETE, ip_range, port_range);

                    /* Delete dynamic iptables rule without acl */
                    if (!setDynamicNatIptablesRulesWithoutAcl(DELETE, poolInterface, ip_range, port_range, (*it).second.static_key))
                    {
                        SWSS_LOG_ERROR("Failed to remove dynamic iptables rules for %s", aclKey.c_str());
                    }
                    else
                    {
                        SWSS_LOG_INFO("Deleted dynamic iptables rules for %s", aclKey.c_str());
                    }
                    
                    (*it).second.acl_interface = m_natAclTableInfo[aclTableId];
                }
                return;
            }
        }
    }
}

/* To Delete Dynamic NAT/NAPT iptables rules based on ACLs if all valid conditions are met */
void NatMgr::removeDynamicNatRuleByAcl(const string &aclKey, bool isRuleId)
{
    /* Example:
     * Key : (AclId | AclRuleId) or (AclId)
     */

    string aclTableId, aclRuleId;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic nat rules deletion for %s", aclKey.c_str());
        return;
    }

    /* Get the aclTable and aclRule */
    if (isRuleId == true)
    {
        vector<string> keys = tokenize(aclKey, config_db_key_delimiter);
        aclTableId = keys[0];
        aclRuleId = keys[1];
    }
    else
    {
        aclTableId = aclKey;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_name = (*it).second.pool_name;
        string acls_name = (*it).second.acl_name;
        string poolInterface, aclInterface;
        string port_range, ip_range;
        bool isRuleSet = false, isRulePresent = false;

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check if pool interface is valid, otherwise continue */   
        if ((*it).second.pool_interface == NONE_STRING)
        {
            continue;
        }

        /* Check the ACL Ids are configured, otherwise continue */
        if ((*it).second.acl_name == EMPTY_STRING)
        {
            continue;
        }

        /* Check if the twice nat id is added */
        if ((!(*it).second.twice_nat_id.empty()) and (!(*it).second.twice_nat_added))
        {
            continue;
        }

        poolInterface = (*it).second.pool_interface;
        aclInterface = (*it).second.acl_interface;
        ip_range = m_natPoolInfo[pool_name].ip_range;
        port_range = m_natPoolInfo[pool_name].port_range;

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);
 
        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic nat rules deletion for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        vector<string> access_list = tokenize(acls_name, ',');

        /* Get all aclIds */
        for (string aclId : access_list)
        {
            /* Check the aclId is matching with given one, otherwise continue */
            if (aclTableId != aclId)
            {
                continue;
            }

            /* isRuleId is true means we have both AclTableId and AclRuleId */
            if (isRuleId == true)
            {
                /* Check the Acl-id is enabled otherwise continue */
                if (m_natAclTableInfo.find(aclTableId) == m_natAclTableInfo.end())
                {
                    SWSS_LOG_INFO("Acl-id %s is not yet enabled", aclTableId.c_str());
                    return;
                }

                /* Set pool ip to APPL_DB */
                setNaptPoolIpTable(DELETE, ip_range, port_range);

                /* Delete dynamic iptables rule with acls*/
                if (!setDynamicNatIptablesRulesWithAcl(DELETE, poolInterface, ip_range, port_range, m_natAclRuleInfo[aclKey], (*it).second.static_key))
                {
                    SWSS_LOG_ERROR("Failed to delete dynamic iptables acl rules for Rule id %s for Table %s", aclRuleId.c_str(), aclTableId.c_str());
                }
                else
                {
                    SWSS_LOG_INFO("Deleted dynamic iptables acl rules for Rule id %s for Table %s", aclRuleId.c_str(), aclTableId.c_str());
                }

                /* Check any other rule matching in same Table-Id */
                for (auto it = m_natAclRuleInfo.begin(); it != m_natAclRuleInfo.end(); it++)
                {
                    vector<string> aclRuleKeys = tokenize((*it).first, config_db_key_delimiter);

                    /* Check the matching aclTableId otherwise continue */
                    if (aclRuleKeys[0] != aclTableId)
                    {
                        continue;
                    }

                    SWSS_LOG_INFO("Rule-id %s is mapped to Acl-id %s", aclRuleKeys[1].c_str(), aclTableId.c_str());
                    if (aclRuleKeys[1] == aclRuleId)
                    {
                        continue;
                    }
                    isRulePresent = true;
                }

                if (isRulePresent == false)
                {
                    /* Set pool ip to APPL_DB */
                    setNaptPoolIpTable(ADD, ip_range, port_range);

                    /* Set dynamic iptables rule without acl */
                    if (!setDynamicNatIptablesRulesWithoutAcl(ADD, poolInterface, ip_range, port_range, (*it).second.static_key))
                    {
                        SWSS_LOG_ERROR("Failed to add dynamic iptables rules for %s", aclKey.c_str());
                    }
                    else
                    {
                        SWSS_LOG_INFO("Added dynamic iptables rules for %s", aclKey.c_str());
                    }

                    (*it).second.acl_interface = NONE_STRING;
                }
                return;
            }
            else
            {
                /* Get all AclRule Info */
                for (auto it2 = m_natAclRuleInfo.begin(); it2 != m_natAclRuleInfo.end(); it2++)
                {
                    vector<string> aclRuleKeys = tokenize((*it2).first, config_db_key_delimiter);

                    /* Check the matching aclTableId, otherwise continue */
                    if (aclRuleKeys[0] != aclTableId)
                    {
                        continue;
                    }

                    SWSS_LOG_INFO("Rule-id %s is mapped to Acl-id %s", aclRuleKeys[1].c_str(), aclTableId.c_str());

                    /* Set pool ip to APPL_DB */
                    setNaptPoolIpTable(DELETE, ip_range, port_range);

                    /* Delete dynamic iptables rule with acls */
                    if (!setDynamicNatIptablesRulesWithAcl(DELETE, poolInterface, ip_range, port_range, (*it2).second, (*it).second.static_key))
                    {
                        SWSS_LOG_ERROR("Failed to delete dynamic iptables acl rules for Rule id %s for Table %s", aclRuleKeys[1].c_str(), aclTableId.c_str());
                    }
                    else
                    {
                        isRuleSet = true;
                        SWSS_LOG_INFO("Deleted dynamic iptables acl rules for Rule id %s for Table %s", aclRuleKeys[1].c_str(), aclTableId.c_str());
                    }
                }

                /* If aclInterface is not None, add dynamic all forward rules */
                if ((aclInterface != NONE_STRING) and (isRuleSet == true))
                {
                    /* Set pool ip to APPL_DB */
                    setNaptPoolIpTable(ADD, ip_range, port_range);

                    /* Add dynamic iptables rule without acl */
                    if (!setDynamicNatIptablesRulesWithoutAcl(ADD, poolInterface, ip_range, port_range, (*it).second.static_key))
                    {
                        SWSS_LOG_ERROR("Failed to add dynamic iptables rules for %s", aclKey.c_str());
                    }
                    else
                    {
                        SWSS_LOG_INFO("Added dynamic iptables rules for %s", aclKey.c_str());
                    }

                    (*it).second.acl_interface = NONE_STRING;
                }
                return;
            }
        }
    }
}

/* To Add Dynamic NAT iptables rules based on L3 Interface if all valid conditions are met */
void NatMgr::addDynamicNatRules(const string port, const string ipPrefix)
{
    /* Example:
     * Port is Ethernet1 and ipPrefix is 10.0.0.1/24
     */

    bool isRuleAdded = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic nat rules addition");
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_interface = EMPTY_STRING;
        string pool_name = (*it).second.pool_name;
 
        /* Check the pool interface is valid, otherwise continue */
        if ((*it).second.pool_interface != NONE_STRING)
        {
            continue;
        }

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check if the twice nat id is added */
        if ((!(*it).second.twice_nat_id.empty()) and ((*it).second.twice_nat_added))
        {
            continue;
        }

        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic nat rules addition for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }
 
        if ((port == NONE_STRING) and (ipPrefix == NONE_STRING))
        { 
            /* Get the matching Ip interface otherwise return */
            if (!getIpEnabledIntf(nat_ip[0], pool_interface))
            {
                continue;
            }

	    (*it).second.pool_interface = pool_interface;
        }
        else if ((port != NONE_STRING) and (ipPrefix == NONE_STRING))
        {
            /* Get the matching Ip interface otherwise return */
            if (!getIpEnabledIntf(nat_ip[0], pool_interface))
            {
                continue;
            }

            if (pool_interface != port)
            {
                continue;
            }
            (*it).second.pool_interface = pool_interface;
        }
        else
        {
            /* Check global ip address is matching otherwise continue */
            if (isGlobalIpMatching(ipPrefix, nat_ip[0]) == false)
            {
                continue;
            }

            pool_interface = port;
            (*it).second.pool_interface = port;
        }

        if ((*it).second.twice_nat_id.empty())
        {
            /* Add Dynamic rules for Single NAT */
            SWSS_LOG_INFO("Adding dynamic single nat rules for %s", (*it).first.c_str());
            setDynamicAllForwardOrAclbasedRules(ADD, pool_interface, m_natPoolInfo[pool_name].ip_range,
                                                m_natPoolInfo[pool_name].port_range, (*it).second.acl_name,
                                                (*it).first);
        }
        else
        {
            /* Add Dynamic rules for Twice NAT */
            SWSS_LOG_INFO("Adding dynamic twice nat rules for %s", (*it).first.c_str());
            addDynamicTwiceNatRule((*it).first);
        }

        if ((port != NONE_STRING) and (ipPrefix != NONE_STRING))
        {
            /* Send notification to Orchagent to flush the conntrack entries */
            std::vector<swss::FieldValueTuple> entry;
            flushNotifier->send("ENTRIES", "ALL", entry);
            SWSS_LOG_WARN("Added interface is part of Binded NAT Pool range, so it is clearing all the nat translations");
        }

        isRuleAdded = true;
    }

    if (!isRuleAdded)
    {
        SWSS_LOG_INFO("No dynamic nat rules to add");
    }
}

/* To delete Dynamic NAT/NAPT iptables rules based on L3 Interface if all valid conditions are met */
void NatMgr::removeDynamicNatRules(const string port, const string ipPrefix)
{
    /* Example:
     * Port is Ethernet1 and ipPrefix is 10.0.0.1/24
     */

    bool isRuleDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic nat rules deletion");
        return;
    }

    /* Get all Binding Info */
    for (auto it = m_natBindingInfo.begin(); it != m_natBindingInfo.end(); it++)
    {
        string pool_interface = EMPTY_STRING;
        string pool_name = m_natBindingInfo[(*it).first].pool_name;
        vector<string> nat_ip = tokenize(m_natPoolInfo[pool_name].ip_range, range_specifier);

        /* Check interface is matching, otherwise continue */
        if ((*it).second.pool_interface == NONE_STRING)
        {
            continue;
        }

        /* Check the pool is present in cache, otherwise continue */
        if (m_natPoolInfo.find(pool_name) == m_natPoolInfo.end())
        {
            continue;
        }

        /* Check if the twice nat id is added */
        if ((!(*it).second.twice_nat_id.empty()) and (!(*it).second.twice_nat_added))
        {
            (*it).second.pool_interface = NONE_STRING;
            continue;
        }

        /* Check the pool is valid */
        if (nat_ip.empty())
        {
            SWSS_LOG_INFO("NAT pool %s is not valid, skipping dynamic rules deletion for %s", pool_name.c_str(), (*it).first.c_str());
            continue;
        }

        if ((port == NONE_STRING) and (ipPrefix == NONE_STRING))
        {
            pool_interface = (*it).second.pool_interface;
        }
        else if ((port != NONE_STRING) and (ipPrefix == NONE_STRING))
        {
            /* Check interface is matching, otherwise continue */
            if ((*it).second.pool_interface != port)
            {
                continue;
            }

            pool_interface = port;
        }
        else
        {
            /* Check interface is matching, otherwise continue */
            if ((*it).second.pool_interface != port)
            {
                continue;
            }

            /* Check the global ip address is matching otherwise continue */
            if (isGlobalIpMatching(ipPrefix, nat_ip[0]) == false)
            {
                continue;
            }

            pool_interface = port;
        }

        if ((*it).second.twice_nat_id.empty())
        {
            /* Delete Dynamic rules for Single NAT */
            SWSS_LOG_INFO("Deleting dynamic single nat rules for %s", (*it).first.c_str());
            setDynamicAllForwardOrAclbasedRules(DELETE, pool_interface, m_natPoolInfo[pool_name].ip_range,
                                                m_natPoolInfo[pool_name].port_range, (*it).second.acl_name,
                                                (*it).first);
        }
        else
        {
            /* Delete Dynamic rules for Twice NAT */
            SWSS_LOG_INFO("Deleting dynamic twice nat rules for %s", (*it).first.c_str());
            deleteDynamicTwiceNatRule((*it).first);
        }

        if ((port != NONE_STRING) and (ipPrefix != NONE_STRING))
        {
            deleteConntrackDynamicEntries(m_natPoolInfo[pool_name].ip_range); 
        }

        (*it).second.pool_interface = NONE_STRING;
        isRuleDeleted = true;
    }

    if (!isRuleDeleted)
    {
        SWSS_LOG_INFO("No dynamic nat rules to delete");
    }
}

/* To Add Dynamic Twice NAT/NAPT iptables rules based on Binding Key if all valid conditions are met */
void NatMgr::addDynamicTwiceNatRule(const string &key)
{
    /* Example:
     * Entry is NAT_BINDINGS|BindingName and key is BindingName
     */

    string port_range = EMPTY_STRING;
    string pool_name = m_natBindingInfo[key].pool_name;
    string acls_name = m_natBindingInfo[key].acl_name;
    bool isRuleAdded = false;
 
    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic twice nat rules addition for %s", key.c_str());
        return;
    }

    /* Check the twice NAT is added, otherwise return */
    if ((m_natBindingInfo[key].twice_nat_added) or (!m_natBindingInfo[key].static_key.empty()))
    {
        SWSS_LOG_INFO("Twice NAT is already added, skipping dynamic twice nat rules addition for %s", key.c_str());
        return;
    }

    port_range = m_natPoolInfo[pool_name].port_range;

    if (!port_range.empty() and (port_range != "NULL"))
    {
        /* Check with Static NAPT entries */
        for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
        {
            /* Check the twice_nat_id is matched, otherwise continue */
            if (m_natBindingInfo[key].twice_nat_id != (*it).second.twice_nat_id)
            {
                continue;
            }

            /* Check the twice NAT is not added, otherwise continue */
            if (((*it).second.twice_nat_added) or (!(*it).second.binding_key.empty()))
            {
                continue;
            }

            /* Check interface is assigned, otherwise continue */
            if ((m_natBindingInfo[key].pool_interface == NONE_STRING) or ((*it).second.interface == NONE_STRING))
            {
                continue;
            }

            /* Check the nat type is equal, otherwise continue */
            if (m_natBindingInfo[key].nat_type != (*it).second.nat_type)
            {
                continue;
            }

            (*it).second.twice_nat_added = true;
            (*it).second.binding_key = key;
            m_natBindingInfo[key].twice_nat_added = true;
            m_natBindingInfo[key].static_key = (*it).first;

            /* Add Dynamic rules */
            setDynamicAllForwardOrAclbasedRules(ADD, m_natBindingInfo[key].pool_interface, m_natPoolInfo[pool_name].ip_range,
                                                m_natPoolInfo[pool_name].port_range, acls_name, key);

            isRuleAdded = true;
            break;
        }
    }
    else
    { 
        /* Get all the Static NAT entries */
        for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
        {
            /* Check the twice_nat_id is matched, otherwise continue */
            if (m_natBindingInfo[key].twice_nat_id != (*it).second.twice_nat_id)
            {
                continue;
            }

            /* Check the twice NAT is not added, otherwise continue */
            if (((*it).second.twice_nat_added) or (!(*it).second.binding_key.empty()))
            {
                continue;
            }

            /* Check interface is assigned, otherwise continue */
            if ((m_natBindingInfo[key].pool_interface == NONE_STRING) or ((*it).second.interface == NONE_STRING))
            {
                continue;
            }

            /* Check the nat type is equal, otherwise continue */
            if (m_natBindingInfo[key].nat_type != (*it).second.nat_type)
            {
                continue;
            }

            (*it).second.twice_nat_added = true;
            (*it).second.binding_key = key;
            m_natBindingInfo[key].twice_nat_added = true;
            m_natBindingInfo[key].static_key = (*it).first;

            /* Add Dynamic rules */
            setDynamicAllForwardOrAclbasedRules(ADD, m_natBindingInfo[key].pool_interface, m_natPoolInfo[pool_name].ip_range,
                                                m_natPoolInfo[pool_name].port_range, acls_name, key);

            isRuleAdded = true;
            break;
        }
    }

    if (!isRuleAdded)
    {
        SWSS_LOG_INFO("No dynamic twice nat rules to add");
    }
}

/* To Delete Dynamic Twice NAT/NAPT iptables rules based on Binding Key if all valid conditions are met */
void NatMgr::deleteDynamicTwiceNatRule(const string &key)
{
    /* Example:
     * Entry is NAT_BINDINGS|BindingName and key is BindingName
     */

    string port_range = EMPTY_STRING;
    string pool_name = m_natBindingInfo[key].pool_name;
    string acls_name = m_natBindingInfo[key].acl_name;
    bool isRuleDeleted = false;

    /* Check the NAT is enabled, otherwise return */
    if (!isNatEnabled())
    {
        SWSS_LOG_INFO("NAT is not yet enabled, skipping dynamic twice nat rules deletion for %s", key.c_str());
        return;
    }

    /* Check the twice NAT is not added, otherwise return */
    if ((!m_natBindingInfo[key].twice_nat_added) or (m_natBindingInfo[key].static_key.empty()))
    {
        SWSS_LOG_INFO("Twice NAT rule is not yet added, skipping dynamic twice nat rules deletion for %s", key.c_str());
        return;
    }

    port_range = m_natPoolInfo[pool_name].port_range;

    if (!port_range.empty() and (port_range != "NULL"))
    {
        /* Check with Static NAPT entries */
        for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
        {
            /* Check the twice_nat_id is matched, otherwise continue */
            if (m_natBindingInfo[key].twice_nat_id != (*it).second.twice_nat_id)
            {
                continue;
            }

            /* Check the twice NAT is added, otherwise continue */
            if ((!(*it).second.twice_nat_added) or ((*it).second.binding_key.empty()))
            {
                continue;
            }

            /* Check interface is assigned, otherwise continue */
            if ((m_natBindingInfo[key].pool_interface == NONE_STRING) or ((*it).second.interface == NONE_STRING))
            {
                continue;
            }

            /* Check the nat type is equal, otherwise continue */
            if (m_natBindingInfo[key].nat_type != (*it).second.nat_type)
            {
                continue;
            }

            /* Check the key is matching, otherwise continue */
            if (m_natBindingInfo[key].static_key != (*it).first)
            {
                continue;
            }

            /* Delete Dynamic rules */
            setDynamicAllForwardOrAclbasedRules(DELETE, m_natBindingInfo[key].pool_interface, m_natPoolInfo[pool_name].ip_range,
                                                m_natPoolInfo[pool_name].port_range, acls_name, key);

            (*it).second.twice_nat_added = false;
            (*it).second.binding_key = EMPTY_STRING;
            m_natBindingInfo[key].twice_nat_added = false;
            m_natBindingInfo[key].static_key = EMPTY_STRING;
            isRuleDeleted = true;
            break;
        }
    }
    else
    {
        /* Get all the Static NAT entries */
        for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
        {
            /* Check the twice_nat_id is matched, otherwise continue */
            if (m_natBindingInfo[key].twice_nat_id != (*it).second.twice_nat_id)
            {
                continue;
            }

            /* Check the twice NAT is added, otherwise continue */
            if ((!(*it).second.twice_nat_added) or ((*it).second.binding_key.empty()))
            {
                continue;
            }

            /* Check interface is assigned, otherwise continue */
            if ((m_natBindingInfo[key].pool_interface == NONE_STRING) or ((*it).second.interface == NONE_STRING))
            {
                continue;
            }

            /* Check the nat type is equal, otherwise continue */
            if (m_natBindingInfo[key].nat_type != (*it).second.nat_type)
            {
                continue;
            }

            /* Check the key is matching, otherwise continue */
            if (m_natBindingInfo[key].static_key != (*it).first)
            {
                continue;
            }

            /* Delete Dynamic rules */
            setDynamicAllForwardOrAclbasedRules(DELETE, m_natBindingInfo[key].pool_interface, m_natPoolInfo[pool_name].ip_range,
                                                m_natPoolInfo[pool_name].port_range, acls_name, key);

            (*it).second.twice_nat_added = false;
            (*it).second.binding_key = EMPTY_STRING;
            m_natBindingInfo[key].twice_nat_added = false;
            m_natBindingInfo[key].static_key = EMPTY_STRING;
            isRuleDeleted = true;
            break;
        }
    }

    if (!isRuleDeleted)
    {
        SWSS_LOG_INFO("No dynamic twice nat rules to delete");
    }
}

/* To enable the NAT Feature */
void NatMgr::enableNatFeature(void)
{
    /* Create APPL_DB key */
    string appKey = VALUES;
    vector<FieldValueTuple> fvVector;

    FieldValueTuple p(NAT_ADMIN_MODE, "enabled");
    fvVector.push_back(p);

    if (m_natTcpTimeout != NAT_TCP_TIMEOUT_DEFAULT)
    {
        FieldValueTuple q(NAT_TCP_TIMEOUT, std::to_string(m_natTcpTimeout));
        fvVector.push_back(q);
    }

    if (m_natUdpTimeout != NAT_UDP_TIMEOUT_DEFAULT)
    {
        FieldValueTuple r(NAT_UDP_TIMEOUT, std::to_string(m_natUdpTimeout));
        fvVector.push_back(r);
    }

    if (m_natTimeout != NAT_TIMEOUT_DEFAULT)
    {
        FieldValueTuple s(NAT_TIMEOUT, std::to_string(m_natTimeout));
        fvVector.push_back(s);
    }

    m_appNatGlobalTableProducer.set(appKey, fvVector);
    SWSS_LOG_INFO("Enabled NAT Admin Mode to APPL_DB");

    /* Add static NAT entries */
    SWSS_LOG_INFO("Adding Static NAT entries");
    addStaticNatEntries();

    /* Add static NAPT entries */
    SWSS_LOG_INFO("Adding Static NAPT entries");
    addStaticNaptEntries();

    /* Add dynamic NAT rules */
    SWSS_LOG_INFO("Adding Dynamic NAT rules");
    addDynamicNatRules();

    /* Add full-cone PRE-ROUTING DNAT rule in the kernel */
    setFullConeDnatIptablesRule(ADD);
}

/* To disable the NAT Feature */
void NatMgr::disableNatFeature(void)
{
    /* Create APPL_DB key */
    string appKey = VALUES;
    vector<FieldValueTuple> fvVector;

    FieldValueTuple s(NAT_ADMIN_MODE, DISABLED);
    fvVector.push_back(s);

    /* Delete static NAT entries */
    SWSS_LOG_INFO("Deleting Static NAT entries");
    removeStaticNatEntries();

    /* Delete static NAPT entries */
    SWSS_LOG_INFO("Deleting Static NAPT entries");
    removeStaticNaptEntries();

    /* Delete dynamic NAT/NAPT iptables */
    removeDynamicNatRules();

    m_appNatGlobalTableProducer.set(appKey, fvVector);
    SWSS_LOG_INFO("Disabled NAT Admin Mode to APPL_DB");

    /* Delete full-cone PRE-ROUTING DNAT rule in the kernel */
    setFullConeDnatIptablesRule(DELETE);
}

/* To parse the received Static NAT Table and save it to cache */
void NatMgr::doStaticNatTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t), op = kfvOp(t);
        vector<string> keys = tokenize(key, config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string global_ip, ipAddress;
        string local_ip = EMPTY_STRING, interface = EMPTY_STRING;
        string nat_type = EMPTY_STRING, twice_nat_id = EMPTY_STRING;
        bool ipFound = false, natTypeFound = false, twiceNatFound = false, nonValueFound = false, isOverlap = false;
        int ip_num = 0, nat_type_num = 0, twice_nat_num = 0, twice_nat_value;
        uint32_t ipv4_addr, global_addr, local_addr, pool_addr_low, pool_addr_high;

        /* Example : Config_Db
         * STATIC_NAT|65.55.42.1
         *    local_ip: 10.0.0.1
         *    nat_type: dnat
         *    twice_nat_id: 100
         */

        /* Ensure the global_ip format is x.x.x.x, otherwise ignore */
        if (inet_pton(AF_INET, key.c_str(), &global_addr) != 1)
        {
            SWSS_LOG_ERROR("Invalid global address format, skipping %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Ensure the key size is 1 otherwise ignore */
        if (keys.size() != STATIC_NAT_KEY_SIZE)
        {
            SWSS_LOG_ERROR("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        global_addr = ntohl(global_addr);
        /* Ensure the global ip is not Zero, Broadcast, Loopback, Multicast and Reserved address */
        if (IS_ZERO_ADDR(global_addr) or IS_BROADCAST_ADDR(global_addr) or IS_LOOPBACK_ADDR(global_addr) or
            IS_MULTICAST_ADDR(global_addr) or IS_RESERVED_ADDR(global_addr))
        {
            SWSS_LOG_ERROR("Invalid global address, skipping %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Example : APPL_DB
         * NAT_TABLE:10.0.0.1
         *     translated_ip: 65.55.42.1
         *     nat_type: dnat
         *     entry_type: static
         */

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /* Get the Config_db key values */
            for (auto idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);
                if (field == LOCAL_IP)
                {
                    local_ip = value;
                    ipFound = true;
                    ip_num++;
                }
                else if (field == NAT_TYPE)
                {
                    nat_type = value;
                    natTypeFound = true;
                    nat_type_num++;
                }
                else if (field == TWICE_NAT_ID)
                {
                    twice_nat_id = value;
                    twiceNatFound = true;
                    twice_nat_num++;
                }
                else
                {
                    nonValueFound = true;
                }
            }

            /* Ensure the local_ip value is valid otherwise ignore */
            if ((ipFound == false) or (ip_num != 1) or nonValueFound == true)
            {
                SWSS_LOG_ERROR("Invalid local_ip values, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_type value is valid otherwise ignore */
            if ((natTypeFound == true) and (nat_type_num != 1))
            {
                SWSS_LOG_ERROR("Invalid nat_type value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the twice_nat_id value is valid otherwise ignore */
            if ((twiceNatFound == true) and (twice_nat_num != 1))
            {
                SWSS_LOG_ERROR("Invalid twice_nat_id value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the non value is not present otherwise ignore */
            if (nonValueFound == true)
            {
                SWSS_LOG_ERROR("Invalid value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the local_ip format is x.x.x.x, otherwise ignore */
            if (inet_pton(AF_INET, local_ip.c_str(), &local_addr) != 1)
            {
                SWSS_LOG_ERROR("Invalid local ip address format %s, skipping %s", local_ip.c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            local_addr = ntohl(local_addr);
            /* Ensure the local ip is not Zero, Broadcast, Loopback, Multicast and Reserved address */
            if (IS_ZERO_ADDR(local_addr) or IS_BROADCAST_ADDR(local_addr) or IS_LOOPBACK_ADDR(local_addr) or
                IS_MULTICAST_ADDR(local_addr) or IS_RESERVED_ADDR(local_addr))
            {
                SWSS_LOG_ERROR("Invalid local address %s, skipping %s", local_ip.c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_type value is snat or dnat otherwise ignore */
            if ((natTypeFound == true) and ((nat_type != SNAT_NAT_TYPE) and (nat_type != DNAT_NAT_TYPE)))
            {
                SWSS_LOG_ERROR("Invalid nat_type, it is neither snat or dnat, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (twiceNatFound == true)
            {
                /* Ensure the given twice_nat_id is integer, otherwise ignore */
                try
                {
                    twice_nat_value = stoi(twice_nat_id);
                }
                catch(...)
                {
                    SWSS_LOG_ERROR("Invalid twice_nat_id %s, skipping %s", twice_nat_id.c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                /* Ensure the twice_nat_id is within the limits (1 to 9999), otherwise ignore */
                if ((twice_nat_value < TWICE_NAT_ID_MIN) or (twice_nat_value > TWICE_NAT_ID_MAX))
                {
                    SWSS_LOG_ERROR("Invalid twice_nat_id, not in limits, skipping %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }

            /* check the global address is overlapping with any NAPT entry */
            global_ip = key;
            if (nat_type == SNAT_NAT_TYPE)
            {
                global_ip = local_ip;
            }

            for (auto it = m_staticNaptEntry.begin(); it != m_staticNaptEntry.end(); it++)
            {
                vector<string> napt_keys = tokenize((*it).first, config_db_key_delimiter);
                ipAddress = napt_keys[0];
                if ((*it).second.nat_type == SNAT_NAT_TYPE)
                {
                    ipAddress = (*it).second.local_ip;
                }

                if (ipAddress == global_ip)
                {
                    isOverlap = true;
                    break;
                }
            }

            if (isOverlap)
            {
                SWSS_LOG_ERROR("Global Ip overlaps with static NAPT entry, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* check the global address is overlapping with any Dynamic Pool entry */
            ipv4_addr = global_addr;
            if (nat_type == SNAT_NAT_TYPE)
            {
                ipv4_addr = local_addr;
            }

            for (auto it = m_natPoolInfo.begin(); it != m_natPoolInfo.end(); it++)
            {            
                vector<string> nat_ip = tokenize((*it).second.ip_range, range_specifier);

                /* Check the pool is valid */
                if ((nat_ip.empty()) or (nat_ip.size() > 2))
                {
                    continue;
                }
                else if (nat_ip.size() == 2)
                {
                    inet_pton(AF_INET, nat_ip[1].c_str(), &pool_addr_high);
                    pool_addr_high = ntohl(pool_addr_high);

                    inet_pton(AF_INET, nat_ip[0].c_str(), &pool_addr_low);
                    pool_addr_low = ntohl(pool_addr_low);
                }
                else if (nat_ip.size() == 1)
                {
                    inet_pton(AF_INET, nat_ip[0].c_str(), &pool_addr_low);
                    pool_addr_high = ntohl(pool_addr_low);
                    pool_addr_low = ntohl(pool_addr_low);
                }

                if ((ipv4_addr >= pool_addr_low) and (ipv4_addr <= pool_addr_high))
                {
                    isOverlap = true;
                    break;
                }
            }

            if (isOverlap)
            {
                SWSS_LOG_ERROR("Global Ip overlaps with Dynamic Pool IP entry, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Check the key is already present in cache */
            if (m_staticNatEntry.find(key) != m_staticNatEntry.end())
            {
                SWSS_LOG_INFO("Static NAT %s exists", key.c_str());

                if (m_staticNatEntry[key].local_ip == local_ip)
                {
                    /* Received the same Key and value, ignore */
                    SWSS_LOG_ERROR("Duplicate Static NAT and it's values, skipping %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue; 
                }
                else
                {
                    SWSS_LOG_INFO("Static NAT %s with updated info", key.c_str());

                    /* Received key with new value */
                    if (m_staticNatEntry[key].interface != NONE_STRING)
                    {
                        /* Remove the Static NAT Entry */
                        SWSS_LOG_INFO("Deleting the Static NAT entry for %s", key.c_str());
                        removeStaticNatEntry(key);
                    }
                }
            }
             
            /* New Key, Add it to cache */
            m_staticNatEntry[key].interface = NONE_STRING;
            m_staticNatEntry[key].local_ip = local_ip;
            if (nat_type.empty())
            {
                m_staticNatEntry[key].nat_type = DNAT_NAT_TYPE;
            }
            else
            {
                m_staticNatEntry[key].nat_type = nat_type;
            }
            m_staticNatEntry[key].twice_nat_id = twice_nat_id;
            m_staticNatEntry[key].twice_nat_added = false;
            m_staticNatEntry[key].binding_key = EMPTY_STRING;
            SWSS_LOG_INFO("Static NAT %s is added to cache", key.c_str());

            /* Add the new Static NAT entry */
            SWSS_LOG_INFO("Adding the Static NAT entry for %s", key.c_str());
            addStaticNatEntry(key);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Del command for %s", key.c_str());

            /* Check the key is already present in cache */
            if (m_staticNatEntry.find(key) != m_staticNatEntry.end())
            {
                /* Remove the Static NAT Entry */
                SWSS_LOG_INFO("Deleting the Static NAT entry for %s", key.c_str());
                removeStaticNatEntry(key);

                /* Cleaned the cache */
                SWSS_LOG_INFO("Static NAT %s is removed from the cache", key.c_str());
                m_staticNatEntry.erase(key);
            }
            else
            {
                SWSS_LOG_ERROR("Invalid Static NAT %s from Config_db, do nothing", key.c_str());
            }
            
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* To parse the received Static NAPT Table and save it to cache */
void NatMgr::doStaticNaptTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t), op = kfvOp(t);
        vector<string> keys = tokenize(key, config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string global_ip, ipAddress;
        string local_ip = EMPTY_STRING, local_port = EMPTY_STRING, interface = EMPTY_STRING;
        string nat_type = EMPTY_STRING, twice_nat_id = EMPTY_STRING;
        bool ipFound = false, portFound = false, natTypeFound = false, twiceNatFound = false, nonValueFound = false, isOverlap = false;
        int ip_num = 0, port_num = 0, portValue = 0, nat_type_num = 0, twice_nat_num = 0, twice_nat_value;
        uint32_t ipv4_addr;
  
        /* Example : Config_Db
         * STATIC_NAPT|65.55.42.1|TCP|1024
         *    local_ip: 10.0.0.1
         *    local_port: 6000
         */

        /* Ensure the key size is 3 otherwise ignore */
        if (keys.size() != STATIC_NAPT_KEY_SIZE)
        {   
            SWSS_LOG_ERROR("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Ensure the global ip format is x.x.x.x, otherwise ignore */
        if (inet_pton(AF_INET, keys[0].c_str(), &ipv4_addr) != 1)
        {
            SWSS_LOG_ERROR("Invalid global ip address format %s, skipping %s", keys[0].c_str(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        ipv4_addr = ntohl(ipv4_addr);
        /* Ensure the local ip is not Zero, Broadcast, Loopback, Multicast and Reserved address */
        if (IS_ZERO_ADDR(ipv4_addr) or IS_BROADCAST_ADDR(ipv4_addr) or IS_LOOPBACK_ADDR(ipv4_addr) or
            IS_MULTICAST_ADDR(ipv4_addr) or IS_RESERVED_ADDR(ipv4_addr))
        {
            SWSS_LOG_ERROR("Invalid global address %s, skipping %s", keys[0].c_str(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Ensure the prototype is UDP or TCP otherwise ignore */
        if ((keys[1] != to_upper(IP_PROTOCOL_TCP)) and (keys[1] != to_upper(IP_PROTOCOL_UDP)))
        {
            SWSS_LOG_ERROR("Invalid ip prototype %s, skipping %s", keys[1].c_str(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Ensure the given portValue is integer, otherwise ignore */
        try
        {
            portValue = stoi(keys[2]);
        }
        catch(...)
        {
            SWSS_LOG_ERROR("Invalid global port %s, skipping %s", keys[2].c_str(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Ensure the global port is inbetween 1 to 65535, otherwise ignore */
        if ((portValue < L4_PORT_MIN) or (portValue > L4_PORT_MAX))
        {
            SWSS_LOG_ERROR("Invalid global port value %d, skipping %s", portValue, key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Example : APPL_DB
         * NAPT_TABLE:TCP:10.0.0.1:6000
         *     translated_ip: 65.55.42.1
         *     translated_port: 1024
         *     nat_type: dnat
         *     entry_type: static
         */

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /* Get the Config_db key values */
            for (auto idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);
                if (field == LOCAL_IP)
                {
                    local_ip = value;
                    ipFound = true;
                    ip_num++;
                }
                else if (field == LOCAL_PORT)
                {
                    local_port = value;
                    portFound = true;
                    port_num++;
                }
                else if (field == NAT_TYPE)
                {
                    nat_type = value;
                    natTypeFound = true;
                    nat_type_num++;
                }
                else if (field == TWICE_NAT_ID)
                {
                    twice_nat_id = value;
                    twiceNatFound = true;
                    twice_nat_num++;
                }
                else
                {
                    nonValueFound = true;
                }
            }

            /* Ensure the local_ip value is valid otherwise ignore */
            if ((ipFound == true) and (ip_num != 1))
            {
                SWSS_LOG_ERROR("Invalid local_ip value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the local_port value is valid otherwise ignore */
            if ((portFound == true) and (port_num != 1))
            {
                SWSS_LOG_ERROR("Invalid local_port value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_type value is valid otherwise ignore */
            if ((natTypeFound == true) and (nat_type_num != 1))
            {
                SWSS_LOG_ERROR("Invalid nat_type value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the twice_nat_id value is valid otherwise ignore */
            if ((twiceNatFound == true) and (twice_nat_num != 1))
            {
                SWSS_LOG_ERROR("Invalid twice_nat_id value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the non value is not present otherwise ignore */
            if (nonValueFound == true)
            {
                SWSS_LOG_ERROR("Invalid value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the local ip format is x.x.x.x, otherwise ignore */
            if (inet_pton(AF_INET, local_ip.c_str(), &ipv4_addr) != 1)
            {
                SWSS_LOG_ERROR("Invalid local ip address format %s, skipping %s", local_ip.c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            ipv4_addr = ntohl(ipv4_addr);
            /* Ensure the local ip is not Zero, Broadcast, Loopback, Multicast and Reserved address */
            if (IS_ZERO_ADDR(ipv4_addr) or IS_BROADCAST_ADDR(ipv4_addr) or IS_LOOPBACK_ADDR(ipv4_addr) or
                IS_MULTICAST_ADDR(ipv4_addr) or IS_RESERVED_ADDR(ipv4_addr))
            {
                SWSS_LOG_ERROR("Invalid local address %s, skipping %s", local_ip.c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the given local_port is integer, otherwise ignore */
            try
            {
                portValue = stoi(local_port);
            }
            catch(...)
            {
                SWSS_LOG_ERROR("Invalid local port %s, skipping %s", local_port.c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the local port is inbetween 1 to 65535, otherwise ignore */
            if ((portValue < L4_PORT_MIN) or (portValue > L4_PORT_MAX))
            {
                SWSS_LOG_ERROR("Invalid internal port value %d, skipping %s", portValue, key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_type value is snat or dnat otherwise ignore */
            if ((natTypeFound == true) and ((nat_type != SNAT_NAT_TYPE) and (nat_type != DNAT_NAT_TYPE)))
            {
                SWSS_LOG_ERROR("Invalid nat_type, it is neither snat or dnat, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (twiceNatFound == true)
            {
                /* Ensure the given twice_nat_id is integer, otherwise ignore */
                try
                {
                    twice_nat_value = stoi(twice_nat_id);
                }
                catch(...)
                {
                    SWSS_LOG_ERROR("Invalid twice_nat_id %s, skipping %s", twice_nat_id.c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                /* Ensure the twice_nat_id is within the limits (1 to 9999), otherwise ignore */
                if ((twice_nat_value < TWICE_NAT_ID_MIN) or (twice_nat_value > TWICE_NAT_ID_MAX))
                {
                    SWSS_LOG_ERROR("Invalid twice_nat_id %d, not in limits, skipping %s", twice_nat_value, key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }

            /* check the global address is overlapping with any NAT entry */
            global_ip = keys[0];
            if (nat_type == SNAT_NAT_TYPE)
            {
                global_ip = local_ip;
            }

            for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
            {
                ipAddress = (*it).first;
                if ((*it).second.nat_type == SNAT_NAT_TYPE)
                {
                    ipAddress = (*it).second.local_ip;
                }

                if (ipAddress == global_ip)
                {
                    isOverlap = true;
                    break;
                }
            }

            if (isOverlap)
            {
                SWSS_LOG_ERROR("Global Ip overlaps with static NAT entry, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Check the key is already present in cache */
            if (m_staticNaptEntry.find(key) != m_staticNaptEntry.end())
            {
                SWSS_LOG_INFO("Static NAPT %s exists", key.c_str());
                if ((m_staticNaptEntry[key].local_ip == local_ip) and 
                    (m_staticNaptEntry[key].local_port == local_port))
                {
                    /* Received the same Key and value, ignore */
                    SWSS_LOG_ERROR("Duplicate Static NAPT and it's values, skipping %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                else
                {
                    SWSS_LOG_INFO("Static NAPT %s with updated info", key.c_str());

                    /* Received key with new value */
                    if (m_staticNatEntry[key].interface != NONE_STRING)
                    {
                        /* Remove the Static NAPT Entry */
                        SWSS_LOG_INFO("Deleting the Static NAPT entry for %s", key.c_str());
                        removeStaticNaptEntry(key);
                    }
                }
            }

            /* New Key, Add it to cache */
            m_staticNaptEntry[key].interface = NONE_STRING;
            m_staticNaptEntry[key].local_ip = local_ip;
            m_staticNaptEntry[key].local_port = local_port;
            if (nat_type.empty())
            {
                m_staticNaptEntry[key].nat_type = DNAT_NAT_TYPE;
            }
            else
            {
                m_staticNaptEntry[key].nat_type = nat_type;
            }
            m_staticNaptEntry[key].twice_nat_id = twice_nat_id;
            m_staticNaptEntry[key].twice_nat_added = false;
            m_staticNaptEntry[key].binding_key = EMPTY_STRING;
            SWSS_LOG_INFO("Static NAPT %s is added to cache", key.c_str());

            /* Add the new Static NAT entry */
            SWSS_LOG_INFO("Adding the Static NAPT entry for %s", key.c_str());
            addStaticNaptEntry(key);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Del command for %s", key.c_str());

            /* Check the key is already present in cache */
            if (m_staticNaptEntry.find(key) != m_staticNaptEntry.end())
            {
                /* Remove the Static NAPT Entry */
                SWSS_LOG_INFO("Deleting the Static NAPT entry for %s", key.c_str());
                removeStaticNaptEntry(key);

                /* Cleaned the cache */
                SWSS_LOG_INFO("Static NAPT %s is removed from the cache", key.c_str());
                m_staticNaptEntry.erase(key);
            }
            else
            {
                SWSS_LOG_ERROR("Invalid Static NAPT %s from Config_db, do nothing", key.c_str());
            }

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* To parse the received NAT Pool Table and save it to cache */
void NatMgr::doNatPoolTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t), op = kfvOp(t);
        vector<string> keys = tokenize(key, config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string nat_ip = EMPTY_STRING, nat_port = EMPTY_STRING, binding_name = EMPTY_STRING, static_ip;
        bool ipFound = false, portFound = false, nonValueFound = false, isOverlap = false;
        int ip_num = 0, port_num = 0, portValue_low, portValue_high;
        uint32_t ipv4_addr_low, ipv4_addr_high, static_address;

        /* Example : Config_Db
         * NAT_POOL|PoolName
         *    nat_ip: 10.0.0.1-10.0.0.5
         *    nat_port: 100-105
         */

        /* Ensure the key size is 1 otherwise ignore */
        if (keys.size() != POOL_TABLE_KEY_SIZE)
        {
            SWSS_LOG_ERROR("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /* Get the Config_db key values */
            for (auto idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);
                if (field == NAT_IP)
                {
                    nat_ip = value;
                    ipFound = true;
                    ip_num++;
                }
                else if (field == NAT_PORT)
                {
                    nat_port = value;
                    portFound = true;
                    port_num++;
                }
                else
                {
                    nonValueFound = true;
                }
            }

            /* Ensure the "nat_ip" values are valid otherwise ignore */
            if (((ipFound == true) and (ip_num != 1)) or (ipFound == false))
            {
                SWSS_LOG_ERROR("Invalid nat_ip values, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the "nat_port" values are valid otherwise ignore */
            if ((portFound == true) and (port_num != 1))
            {
                SWSS_LOG_ERROR("Invalid key values, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the non value is not present, otherwise ignore */
            if (nonValueFound == true)
            {
                SWSS_LOG_ERROR("Invalid value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the Pool name length is not more than 32 otherwise ignore */
            if (key.length() > 32)
            {
                SWSS_LOG_ERROR("Invalid pool name length - %lu, skipping %s", key.length(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_ip is not empty */
            if (nat_ip.empty() or (nat_ip == "NULL"))
            {
                SWSS_LOG_ERROR("Invalid nat_ip, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            vector<string> nat_ip_range = tokenize(nat_ip, range_specifier);

            /* Ensure the nat_ip_range is valid */
            if (nat_ip_range.empty())
            {
                SWSS_LOG_ERROR("NAT pool ip range %s is not valid, skipping %s", nat_ip.c_str(), key.c_str());
                continue;
            }

            /* Ensure the ip range size is not more than 2, otherwise ignore */
            if (nat_ip_range.size() > 2)
            {
                SWSS_LOG_ERROR("Invalid nat ip range size %lu, skipping %s", nat_ip_range.size(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else if (nat_ip_range.size() == 2)
            {
                SWSS_LOG_INFO("Pool %s contains nat_ip range", key.c_str());

                /* Ensure the ip format is x.x.x.x, otherwise ignore */
                if (inet_pton(AF_INET, nat_ip_range[1].c_str(), &ipv4_addr_high) != 1)
                {
                    SWSS_LOG_ERROR("Invalid ip address format %s, skipping %s", nat_ip_range[1].c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                ipv4_addr_high = ntohl(ipv4_addr_high);
                /* Ensure the ip address is not Zero, Broadcast, Loopback, Multicast and Reserved address */
                if (IS_ZERO_ADDR(ipv4_addr_high) or IS_BROADCAST_ADDR(ipv4_addr_high) or IS_LOOPBACK_ADDR(ipv4_addr_high) or
                    IS_MULTICAST_ADDR(ipv4_addr_high) or IS_RESERVED_ADDR(ipv4_addr_high))
                {
                    SWSS_LOG_ERROR("Invalid ip address %s, skipping %s", nat_ip_range[1].c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                /* Ensure the ip format is x.x.x.x, otherwise ignore */
                if (inet_pton(AF_INET, nat_ip_range[0].c_str(), &ipv4_addr_low) != 1)
                {
                    SWSS_LOG_ERROR("Invalid ip address format %s, skipping %s", nat_ip_range[0].c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                ipv4_addr_low = ntohl(ipv4_addr_low);
                /* Ensure the ip address is not Zero, Broadcast, Loopback, Multicast and Reserved address */
                if (IS_ZERO_ADDR(ipv4_addr_low) or IS_BROADCAST_ADDR(ipv4_addr_low) or IS_LOOPBACK_ADDR(ipv4_addr_low) or
                    IS_MULTICAST_ADDR(ipv4_addr_low) or IS_RESERVED_ADDR(ipv4_addr_low))
                {
                    SWSS_LOG_ERROR("Invalid ip address %s, skipping %s", nat_ip_range[0].c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                /* Ensure the ip range is proper */
                if (ipv4_addr_low >= ipv4_addr_high)
                {
                    SWSS_LOG_ERROR("NAT pool ip range %s is not valid, skipping %s", nat_ip.c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }
            else
            {
                /* Ensure the ip format is x.x.x.x, otherwise ignore */
                if (inet_pton(AF_INET, nat_ip_range[0].c_str(), &ipv4_addr_low) != 1)
                {
                    SWSS_LOG_ERROR("Invalid ip address format %s, skipping %s", nat_ip_range[0].c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                ipv4_addr_high = ntohl(ipv4_addr_low);
                ipv4_addr_low = ntohl(ipv4_addr_low);

                /* Ensure the ip address is not Zero, Broadcast, Loopback, Multicast and Reserved address */
                if (IS_ZERO_ADDR(ipv4_addr_low) or IS_BROADCAST_ADDR(ipv4_addr_low) or IS_LOOPBACK_ADDR(ipv4_addr_low) or
                    IS_MULTICAST_ADDR(ipv4_addr_low) or IS_RESERVED_ADDR(ipv4_addr_low))
                {
                    SWSS_LOG_ERROR("Invalid ip address %s, skipping %s", nat_ip_range[0].c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }

            /* Check the Pool table contains nat_port */
            if (!nat_port.empty() and (nat_port != "NULL"))
            {
                SWSS_LOG_INFO("Pool %s contains nat_port info", key.c_str());   

                vector<string> nat_port_range = tokenize(nat_port, range_specifier);

                /* Ensure the port range size is not more than 2, otherwise ignore */
                if (nat_port_range.size() > 2)
                {
                    SWSS_LOG_ERROR("Invalid nat port range size %lu, skipping %s", nat_port_range.size(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                else if (nat_port_range.size() == 2)
                {
                    /* Ensure the given port is integer, otherwise ignore */
                    try 
                    {
                        portValue_low = stoi(nat_port_range[0]);
                    }
                    catch(...)
                    {
                        SWSS_LOG_ERROR("Invalid port %s, skipping %s", nat_port.c_str(), key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    /* Ensure the port is inbetween 1 to 65535, otherwise ignore */
                    if ((portValue_low < L4_PORT_MIN) or (portValue_low > L4_PORT_MAX))
                    {
                        SWSS_LOG_ERROR("Invalid port value %d, skipping %s", portValue_low, key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    /* Ensure the given port is integer, otherwise ignore */
                    try
                    {
                        portValue_high = stoi(nat_port_range[1]);
                    }
                    catch(...)
                    {
                        SWSS_LOG_ERROR("Invalid port %s, skipping %s", nat_port.c_str(), key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    /* Ensure the port is inbetween 1 to 65535, otherwise ignore */
                    if ((portValue_high < L4_PORT_MIN) or (portValue_high > L4_PORT_MAX))
                    {
                        SWSS_LOG_ERROR("Invalid port value %d, skipping %s", portValue_high, key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    if (portValue_low >= portValue_high)
                    {
                        SWSS_LOG_ERROR("Invalid nat port range %s, skipping %s", nat_port.c_str(), key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                }
                else
                {
                    /* Ensure the given port is integer, otherwise ignore */
                    try
                    {
                        portValue_low = stoi(nat_port_range[0]);
                    }
                    catch(...)
                    {
                        SWSS_LOG_ERROR("Invalid port %s, skipping %s", nat_port.c_str(), key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    /* Ensure the port is inbetween 1 to 65535, otherwise ignore */
                    if ((portValue_low < L4_PORT_MIN) or (portValue_low > L4_PORT_MAX))
                    {
                        SWSS_LOG_ERROR("Invalid port value %d, skipping %s", portValue_low, key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                }
            }

            /* check the pool ip address is overlapping with any NAT entry */
            for (auto it = m_staticNatEntry.begin(); it != m_staticNatEntry.end(); it++)
            {
                static_ip = (*it).first;
                if ((*it).second.nat_type == SNAT_NAT_TYPE)
                {
                    static_ip = (*it).second.local_ip;
                }

                inet_pton(AF_INET, static_ip.c_str(), &static_address);
                static_address = ntohl(static_address);

                if ((static_address >= ipv4_addr_low) and (static_address <= ipv4_addr_high))
                {
                    isOverlap = true;
                    break;
                }
            }

            if (isOverlap)
            {
                SWSS_LOG_ERROR("Pool Ip address is overlaps with static NAT entry, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Check the key is already present in cache */
            if (m_natPoolInfo.find(key) != m_natPoolInfo.end())
            {
                SWSS_LOG_INFO("Pool %s exists", key.c_str());

                if ((m_natPoolInfo[key].ip_range == nat_ip) and (m_natPoolInfo[key].port_range == nat_port))
                {
                    /* Received the same Key and value, ignore */
                    SWSS_LOG_ERROR("Duplicate Pool and it's values, skipping %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                else
                {
                    SWSS_LOG_INFO("Pool %s with updated info", key.c_str());

                    /* Check this pool name on any nat binding */
                    if (isPoolMappedtoBinding(key, binding_name))
                    {
                        /* Remove the existing Dynamic NAT rules */
                        SWSS_LOG_INFO("Deleting the Dynamic NAT/NAPT iptables rules for %s", key.c_str()); 
                        removeDynamicNatRule(binding_name);
                    }
                }
            }

            /* New Key, Add it to cache */
            m_natPoolInfo[key].ip_range = nat_ip;
            if (!nat_port.empty() and (nat_port != "NULL")) 
            {
                m_natPoolInfo[key].port_range = nat_port;
            }
            else
            {
                m_natPoolInfo[key].port_range = EMPTY_STRING;
            }

            /* Check this pool name on any nat binding */
            if (isPoolMappedtoBinding(key, binding_name))
            {
                SWSS_LOG_INFO("Pool %s info is added to the cache", key.c_str());

                /* Add the new Dynamic Nat Rules */
                SWSS_LOG_INFO("Adding the Dynamic NAT rules for %s", key.c_str());
                addDynamicNatRule(binding_name);
            }
            else
            {
                SWSS_LOG_INFO("Pool %s is not yet binded, saved it to cache", key.c_str());
            }

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Del command for %s", key.c_str());

            string binding_name = EMPTY_STRING;
            
            /* Check the key is already present in cache */
            if (m_natPoolInfo.find(key) != m_natPoolInfo.end())
            {
                /* Check this pool name on any nat binding */
                if (isPoolMappedtoBinding(key, binding_name))
                {
                    SWSS_LOG_INFO("Pool %s is mapped on binding %s, deleting the dynamic iptables rules", key.c_str(), binding_name.c_str());
                    /* Remove the existing Dynamic NAT rules */
                    removeDynamicNatRule(binding_name);
                }

                /* Clean the pool Info */
                m_natPoolInfo.erase(key);
                SWSS_LOG_INFO("Pool %s is cleaned from the cache", key.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Invalid NAT Pool %s from Config_db, do nothing", key.c_str());
            }

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* To parse the received NAT Binding Table and save it to cache */
void NatMgr::doNatBindingTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t), op = kfvOp(t);
        vector<string> keys = tokenize(key, config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string nat_pool = EMPTY_STRING, nat_acl = EMPTY_STRING;
        string nat_type = NONE_STRING, twice_nat_id = EMPTY_STRING;
        bool poolFound = false, aclFound = false, natTypeFound = false, twiceNatFound = false, nonValueFound = false;
        int pool_num = 0, acl_num = 0, nat_type_num = 0, twice_nat_num = 0, twice_nat_value;

        /* Example :
         * NAT_BINDINGS|bindingName
         *    nat_pool: poolName
         *    access_list: aclName,aclName2
         */

        /* Ensure the key size is 1 otherwise ignore */
        if (keys.size() != BINDING_TABLE_KEY_SIZE)
        {
            SWSS_LOG_ERROR("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Ensure the Binding name length is not more than 32 otherwise ignore */
        if (key.length() > 32)
        {
            SWSS_LOG_ERROR("Invalid binding name length - %lu, skipping %s", key.length(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /* Get the Config_db key values */
            for (auto idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);
                if (field == NAT_POOL)
                {
                    nat_pool = value;
                    poolFound = true;
                    pool_num++;
                }
                else if (field == NAT_ACLS)
                {
                    nat_acl = value;
                    aclFound = true;
                    acl_num++;
                }
                else if (field == NAT_TYPE)
                {
                    nat_type = value;
                    natTypeFound = true;
                    nat_type_num++;
                }
                else if (field == TWICE_NAT_ID)
                {
                    twice_nat_id = value;
                    twiceNatFound = true;
                    twice_nat_num++;
                }
                else
                {
                    nonValueFound = true;
                }
            }

            /* Ensure the nat_pool value is valid, otherwise ignore */
            if ((poolFound == true) and (pool_num != 1))
            {
                SWSS_LOG_ERROR("Invalid nat_pool values, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the access_list value is valid, otherwise ignore */
            if ((aclFound == true) and (acl_num != 1))
            {
                SWSS_LOG_ERROR("Invalid access_list values, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_type value is valid otherwise ignore */
            if ((natTypeFound == true) and (nat_type_num != 1))
            {
                SWSS_LOG_ERROR("Invalid nat_type value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the twice_nat_id value is valid otherwise ignore */
            if ((twiceNatFound == true) and (twice_nat_num != 1))
            {
                SWSS_LOG_ERROR("Invalid twice_nat_id value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the non value is not present, otherwise ignore */
            if (nonValueFound == true)
            {
                SWSS_LOG_ERROR("Invalid value, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_type value is snat otherwise ignore */
            if ((natTypeFound == true) and (nat_type != SNAT_NAT_TYPE))
            {
                SWSS_LOG_ERROR("Invalid nat_type %s, skipping %s", nat_type.c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (twice_nat_id == "NULL")
            {
                twiceNatFound = false;
                twice_nat_id = EMPTY_STRING;
            }

            if (twiceNatFound == true)
            {
                /* Ensure the given twice_nat_id is integer, otherwise ignore */
                try
                {
                    twice_nat_value = stoi(twice_nat_id);
                }
                catch(...)
                {
                    SWSS_LOG_ERROR("Invalid twice_nat_id %s, skipping %s", twice_nat_id.c_str(), key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                /* Ensure the twice_nat_id is within the limits (1 to 9999), otherwise ignore */
                if ((twice_nat_value < TWICE_NAT_ID_MIN) or (twice_nat_value > TWICE_NAT_ID_MAX))
                {
                    SWSS_LOG_ERROR("Invalid twice_nat_id %d, not in limits, skipping %s", twice_nat_value, key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }

            /* Ensure the Pool name length is not more than 32 otherwise ignore */
            if (nat_pool.length() > 32)
            {
                SWSS_LOG_ERROR("Invalid pool name length - %lu, skipping %s", nat_pool.length(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Check the key is already present in cache */
            if (m_natBindingInfo.find(key) != m_natBindingInfo.end())
            {
                SWSS_LOG_INFO("Binding %s exists", key.c_str());
                if ((m_natBindingInfo[key].pool_name == nat_pool) and (m_natBindingInfo[key].acl_name == nat_acl))
                {
                    /* Received the same Key and value, ignore */
                    SWSS_LOG_ERROR("Duplicate Binding and it's values, skipping %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                else
                {
                    /* Remove the existing Dynamic NAT rules */
                    SWSS_LOG_INFO("Deleting the Dynamic NAT rules for %s", key.c_str());
                    removeDynamicNatRule(key);
                }
            }

            /* New Key, Add it to cache */
            m_natBindingInfo[key].pool_name = nat_pool;
            m_natBindingInfo[key].acl_name = nat_acl;
            m_natBindingInfo[key].pool_interface = NONE_STRING;
            m_natBindingInfo[key].acl_interface = NONE_STRING;
            m_natBindingInfo[key].twice_nat_id = twice_nat_id;
            m_natBindingInfo[key].twice_nat_added = false;
            if (nat_type.empty())
            {
                m_natBindingInfo[key].nat_type = SNAT_NAT_TYPE;
            }
            else
            {
                m_natBindingInfo[key].nat_type = nat_type;
            }
            m_natBindingInfo[key].static_key = EMPTY_STRING;
            SWSS_LOG_INFO("Binding Info %s is added to the cache", key.c_str());
 
            /* Add the new Dynamic Nat Rules */
            SWSS_LOG_INFO("Adding the Dynamic NAT rules for %s", key.c_str());
            addDynamicNatRule(key);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Del command for %s", key.c_str());

            /* Check the key is already present in cache */
            if (m_natBindingInfo.find(key) != m_natBindingInfo.end())
            {
                /* Remove the existing Dynamic NAT rules */
                SWSS_LOG_INFO("Deleting the Dynamic NAT rules for %s", key.c_str());
                removeDynamicNatRule(key);

               /* Clean the binding info */
               m_natBindingInfo.erase(key);
               SWSS_LOG_INFO("Binding Info %s is cleaned from the cache", key.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Invalid NAT Binding %s from Config_Db, do nothing", key.c_str());
            }

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* To parse the received NAT Global Table and save it to cache */
void NatMgr::doNatGlobalTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        string key = kfvKey(t), op = kfvOp(t), adminMode;
        bool tcpFound = false, udpFound = false, timeoutFound = false;
        bool nonValueFound = false, adminModeFound = false;
        int tcp_timeout = 0, udp_timeout = 0, timeout = 0;
        int tcp_num = 0, udp_num = 0, timeout_num = 0, admin_mode_num = 0;

        /* Example : Config_DB
         * NAT_GLOBAL|Values
         *    admin_mode: disabled
         *    nat_timeout: 600
         *    nat_tcp_timeout: 300
         *    nat_udp_timeout: 50
         */

        /* Ensure the key is "Values" otherwise ignore */
        if (strcmp(key.c_str(), VALUES))
        {
            SWSS_LOG_ERROR("Invalid key %s format. No Values", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Example : APPL_DB
         * NAT_GLOBAL_TABLE:Values
         *    admin_mode: disabled
         *    nat_timeout : 600
         *    nat_tcp_timeout : 300 
         *    nat_udp_timeout : 50
         */

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /* Create APPL_DB key */
            string appKey = VALUES;
            vector<FieldValueTuple> fvVector;

            /* Get the Config_db values */
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == NAT_ADMIN_MODE)
                {
                    adminMode = fvValue(i);
                    adminModeFound = true;
                    admin_mode_num++;
                }
                else if (fvField(i) == NAT_TCP_TIMEOUT)
                {
                    /* Ensure the given tcp_timeout is integer, otherwise ignore */
                    try
                    {
                        tcp_timeout = stoi(fvValue(i));
                    }
                    catch(...)
                    {
                        SWSS_LOG_ERROR("Invalid tcp_timeout %s, skipping %s", fvValue(i).c_str(), key.c_str());
                        continue;
                    }
                    tcpFound = true;
                    tcp_num++;
                }
                else if (fvField(i) == NAT_UDP_TIMEOUT)
                {
                    /* Ensure the given udp_timeout is integer, otherwise ignore */
                    try
                    {
                        udp_timeout = stoi(fvValue(i));
                    }
                    catch(...)
                    {
                        SWSS_LOG_ERROR("Invalid udp_timeout %s, skipping %s", fvValue(i).c_str(), key.c_str());
                        continue;
                    }
                    udpFound = true;
                    udp_num++;
                }
                else if (fvField(i) == NAT_TIMEOUT)
                {
                    /* Ensure the given timeout is integer, otherwise ignore */
                    try
                    {
                        timeout = stoi(fvValue(i));
                    }
                    catch(...)
                    {
                        SWSS_LOG_ERROR("Invalid timeout %s, skipping %s", fvValue(i).c_str(), key.c_str());
                        continue;
                    }
                    timeoutFound = true;
                    timeout_num++;
                }
                else
                {
                    nonValueFound = true;
                }
            }

            /* Ensure the admin_mode value is valid otherwise ignore */
            if ((adminModeFound == true) and (admin_mode_num != 1))
            {
                SWSS_LOG_ERROR("Invalid admin_mode, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_tcp_timeout values is valid otherwise ignore */
            if ((tcpFound == true) and (tcp_num != 1))
            {
                SWSS_LOG_ERROR("Invalid nat_tcp_timeout, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_udp_timeout value is valid otherwise ignore */
            if ((udpFound == true) and (udp_num != 1))
            {
                SWSS_LOG_ERROR("Invalid nat_udp_timeout, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the nat_timeout value is valid otherwise ignore */
            if ((timeoutFound == true) and (timeout_num != 1))
            {
                SWSS_LOG_ERROR("Invalid nat_timeout, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the value is valid otherwise ignore */
            if (((tcpFound == false) and (udpFound == false) and (timeoutFound == false) and (adminModeFound == false)) or 
                (nonValueFound == true))
            {
                SWSS_LOG_ERROR("Invalid, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the admin_mode is enabled/disabled, otherwise ignore */
            if ((adminModeFound == true) and ((adminMode != ENABLED) and (adminMode != DISABLED)))
            {
                SWSS_LOG_ERROR("Invalid admin_mode value %s, skipping %s", adminMode.c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            
            /* Ensure the tcp timeout is inbetween 300 to 432000, otherwise ignore */
            if ((tcpFound == true) and ((tcp_timeout < NAT_TCP_TIMEOUT_MIN) or (tcp_timeout > NAT_TCP_TIMEOUT_MAX)))
            {
                SWSS_LOG_ERROR("Invalid tcp timeout value %d, skipping %s", tcp_timeout, key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the udp timeout is inbetween 120 to 600, otherwise ignore */
            if ((udpFound == true) and ((udp_timeout < NAT_UDP_TIMEOUT_MIN) or (udp_timeout > NAT_UDP_TIMEOUT_MAX)))
            {
                SWSS_LOG_ERROR("Invalid udp timeout value %d, skipping %s", udp_timeout, key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the timeout is inbetween 300 to 432000, otherwise ignore */
            if ((timeoutFound == true) and ((timeout < NAT_TIMEOUT_MIN) or (timeout > NAT_TIMEOUT_MAX)))
            {
                SWSS_LOG_ERROR("Invalid timeout value %d, skipping %s", timeout, key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if ((tcpFound == true) and (tcp_timeout != m_natTcpTimeout))
            {
                m_natTcpTimeout = tcp_timeout;
                SWSS_LOG_INFO("NAT TCP Timeout %s is added to cache", key.c_str());
                if (isNatEnabled())
                {
                    FieldValueTuple s(NAT_TCP_TIMEOUT, std::to_string(tcp_timeout));
                    fvVector.push_back(s);
                }
            }

            if ((udpFound == true) and (udp_timeout != m_natUdpTimeout))
            {
                m_natUdpTimeout = udp_timeout;
                SWSS_LOG_INFO("NAT UDP Timeout %s is added to cache", key.c_str());
                if (isNatEnabled())
                {
                    FieldValueTuple s(NAT_UDP_TIMEOUT, std::to_string(udp_timeout));
                    fvVector.push_back(s);
                }
            }

            if ((timeoutFound == true) and (timeout != m_natTimeout))
            {
                m_natTimeout = timeout;
                SWSS_LOG_INFO("NAT Timeout %s is added to cache", key.c_str());
                if (isNatEnabled())
                {
                    FieldValueTuple s(NAT_TIMEOUT, std::to_string(timeout));
                    fvVector.push_back(s);
                }
            }            

            if ((isNatEnabled() == true) and ((timeoutFound == true) or (tcpFound == true) or (udpFound == true)))
            {
                m_appNatGlobalTableProducer.set(appKey, fvVector);
                SWSS_LOG_INFO("Added NAT Values %s to APPL_DB", key.c_str());
            }

            if ((adminModeFound == true) and (adminMode != natAdminMode))
            {
                if (adminMode == ENABLED)
                {
                    SWSS_LOG_INFO("NAT Admin Mode enabled is added to cache");
                    natAdminMode = adminMode;
                    enableNatFeature();
                }
                else
                {
                    disableNatFeature();
                    natAdminMode = adminMode;
                    SWSS_LOG_INFO("NAT Admin Mode disabled is added to cache");
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            /* Create APPL_DB key */
            string appKey = VALUES;
            vector<FieldValueTuple> fvVector;

            /* Set NAT default timeout as 600 seconds */
            m_natTimeout = NAT_TIMEOUT_DEFAULT;
           
            /* Set NAT default tcp timeout as 86400 seconds (1 Day) */
            m_natTcpTimeout = NAT_TCP_TIMEOUT_DEFAULT;

            /* Set NAT default udp timeout as 300 seconds */
            m_natUdpTimeout = NAT_UDP_TIMEOUT_DEFAULT;
     
            if (natAdminMode == ENABLED)
            {
                FieldValueTuple p(NAT_TIMEOUT, std::to_string(m_natTimeout));
                FieldValueTuple q(NAT_UDP_TIMEOUT, std::to_string(m_natUdpTimeout));               
                FieldValueTuple r(NAT_TCP_TIMEOUT, std::to_string(m_natTcpTimeout));
                fvVector.push_back(p);
                fvVector.push_back(q);
                fvVector.push_back(r);                
                m_appNatGlobalTableProducer.set(appKey, fvVector);

                disableNatFeature();
                natAdminMode = DISABLED;
                SWSS_LOG_INFO("NAT Admin Mode disabled is added to cache");
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* To parse the received L3 Interface Table and save it to cache */
void NatMgr::doNatIpInterfaceTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t), nat_zone = "1";
        vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
        string op = kfvOp(t), port(keys[0]);
        bool skipAddition = false, skipDeletion = false;
        int prefixLen = 0, nat_zone_value = 1;
        vector<string> ipPrefixKeys;

        /* Example : Config_DB
         * INTERFACE|Ethernet28|10.0.0.1/24 
         * or
         * INTERFACE|Ethernet28
         * {
         *    nat_zone = "0"
         * }
         */

        /* Ensure the key size is 2 or 1, otherwise ignore */
        if ((keys.size() != L3_INTERFACE_KEY_SIZE) and (keys.size() != L3_INTERFACE_ZONE_SIZE))
        {
            SWSS_LOG_INFO("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        else
        {
            SWSS_LOG_INFO("Key size %lu for %s", keys.size(), key.c_str());
        }

        /* Ensure the key starts with "Vlan" or "Ethernet" or "PortChannel" or "Loopback", otherwise ignore */
        if ((strncmp(keys[0].c_str(), VLAN_PREFIX, strlen(VLAN_PREFIX))) and
            (strncmp(keys[0].c_str(), ETHERNET_PREFIX, strlen(ETHERNET_PREFIX))) and
            (strncmp(keys[0].c_str(), LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX))) and
            (strncmp(keys[0].c_str(), LAG_PREFIX, strlen(LAG_PREFIX))))
        {
            SWSS_LOG_INFO("Invalid key %s format, skipping %s", keys[0].c_str(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (keys.size() == L3_INTERFACE_KEY_SIZE)
        {
            ipPrefixKeys = tokenize(keys[1], ip_address_delimiter);
        
            /* Ensure the ipPrefix key size is 2 otherwise ignore */
            if (ipPrefixKeys.size() != IP_PREFIX_SIZE)
            {
                SWSS_LOG_INFO("Invalid IpPrefix size %s, skipping %s", keys[1].c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the ip address is ipv4, otherwise ignore */
            try
            {
                IpAddress ipAddress(ipPrefixKeys[0]);

                if (!ipAddress.isV4())
                {
                    /* Ignore the IPv6 addresses */
                    SWSS_LOG_INFO("IPv6 address is not supported, skipping %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }
            catch(...)
            {
                SWSS_LOG_INFO("Invalid ip address %s format, skipping %s", ipPrefixKeys[0].c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the given PrefixLen is integer, otherwise ignore */
            try
            {
                prefixLen = stoi(ipPrefixKeys[1]);
            }
            catch(...)
            {
                SWSS_LOG_ERROR("Invalid ip mask len %s, skipping %s", ipPrefixKeys[1].c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the ip mask len is valid, otherwise ignore */
            if ((prefixLen < IP_ADDR_MASK_LEN_MIN) or (prefixLen > IP_ADDR_MASK_LEN_MAX))
            {
                SWSS_LOG_INFO("Invalid ip mask len %s, skipping %s", ipPrefixKeys[1].c_str(), key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /*
             * Don't proceed if Port/LAG/VLAN is not ready yet.
             * The pending task will be checked periodically and retried.
             */
            if ((strncmp(keys[0].c_str(), LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX))) and 
                (!isPortStateOk(port)))
            {
                SWSS_LOG_INFO("Port is not ready, skipping %s", port.c_str());
                it++;
                continue;
            }

            if (keys.size() == L3_INTERFACE_ZONE_SIZE)
            {
                const vector<FieldValueTuple>& data = kfvFieldsValues(t);

                /* Get the Config_db key values */
                for (auto idx : data)
                {
                    if (fvField(idx) == NAT_ZONE)
                    {
                        /* Ensure the given nat_zone is integer, otherwise ignore */
                        try
                        {
                            nat_zone_value = stoi(fvValue(idx));
                        }
                        catch(...)
                        {
                            SWSS_LOG_ERROR("Invalid nat_zone %s, skipping %s", fvValue(idx).c_str(), key.c_str());
                            continue;
                        }

                        /* Add plus 1 to avoid adding default zero mark in iptables mangle table */
                        nat_zone_value++;
                        nat_zone = to_string(nat_zone_value);
                        break;
                    }      
                }

                /* Check the key is present in zone_interface cache */
                if (m_natZoneInterfaceInfo.find(port) != m_natZoneInterfaceInfo.end())
                {
                    /* Check the nat_zone is same or not */
                    if (m_natZoneInterfaceInfo[port] != nat_zone)
                    {
                        /* Delete the mangle iptables rules for non-loopback interface */
                        if (strncmp(keys[0].c_str(), LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX)))
                        {
                            setMangleIptablesRules(DELETE, port, m_natZoneInterfaceInfo[port]);
                        }

                        /* Check the port is present in ip_interface cache */
                        if (m_natIpInterfaceInfo.find(keys[0]) != m_natIpInterfaceInfo.end())
                        {
                            /* Delete the Static NAT and NAPT iptables rules */
                            SWSS_LOG_INFO("Deleting Static NAT iptables rules for %s", key.c_str());
                            removeStaticNatIptables(keys[0]);

                            SWSS_LOG_INFO("Deleting Static NAPT iptables rules for %s", key.c_str());
                            removeStaticNaptIptables(keys[0]);

                            /* Delete the Dynamic NAT rules */
                            SWSS_LOG_INFO("Deleting Dynamic NAT rules for %s", key.c_str());
                            removeDynamicNatRules(keys[0]);
                        }

                        m_natZoneInterfaceInfo[port] = nat_zone;

                        /* Add the mangle iptables rules for non-loopback interface */
                        if (strncmp(keys[0].c_str(), LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX)))
                        {
                            setMangleIptablesRules(ADD, port, nat_zone);
                        }

                        /* Check the port is present in ip_interface cache */
                        if (m_natIpInterfaceInfo.find(keys[0]) != m_natIpInterfaceInfo.end())
                        {
                            /* Add the Static NAT and NAPT iptables rules */
                            SWSS_LOG_INFO("Adding Static NAT iptables rules for %s", key.c_str());
                            addStaticNatIptables(keys[0]);

                            SWSS_LOG_INFO("Adding Static NAPT iptables rules for %s", key.c_str());
                            addStaticNaptIptables(keys[0]);

                            /* Add the Dynamic NAT rules */
                            SWSS_LOG_INFO("Adding Dynamic NAT rules for %s", key.c_str());
                            addDynamicNatRules(keys[0]);
                        }
                    }
                    else
                    {
                        SWSS_LOG_INFO("Received same nat_zone %s, skipping %s", nat_zone.c_str(), key.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                }
                else
                {
                    m_natZoneInterfaceInfo[port] = nat_zone;

                    /* Add the mangle iptables rules for non-loopback interface */
                    if (strncmp(keys[0].c_str(), LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX)))
                    {
                        setMangleIptablesRules(ADD, port, nat_zone);
                    }
                }
            }
            else if (keys.size() == L3_INTERFACE_KEY_SIZE)
            {
                /*
                 * Don't proceed if Interface is not ready yet.
                 * The pending task will be checked periodically and retried.
                 */
                if (!isIntfStateOk(key))
                {
                    SWSS_LOG_INFO("Interface is not ready, skipping %s", key.c_str());
                    it++;
                    continue;
                }

                /* Check the key is present in ip_interface cache */
                if (m_natIpInterfaceInfo.find(port) != m_natIpInterfaceInfo.end())
                {
                    if (m_natIpInterfaceInfo[port].find(keys[1]) != m_natIpInterfaceInfo[port].end())
                    {
                        SWSS_LOG_INFO("Duplicate Ip Interface, skipping %s", key.c_str());
                        skipAddition = true;
                    }
                    else
                    {
                        for (auto it = m_natIpInterfaceInfo[port].begin(); it != m_natIpInterfaceInfo[port].end(); it++)
                        {
                            IpPrefix entry(keys[1]), prefix(*it);

                            if (prefix.isAddressInSubnet(entry.getIp()))
                            {  
                                SWSS_LOG_INFO("IP Address %s belongs to existing subnet, skipped adding entries", ipPrefixKeys[0].c_str());
                                skipAddition = true;
                                break;
                            }
                        }
                    
                        m_natIpInterfaceInfo[port].insert(keys[1]);
                        SWSS_LOG_INFO("Ip Interface %s is added to the existing Port cache", key.c_str());
                    }
                }
                else
                {
                    m_natIpInterfaceInfo[port].insert(keys[1]);
                    SWSS_LOG_INFO("Ip Interface %s is added to the Port cache", key.c_str());
                }

                if (!skipAddition)
                {
                    /* Add the Static NAT and NAPT entries */
                    SWSS_LOG_INFO("Adding Static NAT entries for %s", key.c_str());
                    addStaticNatEntries(keys[0], keys[1]);

                    SWSS_LOG_INFO("Adding Static NAPT entries for %s", key.c_str());
                    addStaticNaptEntries(keys[0], keys[1]);

                    /* Add the Dynamic NAT rules */
                    SWSS_LOG_INFO("Adding Dynamic NAT rules for %s", key.c_str());
                    addDynamicNatRules(keys[0], keys[1]);
                }
            }
            else
            {
                SWSS_LOG_INFO("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            }
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Del command for %s", key.c_str());
            
            if (keys.size() == L3_INTERFACE_ZONE_SIZE)   
            {
                /* Check the key is present in zone_interface cache*/
                if (m_natZoneInterfaceInfo.find(port) != m_natZoneInterfaceInfo.end())
                {
                    /* Set the mangle iptables rules for non-loopback interface */
                    if (strncmp(keys[0].c_str(), LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX)))
                    {
                        setMangleIptablesRules(DELETE, port, m_natZoneInterfaceInfo[port]); 
                    }

                    SWSS_LOG_INFO("Nat Zone %s for Interface %s is cleaned from the cache", m_natZoneInterfaceInfo[port].c_str(), key.c_str());
                    m_natZoneInterfaceInfo.erase(port);
                }
                else
                {
                    SWSS_LOG_INFO("Zone Interface is not present in cache, skipping %s", key.c_str());
                }
            }
            else if (keys.size() == L3_INTERFACE_KEY_SIZE)
            {
                /* Check the key is present in ip_interface cache*/
                if (m_natIpInterfaceInfo.find(port) != m_natIpInterfaceInfo.end())
                {
                    if (m_natIpInterfaceInfo[port].find(keys[1]) != m_natIpInterfaceInfo[port].end())
                    {
                        for (auto ipPrefix = m_natIpInterfaceInfo[port].begin(); ipPrefix != m_natIpInterfaceInfo[port].end(); ipPrefix++)
                        {
                            if (*ipPrefix == keys[1])
                            {
                                continue; 
                            }

                            IpPrefix entry(keys[1]), prefix(*ipPrefix);

                            if (prefix.isAddressInSubnet(entry.getIp()))
                            {
                                SWSS_LOG_INFO("IP Address %s belongs to existing subnet, skipping deleting the entries", ipPrefixKeys[0].c_str());
                                skipDeletion = true;
                                break;
                            }
                        }

                        if (!skipDeletion)
                        {
                            /* Delete the Static NAT and NAPT entries */
                            SWSS_LOG_INFO("Deleting Static NAT entries for %s", key.c_str());
                            removeStaticNatEntries(keys[0], keys[1]);

                            SWSS_LOG_INFO("Deleting Static NAPT entries for %s", key.c_str());
                            removeStaticNaptEntries(keys[0], keys[1]);

                            /* Delete the Dynamic NAT rules */
                            SWSS_LOG_INFO("Deleting Dynamic NAT rules for %s", key.c_str());
                            removeDynamicNatRules(keys[0], keys[1]);
                        }

                        m_natIpInterfaceInfo[port].erase(keys[1]);
                        SWSS_LOG_INFO("Ip Interface %s is cleaned from the existing Port cache", keys[1].c_str());
                   
                        if (m_natIpInterfaceInfo[port].empty())
                        {
                            m_natIpInterfaceInfo.erase(port);
                            SWSS_LOG_INFO("Ip Interface %s is cleaned from the cache", key.c_str());
                        }
                    }
                    else
                    {
                        SWSS_LOG_INFO("Ip Interface %s from Config_Db not in cache, do nothing", keys[1].c_str());
                    }
                }
                else
                {
                    SWSS_LOG_INFO("Invalid Ip Interface %s from Config_Db, do nothing", key.c_str());
                }
            }
            else
            {
                SWSS_LOG_INFO("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            }
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_INFO("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* To parse the received ACL Table and save it to cache */
void NatMgr::doNatAclTableTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t), op = kfvOp(t), ports;
        vector<string> keys = tokenize(key, config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        bool isNatAclNotValid = false;        

        /* Example : Config_DB
         * ACL_TABLE|Table-Id
         *    policy_desc: nat_acl
         *    stage: INGRESS
         *    type: l3
         *    ports: Ethernet10
         */

        /* Ensure the key size is 1 otherwise ignore */
        if (keys.size() != ACL_TABLE_KEY_SIZE)
        {
            SWSS_LOG_INFO("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /* Get the Config_db key values */
            for (auto idx : data)
            {             
                const auto &field = to_upper(fvField(idx));
                const auto &value = fvValue(idx);
                if (field == TABLE_TYPE)
                {
                    /* Ensure the Table type is L3, otherwise ignore */
                    if (to_upper(value) != TABLE_TYPE_L3)
                    {
                        isNatAclNotValid = true;
                        SWSS_LOG_INFO("Invalid table type %s, skipping %s", value.c_str(), key.c_str());
                        break;
                    }
                }
                else if (field == TABLE_STAGE)
                {
                    /* Ensure the Table stage is Ingress, otherwise ignore */
                    if (to_upper(value) != TABLE_STAGE_INGRESS)
                    {
                        isNatAclNotValid = true;
                        SWSS_LOG_INFO("Invalid stage %s, skipping %s", value.c_str(), key.c_str());
                        break;
                    }                    
                }
                else if (field == TABLE_PORTS)
                {
                    ports = value;
                }
            }

            /* Ensure the Table ports starts with "Vlan" or "Ethernet" or "PortChannel" otherwise ignore */
            vector<string> ports_list = tokenize(ports, comma);
            for (string port : ports_list)
            {
                /* Ensure the key starts with "Vlan" or "Ethernet" or "PortChannel" otherwise ignore */
                if ((strncmp(port.c_str(), VLAN_PREFIX, strlen(VLAN_PREFIX))) and
                    (strncmp(port.c_str(), ETHERNET_PREFIX, strlen(ETHERNET_PREFIX))) and
                    (strncmp(port.c_str(), LAG_PREFIX, strlen(LAG_PREFIX))))
                {
                    SWSS_LOG_INFO("Invalid Port %s format, skipping %s", ports.c_str(), key.c_str());
                    isNatAclNotValid = true;
                    break;                    
                }
            }

            /* Ensure the ACL Table is valid for NAT, otherwise ignore */
            if (isNatAclNotValid == true)
            {
                SWSS_LOG_INFO("Not a valid ACL Table for NAT, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Check the key is already present in cache */
            if (m_natAclTableInfo.find(key) != m_natAclTableInfo.end())
            {
                SWSS_LOG_INFO("ACL Table %s exists, skipping", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            
            /* New Key, Add it to cache */
            m_natAclTableInfo[key] = ports;
            SWSS_LOG_INFO("ACL Table Info %s is added to cache", key.c_str());

            /* Add the new Dynamic Nat Rules */
            SWSS_LOG_INFO("Adding Dynamic NAT rules for %s", key.c_str());
            addDynamicNatRuleByAcl(key);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Del command for %s", key.c_str());

            /* Check the key is already present in cache */
            if (m_natAclTableInfo.find(key) != m_natAclTableInfo.end())
            {
                /* Remove the existing Dynamic NAT rules */
                SWSS_LOG_INFO("Deleting Dynamic NAT rules for %s", key.c_str());
                removeDynamicNatRuleByAcl(key);

                /* Clean the ACL Table info */
                m_natAclTableInfo.erase(key);
                SWSS_LOG_INFO("ACL Table Info %s is cleaned from cache", key.c_str());
            }
            else
            {
                SWSS_LOG_INFO("Invalid ACL Table %s from Config_Db, do nothing", key.c_str());
            }

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_INFO("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* To parse the received ACL Rule Table and save it to cache */
void NatMgr::doNatAclRuleTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t), op = kfvOp(t);
        vector<string> keys = tokenize(key, config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string aclSrcIpAddress = NONE_STRING, aclDstIpAddress = NONE_STRING, aclIpProtocol = NONE_STRING;
        string aclSrcPort = NONE_STRING, aclDstPort = NONE_STRING, aclPacketAction;
        bool isNatAclRuleNotValid = false, isPacketActionSet = false;
        uint32_t ipv4_addr, aclPriority = 0;
        uint8_t ip_protocol;

        /* Example : Config_DB
         * ACL_Rule|Table-Id|Rule-Id
         *    priority: 55
         *    ip_type: ipv4any
         *    src_ip: 10.10.0.26/32
         *    dst_ip: 10.10.1.26/32
         *    packet_action: forward
         */

        /* Ensure the key size is 2 otherwise ignore */
        if (keys.size() != ACL_RULE_TABLE_KEY_SIZE)
        {
            SWSS_LOG_INFO("Invalid key size %lu, skipping %s", keys.size(), key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set command for %s", key.c_str());

            /* Get the Config_db key values */
            for (auto idx : data)
            {
                const auto &field = to_upper(fvField(idx));
                const auto &value = fvValue(idx);

                if (field == ACTION_PACKET_ACTION)
                {
                    /* Ensure the packet action is valid, otherwise ignore */
                    if ((to_upper(value) != PACKET_ACTION_FORWARD) and (to_upper(value) != PACKET_ACTION_DO_NOT_NAT))
                    {
                        isNatAclRuleNotValid = true;
                        SWSS_LOG_INFO("Invalid packet action %s for Packet Action Field, skipping %s", value.c_str(), key.c_str());
                        break;
                    }              
                    isPacketActionSet = true;
                    aclPacketAction = to_upper(value);
                }
                else if (field == MATCH_IP_TYPE)
                {
                    /* Ensure the ip type is valid, otherwise ignore */
                    if ((to_upper(value) != IP_TYPE_IP) and (to_upper(value) != IP_TYPE_IPv4ANY))
                    {
                        isNatAclRuleNotValid = true;
                        SWSS_LOG_INFO("Invalid ip type %s for Matching IP Type Field, skipping %s", value.c_str(), key.c_str());
                        break;
                    }
                }
                else if ((field == MATCH_SRC_IP) or (field == MATCH_DST_IP))
                {
                    vector<string> acl_ip = tokenize(value, ip_address_delimiter);

                    /* Ensure the ip format is x.x.x.x, otherwise ignore */
                    if (inet_pton(AF_INET, acl_ip[0].c_str(), &ipv4_addr) != 1)
                    {
                        SWSS_LOG_INFO("Invalid ip address %s format for Matching IP Field, skipping %s", value.c_str(), key.c_str());
                        isNatAclRuleNotValid = true;
                        break;
                    }

                    /* Ensure the ip address is non-zero, otherwise ignore */
                    if (ipv4_addr == 0)
                    {
                        SWSS_LOG_INFO("Invalid ip address %s for Matching IP Field, skipping %s", value.c_str(), key.c_str());
                        isNatAclRuleNotValid = true;
                        break;
                    }

                    if (acl_ip.size() > 1)
                    {
                        /* Ensure the mask length is valid otherwise ignore */
                        if ((stoi(acl_ip[1]) < IP_ADDR_MASK_LEN_MIN) or (stoi(acl_ip[1]) > IP_ADDR_MASK_LEN_MAX))
                        {
                            SWSS_LOG_INFO("Invalid ip address mask length for Matching IP Field, skipping %s", key.c_str());
                            isNatAclRuleNotValid = true;
                            break;  
                        }
                    }
                
                    if (field == MATCH_SRC_IP)
                    {
                        aclSrcIpAddress = value;
                    }
                    else if (field == MATCH_DST_IP)
                    {
                        aclDstIpAddress = value; 
                    }
                }
                else if (field == MATCH_IP_PROTOCOL)
                {
                    ip_protocol = to_uint<uint8_t>(value);
                    
                    /* Ensure the ip protocol is TCP, UDP or ICMP, otherwise ignore */
                    if (ip_protocol == MATCH_IP_PROTOCOL_TCP)
                    {
                        aclIpProtocol = IP_PROTOCOL_TCP;
                    }
                    else if (ip_protocol == MATCH_IP_PROTOCOL_UDP)
                    {
                        aclIpProtocol = IP_PROTOCOL_UDP;
                    }
                    else if (ip_protocol == MATCH_IP_PROTOCOL_ICMP)
                    {
                        aclIpProtocol = IP_PROTOCOL_ICMP;
                    }
                    else
                    {
                       SWSS_LOG_INFO("Invalid ip protocol %d for Matching Ip Protocol Field, skipping %s", ip_protocol, key.c_str());
                       isNatAclRuleNotValid = true;
                       break;
                    }
                }
                else if ((field == MATCH_L4_SRC_PORT) or (field == MATCH_L4_DST_PORT))
                {
                    /* Ensure the port is inbetween 1 to 65535, otherwise ignore */
                    if ((stoi(value) < L4_PORT_MIN) or (stoi(value) > L4_PORT_MAX))
                    {
                       SWSS_LOG_INFO("Invalid port value %s for Matching Port Field, skipping %s", value.c_str(), key.c_str());
                       isNatAclRuleNotValid = true;
                       break;
                    }
                  
                    if (field == MATCH_L4_SRC_PORT)
                    {
                        aclSrcPort = value;
                    }
                    else if (field == MATCH_L4_DST_PORT)
                    {
                        aclDstPort = value;
                    }
                }
                else if ((field == MATCH_L4_SRC_PORT_RANGE) or (field == MATCH_L4_DST_PORT_RANGE))
                {
                    vector<string> port_range = tokenize(value, range_specifier);

                    /* Ensure the port range size is valid, otherwise ignore */
                    if (port_range.size() != L4_PORT_RANGE_SIZE)
                    {
                        SWSS_LOG_INFO("Invalid port range size %lu for Matching Port Range Field, skipping %s", port_range.size(), key.c_str());
                        isNatAclRuleNotValid = true;
                        break;
                    }

                    /* Ensure the port is inbetween 1 to 65535, otherwise ignore */
                    if ((stoi(port_range[0]) < L4_PORT_MIN) or (stoi(port_range[1]) > L4_PORT_MAX))
                    {
                       SWSS_LOG_INFO("Invalid port range %s for Matching Port Range Field, skipping %s", value.c_str(), key.c_str());
                       isNatAclRuleNotValid = true;
                       break;
                    }

                    if (field == MATCH_L4_SRC_PORT_RANGE)
                    {
                        aclSrcPort = value;
                    }
                    else if (field == MATCH_L4_DST_PORT_RANGE)
                    {
                        aclDstPort = value;
                    }
                }
                else if (field == RULE_PRIORITY)
                {
                    aclPriority = stoi(value); 
                }
                else
                {
                    isNatAclRuleNotValid = true;
                    break;
                }
            }

            /* Ensure the ACL Table Rule is valid for NAT, otherwise ignore */
            if (isNatAclRuleNotValid == true)
            {
                SWSS_LOG_INFO("Not a valid ACL Rule for NAT, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Ensure the ACL Rule has packet action for NAT, otherwise ignore */
            if (isPacketActionSet == false)
            {
                SWSS_LOG_INFO("Packet action is missing for NAT ACL, skipping %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Check the key is already present in cache */
            if (m_natAclRuleInfo.find(key) != m_natAclRuleInfo.end())
            {
                /* Remove the existing Dynamic NAT rules */
                SWSS_LOG_INFO("Deleting Dynamic NAT rules for %s", key.c_str());
                removeDynamicNatRuleByAcl(key, true);

                /* Clean the ACL Rule info */
                m_natAclRuleInfo.erase(key);
                SWSS_LOG_INFO("ACL Rule Info %s is cleaned from cache", key.c_str());
            }

            /* Save the ACL Rule info to cache */
            m_natAclRuleInfo[key].packet_action = aclPacketAction;
            m_natAclRuleInfo[key].priority = aclPriority;
            m_natAclRuleInfo[key].src_ip_range = aclSrcIpAddress;
            m_natAclRuleInfo[key].dst_ip_range = aclDstIpAddress;
            m_natAclRuleInfo[key].src_l4_port_range = aclSrcPort;
            m_natAclRuleInfo[key].dst_l4_port_range = aclDstPort;
            m_natAclRuleInfo[key].ip_protocol = aclIpProtocol;
            SWSS_LOG_INFO("ACL Rule Info %s is added to cache", key.c_str());

            /* Add the new Dynamic Nat Rules */
            SWSS_LOG_INFO("Adding Dynamic NAT rules for %s", key.c_str());
            addDynamicNatRuleByAcl(key, true);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Del command for %s", key.c_str());

            /* Check the key is already present in cache */
            if (m_natAclRuleInfo.find(key) != m_natAclRuleInfo.end())
            {
                /* Remove the existing Dynamic NAT rules */
                SWSS_LOG_INFO("Deleting Dynamic NAT rules for %s", key.c_str());
                removeDynamicNatRuleByAcl(key, true);

                /* Clean the ACL Rule info */
                m_natAclRuleInfo.erase(key);
                SWSS_LOG_INFO("ACL Rule Info %s is cleaned from cache", key.c_str());
            }
            else
            {
                SWSS_LOG_INFO("Invalid ACL Rule %s from Config_Db, do nothing", key.c_str());
            }

            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_INFO("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* To parse the received Table Task */
void NatMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name == CFG_STATIC_NAT_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received update from CFG_STATIC_NAT_TABLE_NAME");
        doStaticNatTask(consumer);
    }
    else if (table_name == CFG_STATIC_NAPT_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received update from CFG_STATIC_NAPT_TABLE_NAME");
        doStaticNaptTask(consumer);
    }
    else if (table_name == CFG_NAT_POOL_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received update from CFG_NAT_POOL_TABLE_NAME");
        doNatPoolTask(consumer);
    }
    else if (table_name == CFG_NAT_BINDINGS_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received update from CFG_NAT_BINDINGS_TABLE_NAME");
        doNatBindingTask(consumer);
    }
    else if (table_name == CFG_NAT_GLOBAL_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received update from CFG_NAT_GLOBAL_TABLE_NAME");
        doNatGlobalTask(consumer);
    }
    else if ((table_name == CFG_INTF_TABLE_NAME) || (table_name == CFG_LAG_INTF_TABLE_NAME) ||
             (table_name == CFG_VLAN_INTF_TABLE_NAME) || (table_name == CFG_LOOPBACK_INTERFACE_TABLE_NAME))
    {
        SWSS_LOG_INFO("Received update from CFG_INTF_TABLE_NAME");
        doNatIpInterfaceTask(consumer);
    }
    else if (table_name == CFG_ACL_TABLE_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received update from CFG_ACL_TABLE_TABLE_NAME");
        doNatAclTableTask(consumer);
    }
    else if (table_name == CFG_ACL_RULE_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received update from CFG_ACL_RULE_TABLE_NAME");
        doNatAclRuleTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown config table %s ", table_name.c_str());
        throw runtime_error("NatMgr doTask failure.");
    }
}

