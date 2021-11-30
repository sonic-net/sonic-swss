#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "intfmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include "macaddress.h"
#include "warm_restart.h"
#include "subscriberstatetable.h"
#include <swss/redisutility.h>
#include "subintf.h"

using namespace std;
using namespace swss;

#define VLAN_PREFIX         "Vlan"
#define LAG_PREFIX          "PortChannel"
#define SUBINTF_LAG_PREFIX  "Po"
#define LOOPBACK_PREFIX     "Loopback"
#define VNET_PREFIX         "Vnet"
#define MTU_INHERITANCE     "0"
#define VRF_PREFIX          "Vrf"
#define VRF_MGMT            "mgmt"

#define LOOPBACK_DEFAULT_MTU_STR "65536"
#define DEFAULT_MTU_STR 9100

IntfMgr::IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgIntfTable(cfgDb, CFG_INTF_TABLE_NAME),
        m_cfgVlanIntfTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME),
        m_cfgLagIntfTable(cfgDb, CFG_LAG_INTF_TABLE_NAME),
        m_cfgLoopbackIntfTable(cfgDb, CFG_LOOPBACK_INTERFACE_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateIntfTable(stateDb, STATE_INTERFACE_TABLE_NAME),
        m_appIntfTableProducer(appDb, APP_INTF_TABLE_NAME),
        m_neighTable(appDb, APP_NEIGH_TABLE_NAME),
        m_appLagTable(appDb, APP_LAG_TABLE_NAME)
{
    auto subscriberStateTable = new swss::SubscriberStateTable(stateDb,
            STATE_PORT_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 100);
    auto stateConsumer = new Consumer(subscriberStateTable, this, STATE_PORT_TABLE_NAME);
    Orch::addExecutor(stateConsumer);

    auto subscriberStateLagTable = new swss::SubscriberStateTable(stateDb,
            STATE_LAG_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 200);
    auto stateLagConsumer = new Consumer(subscriberStateLagTable, this, STATE_LAG_TABLE_NAME);
    Orch::addExecutor(stateLagConsumer);

    if (!WarmStart::isWarmStart())
    {
        flushLoopbackIntfs();
        WarmStart::setWarmStartState("intfmgrd", WarmStart::WSDISABLED);
    }
    else
    {
        //Build the interface list to be replayed to Kernel
        buildIntfReplayList();
        if (m_pendingReplayIntfList.empty())
        {
            setWarmReplayDoneState();
        }
    }
}

void IntfMgr::setIntfIp(const string &alias, const string &opCmd,
                        const IpPrefix &ipPrefix)
{
    stringstream    cmd;
    string          res;
    string          ipPrefixStr = ipPrefix.to_string();
    string          broadcastIpStr = ipPrefix.getBroadcastIp().to_string();
    int             prefixLen = ipPrefix.getMaskLength();

    if (ipPrefix.isV4())
    {
        (prefixLen < 31) ?
        (cmd << IP_CMD << " address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " broadcast " << shellquote(broadcastIpStr) <<" dev " << shellquote(alias)) :
        (cmd << IP_CMD << " address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " dev " << shellquote(alias));
    }
    else
    {
        (prefixLen < 127) ?
        (cmd << IP_CMD << " -6 address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " broadcast " << shellquote(broadcastIpStr) << " dev " << shellquote(alias)) :
        (cmd << IP_CMD << " -6 address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " dev " << shellquote(alias));
    }

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::setIntfMac(const string &alias, const string &mac_str)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link set " << alias << " address " << mac_str;

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::setIntfVrf(const string &alias, const string &vrfName)
{
    stringstream cmd;
    string res;

    if (!vrfName.empty())
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " master " << shellquote(vrfName);
    }
    else
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " nomaster";
    }
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

bool IntfMgr::setIntfMpls(const string &alias, const string& mpls)
{
    stringstream cmd;
    string res;

    if (mpls == "enable")
    {
        cmd << "sysctl -w net.mpls.conf." << alias << ".input=1";
    }
    else if ((mpls == "disable") || mpls.empty())
    {
        cmd << "sysctl -w net.mpls.conf." << alias << ".input=0";
    }
    else
    {
        SWSS_LOG_ERROR("MPLS state is invalid: \"%s\"", mpls.c_str());
        return false;
    }
    int ret = swss::exec(cmd.str(), res);
    // Don't return error unless MPLS is explicitly set
    if (ret && !mpls.empty())
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
    return true;
}

void IntfMgr::addLoopbackIntf(const string &alias)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link add " << alias << " mtu " << LOOPBACK_DEFAULT_MTU_STR << " type dummy && ";
    cmd << IP_CMD << " link set " << alias << " up";
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::delLoopbackIntf(const string &alias)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link del " << alias;
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::flushLoopbackIntfs()
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link show type dummy | grep -o '" << LOOPBACK_PREFIX << "[^:]*'";

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_DEBUG("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        return;
    }

    auto aliases = tokenize(res, '\n');
    for (string &alias : aliases)
    {
        SWSS_LOG_NOTICE("Remove loopback device %s", alias.c_str());
        delLoopbackIntf(alias);
    }
}

int IntfMgr::getIntfIpCount(const string &alias)
{
    stringstream cmd;
    string res;

    /* query ip address of the device with master name, it is much faster */
    // ip address show {{intf_name}}
    // $(ip link show {{intf_name}} | grep -o 'master [^\\s]*') ==> [master {{vrf_name}}]
    // | grep inet | grep -v 'inet6 fe80:' | wc -l
    cmd << IP_CMD << " address show " << alias
        << " $(" << IP_CMD << " link show " << alias << " | grep -o 'master [^\\s]*')"
        << " | grep inet | grep -v 'inet6 fe80:' | wc -l";

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        return 0;
    }

    return std::stoi(res);
}

void IntfMgr::buildIntfReplayList(void)
{
    vector<string> intfList;

    m_cfgIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    m_cfgLoopbackIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    m_cfgVlanIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    m_cfgLagIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    SWSS_LOG_INFO("Found %d Total Intfs to be replayed", (int)m_pendingReplayIntfList.size() );
}

void IntfMgr::setWarmReplayDoneState()
{
    m_replayDone = true;
    WarmStart::setWarmStartState("intfmgrd", WarmStart::REPLAYED);
    // There is no operation to be performed for intfmgr reconcillation
    // Hence mark it reconciled right away
    WarmStart::setWarmStartState("intfmgrd", WarmStart::RECONCILED);
}

bool IntfMgr::isIntfCreated(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (m_stateIntfTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("Intf %s is ready", alias.c_str());
        return true;
    }

    return false;
}

bool IntfMgr::isIntfChangeVrf(const string &alias, const string &vrfName)
{
    vector<FieldValueTuple> temp;

    if (m_stateIntfTable.get(alias, temp))
    {
        for (auto idx : temp)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "vrf")
            {
                if (value == vrfName)
                    return false;
                else
                    return true;
            }
        }
    }

    return false;
}

void IntfMgr::addHostSubIntf(const string&intf, const string &subIntf, const string &vlan)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD " link add link " << shellquote(intf) << " name " << shellquote(subIntf) << " type vlan id " << shellquote(vlan);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);
}


std::string IntfMgr::getIntfAdminStatus(const string &alias)
{
    Table *portTable;
    string admin = "down";
    if (!alias.compare(0, strlen("Eth"), "Eth"))
    {
        portTable = &m_statePortTable;
    }
    else if (!alias.compare(0, strlen("Po"), "Po"))
    {
        portTable = &m_appLagTable;
    }
    else
    {
        return admin;
    }

    vector<FieldValueTuple> temp;
    portTable->get(alias, temp);

    for (auto idx : temp)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);
        if (field == "admin_status")
        {
            admin = value;
        }
    }
    return admin;
}

std::string IntfMgr::getIntfMtu(const string &alias)
{
    Table *portTable;
    string mtu = "0";
    if (!alias.compare(0, strlen("Eth"), "Eth"))
    {
        portTable = &m_statePortTable;
    }
    else if (!alias.compare(0, strlen("Po"), "Po"))
    {
        portTable = &m_appLagTable;
    }
    else
    {
        return mtu;
    }
    vector<FieldValueTuple> temp;
    portTable->get(alias, temp);
    for (auto idx : temp)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);
        if (field == "mtu")
        {
            mtu = value;
        }
    }
    if (mtu.empty())
    {
        mtu = std::to_string(DEFAULT_MTU_STR);
    }
    return mtu;
}

void IntfMgr::updateSubIntfMtu(const string &alias, const string &mtu)
{
    string intf;
    for (auto entry : m_subIntfList)
    {
        intf = entry.first;
        subIntf subIf(intf);
        if (subIf.parentIntf() == alias)
        {
            std::vector<FieldValueTuple> fvVector;

            string subif_config_mtu = m_subIntfList[intf].mtu;
            if (subif_config_mtu == MTU_INHERITANCE || subif_config_mtu.empty())
                subif_config_mtu = std::to_string(DEFAULT_MTU_STR);

            string subintf_mtu = setHostSubIntfMtu(intf, subif_config_mtu, mtu);

            FieldValueTuple fvTuple("mtu", subintf_mtu);
            fvVector.push_back(fvTuple);
            m_appIntfTableProducer.set(intf, fvVector);
        }
    }
}

std::string IntfMgr::setHostSubIntfMtu(const string &alias, const string &mtu, const string &parent_mtu)
{
    stringstream cmd;
    string res;

    string subifMtu = mtu;
    subIntf subIf(alias);

    int pmtu = (uint32_t)stoul(parent_mtu);
    int cmtu = (uint32_t)stoul(mtu);

    if (pmtu < cmtu)
    {
        subifMtu = parent_mtu;
    }
    SWSS_LOG_INFO("subintf %s active mtu: %s", alias.c_str(), subifMtu.c_str());
    cmd << IP_CMD " link set " << shellquote(alias) << " mtu " << shellquote(subifMtu);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    return subifMtu;
}

void IntfMgr::updateSubIntfAdminStatus(const string &alias, const string &admin)
{
    string intf;
    for (auto entry : m_subIntfList)
    {
        intf = entry.first;
        subIntf subIf(intf);
        if (subIf.parentIntf() == alias)
        {
            /*  Avoid duplicate interface admin UP event. */
            string curr_admin = m_subIntfList[intf].currAdminStatus;
            if (curr_admin == "up" && curr_admin == admin)
            {
                continue;
            }
            std::vector<FieldValueTuple> fvVector;
            string subintf_admin = setHostSubIntfAdminStatus(intf, m_subIntfList[intf].adminStatus, admin); 
            m_subIntfList[intf].currAdminStatus = subintf_admin;
            FieldValueTuple fvTuple("admin_status", subintf_admin);
            fvVector.push_back(fvTuple);
            m_appIntfTableProducer.set(intf, fvVector);
        }
    }
}

std::string IntfMgr::setHostSubIntfAdminStatus(const string &alias, const string &admin_status, const string &parent_admin_status)
{
    stringstream cmd;
    string res;

    if (parent_admin_status == "up" || admin_status == "down")
    {
        SWSS_LOG_INFO("subintf %s admin_status: %s", alias.c_str(), admin_status.c_str());
        cmd << IP_CMD " link set " << shellquote(alias) << " " << shellquote(admin_status);
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
        return admin_status;
    }
    else
    {
        return "down";
    }
}

void IntfMgr::removeHostSubIntf(const string &subIntf)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD " link del " << shellquote(subIntf);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);
}

void IntfMgr::setSubIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> fvTuples = {{"state", "ok"}};

    if (!alias.compare(0, strlen(SUBINTF_LAG_PREFIX), SUBINTF_LAG_PREFIX))
    {
        m_stateLagTable.set(alias, fvTuples);
    }
    else
    {
        // EthernetX using PORT_TABLE
        m_statePortTable.set(alias, fvTuples);
    }
}

void IntfMgr::removeSubIntfState(const string &alias)
{
    if (!alias.compare(0, strlen(SUBINTF_LAG_PREFIX), SUBINTF_LAG_PREFIX))
    {
        m_stateLagTable.del(alias);
    }
    else
    {
        // EthernetX using PORT_TABLE
        m_statePortTable.del(alias);
    }
}

bool IntfMgr::setIntfGratArp(const string &alias, const string &grat_arp)
{
    /*
     * Enable gratuitous ARP by accepting unsolicited ARP replies
     */
    stringstream cmd;
    string res;
    string garp_enabled;

    if (grat_arp == "enabled")
    {
        garp_enabled = "1";
    }
    else if (grat_arp == "disabled")
    {
        garp_enabled = "0";
    }
    else
    {
        SWSS_LOG_ERROR("GARP state is invalid: \"%s\"", grat_arp.c_str());
        return false;
    }

    cmd << ECHO_CMD << " " << garp_enabled << " > /proc/sys/net/ipv4/conf/" << alias << "/arp_accept";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_INFO("ARP accept set to \"%s\" on interface \"%s\"",  grat_arp.c_str(), alias.c_str());
    return true;
}

bool IntfMgr::setIntfProxyArp(const string &alias, const string &proxy_arp)
{
    stringstream cmd;
    string res;
    string proxy_arp_pvlan;

    if (proxy_arp == "enabled")
    {
        proxy_arp_pvlan = "1";
    }
    else if (proxy_arp == "disabled")
    {
        proxy_arp_pvlan = "0";
    }
    else
    {
        SWSS_LOG_ERROR("Proxy ARP state is invalid: \"%s\"", proxy_arp.c_str());
        return false;
    }

    cmd << ECHO_CMD << " " << proxy_arp_pvlan << " > /proc/sys/net/ipv4/conf/" << alias << "/proxy_arp_pvlan";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_INFO("Proxy ARP set to \"%s\" on interface \"%s\"", proxy_arp.c_str(), alias.c_str());
    return true;
}

bool IntfMgr::isIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vlan %s is ready", alias.c_str());
            return true;
        }
    }
    else if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Lag %s is ready", alias.c_str());
            return true;
        }
    }
    else if (!alias.compare(0, strlen(VNET_PREFIX), VNET_PREFIX))
    {
        if (m_stateVrfTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vnet %s is ready", alias.c_str());
            return true;
        }
    }
    else if ((!alias.compare(0, strlen(VRF_PREFIX), VRF_PREFIX)) ||
            (alias == VRF_MGMT))
    {
        if (m_stateVrfTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vrf %s is ready", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        auto state_opt = swss::fvsGetValue(temp, "state", true);
        if (!state_opt)
        {
            return false;
        }
        SWSS_LOG_DEBUG("Port %s is ready", alias.c_str());
        return true;
    }
    else if (!alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX))
    {
        return true;
    }
    else if (!alias.compare(0, strlen(SUBINTF_LAG_PREFIX), SUBINTF_LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Lag %s is ready", alias.c_str());
            return true;
        }
    }

    return false;
}

void IntfMgr::delIpv6LinkLocalNeigh(const string &alias)
{
    vector<string> neighEntries;

    SWSS_LOG_INFO("Deleting ipv6 link local neighbors for %s", alias.c_str());

    m_neighTable.getKeys(neighEntries);
    for (auto neighKey : neighEntries)
    {
        if (!neighKey.compare(0, alias.size(), alias.c_str()))
        {
            vector<string> keys = tokenize(neighKey, ':', 1);
            if (keys.size() == 2)
            {
                IpAddress ipAddress(keys[1]);
                if (ipAddress.getAddrScope() == IpAddress::AddrScope::LINK_SCOPE)
                {
                    stringstream cmd;
                    string res;

                    cmd << IP_CMD << " neigh del dev " << keys[0] << " " << keys[1] ;
                    swss::exec(cmd.str(), res);
                    SWSS_LOG_INFO("Deleted ipv6 link local neighbor - %s", keys[1].c_str());
                }
            }
        }
    }
}

bool IntfMgr::doIntfGeneralTask(const vector<string>& keys,
        vector<FieldValueTuple> data,
        const string& op)
{
    SWSS_LOG_ENTER();

    string alias(keys[0]);
    string vlanId;
    string parentAlias;
    size_t found = alias.find(VLAN_SUB_INTERFACE_SEPARATOR);
    if (found != string::npos)
    {
        subIntf subIf(alias);
        // alias holds the complete sub interface name
        // while parentAlias holds the parent port name
        /*Check if subinterface is valid and sub interface name length is < 15(IFNAMSIZ)*/
        if (!subIf.isValid())
        {
            SWSS_LOG_ERROR("Invalid subnitf: %s", alias.c_str());
            return true;
        }
        parentAlias = subIf.parentIntf();
        int subIntfId = subIf.subIntfIdx();
        /*If long name format, subinterface Id is vlanid */
        if (!subIf.isShortName())
        {
            vlanId = std::to_string(subIntfId);
            FieldValueTuple vlanTuple("vlan", vlanId);
            data.push_back(vlanTuple);
        }
    }
    bool is_lo = !alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX);
    string mac = "";
    string vrf_name = "";
    string mtu = "";
    string adminStatus = "";
    string nat_zone = "";
    string proxy_arp = "";
    string grat_arp = "";
    string mpls = "";
    string ipv6_link_local_mode = "";

    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);

        if (field == "vnet_name" || field == "vrf_name")
        {
            vrf_name = value;
        }
        else if (field == "mac_addr")
        {
            mac = value;
        }
        else if (field == "admin_status")
        {
            adminStatus = value;
        }
        else if (field == "proxy_arp")
        {
            proxy_arp = value;
        }
        else if (field == "grat_arp")
        {
            grat_arp = value;
        }
        else if (field == "mpls")
        {
            mpls = value;
        }
        else if (field == "nat_zone")
        {
            nat_zone = value;
        }
        else if (field == "ipv6_use_link_local_only")
        {
            ipv6_link_local_mode = value;
        }
        else if (field == "vlan")
        {
            vlanId = value;
        }
    }

    if (op == SET_COMMAND)
    {
        if (!isIntfStateOk(parentAlias.empty() ? alias : parentAlias))
        {
            SWSS_LOG_DEBUG("Interface is not ready, skipping %s", alias.c_str());
            return false;
        }

        if (!vrf_name.empty() && !isIntfStateOk(vrf_name))
        {
            SWSS_LOG_DEBUG("VRF is not ready, skipping %s", vrf_name.c_str());
            return false;
        }

        /* if to change vrf then skip */
        if (isIntfChangeVrf(alias, vrf_name))
        {
            SWSS_LOG_ERROR("%s can not change to %s directly, skipping", alias.c_str(), vrf_name.c_str());
            return true;
        }

        if (is_lo)
        {
            if (m_loopbackIntfList.find(alias) == m_loopbackIntfList.end())
            {
                addLoopbackIntf(alias);
                m_loopbackIntfList.insert(alias);
                SWSS_LOG_INFO("Added %s loopback interface", alias.c_str());
            }
        }
        else
        {
            /* Set nat zone */
            if (!nat_zone.empty())
            {
                FieldValueTuple fvTuple("nat_zone", nat_zone);
                data.push_back(fvTuple);
            }

            /* Set mpls */
            if (!setIntfMpls(alias, mpls))
            {
                SWSS_LOG_ERROR("Failed to set MPLS to \"%s\" for the \"%s\" interface", mpls.c_str(), alias.c_str());
                return false;
            }
            if (!mpls.empty())
            {
                FieldValueTuple fvTuple("mpls", mpls);
                data.push_back(fvTuple);
            }

            /* Set ipv6 mode */
            if (!ipv6_link_local_mode.empty())
            {
                if ((ipv6_link_local_mode == "enable") && (m_ipv6LinkLocalModeList.find(alias) == m_ipv6LinkLocalModeList.end()))
                {
                    m_ipv6LinkLocalModeList.insert(alias);
                    SWSS_LOG_INFO("Inserted ipv6 link local mode list for %s", alias.c_str());
                }
                else if ((ipv6_link_local_mode == "disable") && (m_ipv6LinkLocalModeList.find(alias) != m_ipv6LinkLocalModeList.end()))
                {
                    m_ipv6LinkLocalModeList.erase(alias);
                    delIpv6LinkLocalNeigh(alias);
                    SWSS_LOG_INFO("Erased ipv6 link local mode list for %s", alias.c_str());
                }
                FieldValueTuple fvTuple("ipv6_use_link_local_only", ipv6_link_local_mode);
                data.push_back(fvTuple);
            }
        }

        if (!parentAlias.empty())
        {
            subIntf subIf(alias);
            if (m_subIntfList.find(alias) == m_subIntfList.end())
            {
                if (vlanId == "0" || vlanId.empty())
                {
                    SWSS_LOG_INFO("Vlan ID not configured for sub interface %s", alias.c_str());
                    return false;
                }
                try
                {
                    addHostSubIntf(parentAlias, alias, vlanId);
                }
                catch (const std::runtime_error &e)
                {
                    SWSS_LOG_NOTICE("Sub interface ip link add failure. Runtime error: %s", e.what());
                    return false;
                }

                m_subIntfList[alias].vlanId = vlanId;
            }

            if (!mtu.empty())
            {
                string subintf_mtu;
                try
                {
                    string parentMtu = getIntfMtu(subIf.parentIntf());
                    subintf_mtu = setHostSubIntfMtu(alias, mtu, parentMtu);
                    FieldValueTuple fvTuple("mtu", mtu);
                    std::remove(data.begin(), data.end(), fvTuple);
                    FieldValueTuple newMtuFvTuple("mtu", subintf_mtu);
                    data.push_back(newMtuFvTuple);
                }
                catch (const std::runtime_error &e)
                {
                    SWSS_LOG_NOTICE("Sub interface ip link set mtu failure. Runtime error: %s", e.what());
                    return false;
                }
                m_subIntfList[alias].mtu = mtu;
            }
            else
            {
                FieldValueTuple fvTuple("mtu", MTU_INHERITANCE);
                data.push_back(fvTuple);
                m_subIntfList[alias].mtu = MTU_INHERITANCE;
            }

            if (adminStatus.empty())
            {
                adminStatus = "up";
                FieldValueTuple fvTuple("admin_status", adminStatus);
                data.push_back(fvTuple);
            }
            try
            {
                string parentAdmin = getIntfAdminStatus(subIf.parentIntf());
                string subintf_admin = setHostSubIntfAdminStatus(alias, adminStatus, parentAdmin);
                m_subIntfList[alias].currAdminStatus = subintf_admin;
                FieldValueTuple fvTuple("admin_status", adminStatus);
                std::remove(data.begin(), data.end(), fvTuple);
                FieldValueTuple newAdminFvTuple("admin_status", subintf_admin);
                data.push_back(newAdminFvTuple);
            }
            catch (const std::runtime_error &e)
            {
                SWSS_LOG_NOTICE("Sub interface ip link set admin status %s failure. Runtime error: %s", adminStatus.c_str(), e.what());
                return false;
            }
            m_subIntfList[alias].adminStatus = adminStatus;

            // set STATE_DB port state
            setSubIntfStateOk(alias);
        }

        if (!vrf_name.empty())
        {
            setIntfVrf(alias, vrf_name);
        }

        /*Set the mac of interface*/
        if (!mac.empty())
        {
            setIntfMac(alias, mac);
        }
        else
        {
            FieldValueTuple fvTuple("mac_addr", MacAddress().to_string());
            data.push_back(fvTuple);
        }

        if (!proxy_arp.empty())
        {
            if (!setIntfProxyArp(alias, proxy_arp))
            {
                SWSS_LOG_ERROR("Failed to set proxy ARP to \"%s\" state for the \"%s\" interface", proxy_arp.c_str(), alias.c_str());
                return false;
            }

            if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
            {
                FieldValueTuple fvTuple("proxy_arp", proxy_arp);
                data.push_back(fvTuple);
            }
        }

        if (!grat_arp.empty())
        {
            if (!setIntfGratArp(alias, grat_arp))
            {
                SWSS_LOG_ERROR("Failed to set ARP accept to \"%s\" state for the \"%s\" interface", grat_arp.c_str(), alias.c_str());
                return false;
            }

            if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
            {
                FieldValueTuple fvTuple("grat_arp", grat_arp);
                data.push_back(fvTuple);
            }
        }

        m_appIntfTableProducer.set(alias, data);
        m_stateIntfTable.hset(alias, "vrf", vrf_name);
    }
    else if (op == DEL_COMMAND)
    {
        /* make sure all ip addresses associated with interface are removed, otherwise these ip address would
           be set with global vrf and it may cause ip address conflict. */
        if (getIntfIpCount(alias))
        {
            return false;
        }

        setIntfVrf(alias, "");

        if (is_lo)
        {
            delLoopbackIntf(alias);
            m_loopbackIntfList.erase(alias);
        }

        if (!parentAlias.empty())
        {
            removeHostSubIntf(alias);
            m_subIntfList.erase(alias);

            removeSubIntfState(alias);
        }

        if (m_ipv6LinkLocalModeList.find(alias) != m_ipv6LinkLocalModeList.end())
        {
            m_ipv6LinkLocalModeList.erase(alias);
            delIpv6LinkLocalNeigh(alias);
            SWSS_LOG_INFO("Erased ipv6 link local mode list for %s", alias.c_str());
        }

        m_appIntfTableProducer.del(alias);
        m_stateIntfTable.del(alias);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }

    return true;
}

bool IntfMgr::doIntfAddrTask(const vector<string>& keys,
        const vector<FieldValueTuple>& data,
        const string& op)
{
    SWSS_LOG_ENTER();

    string alias(keys[0]);
    IpPrefix ip_prefix(keys[1]);
    string appKey = keys[0] + ":" + keys[1];

    if (op == SET_COMMAND)
    {
        /*
         * Don't proceed if port/LAG/VLAN/subport and intfGeneral is not ready yet.
         * The pending task will be checked periodically and retried.
         */
        if (!isIntfStateOk(alias) || !isIntfCreated(alias))
        {
            SWSS_LOG_DEBUG("Interface is not ready, skipping %s", alias.c_str());
            return false;
        }

        setIntfIp(alias, "add", ip_prefix);

        std::vector<FieldValueTuple> fvVector;
        FieldValueTuple f("family", ip_prefix.isV4() ? IPV4_NAME : IPV6_NAME);

        // Don't send ipv4 link local config to AppDB and Orchagent
        if ((ip_prefix.isV4() == false) || (ip_prefix.getIp().getAddrScope() != IpAddress::AddrScope::LINK_SCOPE))
        {
            FieldValueTuple s("scope", "global");
            fvVector.push_back(s);
            fvVector.push_back(f);
            m_appIntfTableProducer.set(appKey, fvVector);
            m_stateIntfTable.hset(keys[0] + state_db_key_delimiter + keys[1], "state", "ok");
        }
    }
    else if (op == DEL_COMMAND)
    {
        setIntfIp(alias, "del", ip_prefix);

        // Don't send ipv4 link local config to AppDB and Orchagent
        if ((ip_prefix.isV4() == false) || (ip_prefix.getIp().getAddrScope() != IpAddress::AddrScope::LINK_SCOPE))
        {
            m_appIntfTableProducer.del(appKey);
            m_stateIntfTable.del(keys[0] + state_db_key_delimiter + keys[1]);
        }
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }

    return true;
}

void IntfMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        if ((table_name == STATE_PORT_TABLE_NAME) || (table_name == STATE_LAG_TABLE_NAME))
        {
            doPortTableTask(kfvKey(t), kfvFieldsValues(t), kfvOp(t));
        }
        else
        {
            vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
            const vector<FieldValueTuple>& data = kfvFieldsValues(t);
            string op = kfvOp(t);

            if (keys.size() == 1)
            {
                if((table_name == CFG_VOQ_INBAND_INTERFACE_TABLE_NAME) &&
                        (op == SET_COMMAND))
                {
                    //No further processing needed. Just relay to orchagent
                    m_appIntfTableProducer.set(keys[0], data);
                    m_stateIntfTable.hset(keys[0], "vrf", "");

                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                if (!doIntfGeneralTask(keys, data, op))
                {
                    it++;
                    continue;
                }
                else
                {
                    //Entry programmed, remove it from pending list if present
                    m_pendingReplayIntfList.erase(keys[0]);
                }
            }
            else if (keys.size() == 2)
            {
                if (!doIntfAddrTask(keys, data, op))
                {
                    it++;
                    continue;
                }
                else
                {
                    //Entry programmed, remove it from pending list if present
                    m_pendingReplayIntfList.erase(keys[0] + config_db_key_delimiter + keys[1] );
                }
            }
            else
            {
                SWSS_LOG_ERROR("Invalid key %s", kfvKey(t).c_str());
            }
        }

        it = consumer.m_toSync.erase(it);
    }

    if (!m_replayDone && WarmStart::isWarmStart() && m_pendingReplayIntfList.empty() )
    {
        setWarmReplayDoneState();
    }
}

void IntfMgr::doPortTableTask(const string& key, vector<FieldValueTuple> data, string op)
{
    if (op == SET_COMMAND)
    {
        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);

            if (field == "admin_status")
            {
                SWSS_LOG_INFO("Port %s Admin %s", key.c_str(), value.c_str());
                updateSubIntfAdminStatus(key, value);
            }
            else if (field == "mtu")
            {
                SWSS_LOG_INFO("Port %s MTU %s", key.c_str(), value.c_str());
                updateSubIntfMtu(key, value);
            }
        }
    }
}
