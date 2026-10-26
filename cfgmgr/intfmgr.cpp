#include <string.h>
#include <algorithm>
#include <cerrno>
#include <net/if.h>
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
extern MacAddress gMacAddress;
extern MacAddress gSagMacAddress;

static bool isIntfKey(const string &key, const string &alias, char delimiter)
{
    return key == alias || key.compare(0, alias.size() + 1, alias + delimiter) == 0;
}

static bool isLagIntfKey(const string &key, const string &lag, char delimiter)
{
    auto alias = key.substr(0, key.find(delimiter));
    subIntf subIf(alias);
    return alias == lag || (subIf.isValid() && subIf.parentIntf() == lag);
}

IntfMgr::IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgIntfTable(cfgDb, CFG_INTF_TABLE_NAME),
        m_cfgVlanIntfTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME),
        m_cfgLagIntfTable(cfgDb, CFG_LAG_INTF_TABLE_NAME),
        m_cfgLoopbackIntfTable(cfgDb, CFG_LOOPBACK_INTERFACE_TABLE_NAME),
        m_cfgSagTable(cfgDb, CFG_SAG_TABLE_NAME),
        m_cfgSubIntfTable(cfgDb, CFG_VLAN_SUB_INTF_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateIntfTable(stateDb, STATE_INTERFACE_TABLE_NAME),
        m_appIntfTableProducer(appDb, APP_INTF_TABLE_NAME),
        m_appSagTableProducer(appDb, APP_SAG_TABLE_NAME),
        m_neighTable(appDb, APP_NEIGH_TABLE_NAME),
        m_appLagTable(appDb, APP_LAG_TABLE_NAME),
        m_appIntfTable(appDb, APP_INTF_TABLE_NAME)
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

    string swtype;
    Table cfgDeviceMetaDataTable(cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    if(cfgDeviceMetaDataTable.hget("localhost", "switch_type", swtype))
    {
       mySwitchType = swtype;
    }
}

bool IntfMgr::setIntfIp(const string &alias, const string &opCmd,
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
        string metric = "";
        // Kernel adds connected route with default metric of 256. But the metric is not
        // communicated to frr unless the ip address is added with explicit metric
        // In voq system, We need the static route to the remote neighbor and connected
        // route to have the same metric to enable BGP to choose paths from routes learned
        // via eBGP and iBGP over the internal inband port be part of same ecmp group.
        // For v4 both the metrics (connected and static) are default 0 so we do not need
        // to set the metric explicitly.
        if(mySwitchType == "voq")
        {
           metric = " metric 256";
        }

        (prefixLen < 127) ?
        (cmd << IP_CMD << " -6 address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " broadcast " << shellquote(broadcastIpStr) <<
         " dev " << shellquote(alias) << metric) :
        (cmd << IP_CMD << " -6 address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " dev " << shellquote(alias) << metric);
    }

    try
    {
        int ret = swss::exec(cmd.str(), res);
        if (ret && !ipPrefix.isV4() && opCmd == "add")
        {
            SWSS_LOG_NOTICE("Failed to assign IPv6 on interface %s with return code %d, trying to enable IPv6 and retry", alias.c_str(), ret);
            if (!enableIpv6Flag(alias))
            {
                SWSS_LOG_ERROR("Failed to enable IPv6 on interface %s", alias.c_str());
            }
            else
            {
                ret = swss::exec(cmd.str(), res);
            }
        }
        if (ret == 0)
            return true;
        res = "rc=" + to_string(ret) + ": " + res;
    }
    catch (const runtime_error &e)
    {
        res = e.what();
    }

    const bool present = isIntfIpPresent(alias, ipPrefix);
    if ((opCmd == "add" && present) || (opCmd == "del" && !present))
    {
        SWSS_LOG_INFO("Address %s is already %s on interface %s",
                ipPrefixStr.c_str(), present ? "present" : "absent", alias.c_str());
        return true;
    }

    SWSS_LOG_ERROR("Command '%s' failed: %s", cmd.str().c_str(), res.c_str());
    return false;
}

void IntfMgr::setSagFdbEntry(const string &op, const string &alias, const string &mac_str)
{
    stringstream cmd;
    string res;

    if (op != "add" && op != "replace" && op != "del")
    {
        SWSS_LOG_ERROR("Invalid FDB operation '%s' for MAC %s on %s", op.c_str(), mac_str.c_str(), alias.c_str());
        return;
    }

    if (mac_str == gMacAddress.to_string())
    {
        // Don't add or del for global system MAC address
        return;
    }

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        /* Ensure the key starts with "Vlan" otherwise ignore */
        int vlan_id;
        try
        {
            vlan_id = stoi(alias.substr(4));
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Invalid Vlan alias format. Not a number after 'Vlan' prefix: %s", alias.c_str());
            return;
        }

        // cmd format: bridge fdb add 00:11:22:33:44:55 dev Bridge vlan 3 permanent
        cmd << "bridge fdb " << op << " " << mac_str << " dev Bridge vlan " << vlan_id << " permanent";

        int ret = swss::exec(cmd.str(), res);
        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        }
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

void IntfMgr::setIntfState(const string &alias, bool isUp)
{
    stringstream cmd;
    string res;

    if (isUp)
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " up";
    }
    else
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " down";
    }

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::addLoopbackIntf(const string &alias)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link add " << alias << " mtu " << LOOPBACK_DEFAULT_MTU_STR << " type dummy";
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

bool IntfMgr::isIntfIpPresent(const string &alias, const IpPrefix &ipPrefix)
{
    string res;
    try
    {
        const string cmd = string(IP_CMD) + " -o address show dev " + shellquote(alias);
        const string address = string(ipPrefix.isV4() ? " inet " : " inet6 ") + ipPrefix.to_string() + " ";
        return swss::exec(cmd, res) == 0 && res.find(address) != string::npos;
    }
    catch (const exception &e)
    {
        SWSS_LOG_WARN("Cannot query addresses on interface %s: %s", alias.c_str(), e.what());
        return false;
    }
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

    m_cfgSubIntfTable.getKeys(intfList);
    for (const auto &key : intfList)
    {
        subIntf subIf(key.substr(0, key.find(config_db_key_delimiter)));
        if (subIf.isValid() && !subIf.parentIntf().compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
            m_pendingReplayIntfList.insert(key);
    }

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

bool IntfMgr::isSubIntfPresent(const string &parent, const string &alias, const string &vlan)
{
    string res;
    try
    {
        const string cmd = string(IP_CMD) + " -d -o link show dev " + shellquote(alias);
        return swss::exec(cmd, res) == 0 &&
            res.find(" " + alias + "@" + parent + ": ") != string::npos &&
            res.find(" vlan protocol 802.1Q id " + vlan + " ") != string::npos;
    }
    catch (const exception &e)
    {
        SWSS_LOG_WARN("Cannot query subinterface %s: %s", alias.c_str(), e.what());
        return false;
    }
}

bool IntfMgr::addHostSubIntf(const string&intf, const string &subIntf, const string &vlan)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD " link add link " << shellquote(intf) << " name " << shellquote(subIntf) << " type vlan id " << shellquote(vlan);
    try
    {
        int ret = swss::exec(cmd.str(), res);
        if (ret == 0)
            return true;
        res = "rc=" + to_string(ret) + ": " + res;
    }
    catch (const runtime_error &e)
    {
        res = e.what();
    }
    if (isSubIntfPresent(intf, subIntf, vlan))
    {
        SWSS_LOG_INFO("Subinterface %s is already present", subIntf.c_str());
        return true;
    }
    SWSS_LOG_ERROR("Command '%s' failed: %s", cmd.str().c_str(), res.c_str());
    return false;
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
        portTable = &m_stateLagTable;
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
        portTable = &m_stateLagTable;
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
            if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX) &&
                (!isIntfStateOk(intf) || !isIntfCreated(intf)))
                continue;
            std::vector<FieldValueTuple> fvVector;

            string subif_config_mtu = m_subIntfList[intf].mtu;
            if (subif_config_mtu == MTU_INHERITANCE || subif_config_mtu.empty())
                subif_config_mtu = std::to_string(DEFAULT_MTU_STR);

            string subintf_mtu = setHostSubIntfMtu(intf, subif_config_mtu, mtu);
            if (subintf_mtu.empty())
                continue;

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
    std::string cmd_str = cmd.str();
    int ret = swss::exec(cmd_str, res);

    if (ret && !isIntfStateOk(alias))
    {
        // Can happen when a SET notification on the PORT_TABLE in the State DB
        // followed by a new DEL notification that send by portmgrd
        SWSS_LOG_WARN("Setting mtu to %s netdev failed with cmd:%s, rc:%d, error:%s", alias.c_str(), cmd_str.c_str(), ret, res.c_str());
        return "";
    }
    else if (ret)
    {
        throw runtime_error(cmd_str + " : " + res);
    }
    return subifMtu;
}

bool IntfMgr::setIntfAdminStatus(const string &alias, const string &admin_status)
{
    stringstream cmd;
    string res, cmd_str;

    try
    {
        SWSS_LOG_INFO("intf %s admin_status: %s", alias.c_str(), admin_status.c_str());
        cmd << IP_CMD " link set " << shellquote(alias) << " " << shellquote(admin_status);
        cmd_str = cmd.str();
        int ret = swss::exec(cmd_str, res);
        if (ret)
        {
            SWSS_LOG_WARN("Setting admin_status to %s netdev failed with cmd:%s, rc:%d, error:%s",
                          alias.c_str(), cmd_str.c_str(), ret, res.c_str());
        }
        return ret == 0;
    }
    catch (const runtime_error &e)
    {
        SWSS_LOG_WARN("Setting admin status on interface %s failed: %s", alias.c_str(), e.what());
        return false;
    }
}

bool IntfMgr::isIntfAdminStatusSet(const string &alias, const string &admin_status)
{
    string res;
    try
    {
        const string cmd = string(IP_CMD) + " -o link show dev " + shellquote(alias);
        if (swss::exec(cmd, res) != 0)
            return false;
        const auto start = res.find('<');
        const auto end = res.find('>', start);
        if (start == string::npos || end == string::npos)
            return false;
        const auto flags = tokenize(res.substr(start + 1, end - start - 1), ',');
        const bool up = find(flags.begin(), flags.end(), "UP") != flags.end();
        return up == (admin_status == "up");
    }
    catch (const exception &e)
    {
        SWSS_LOG_WARN("Cannot query admin status on interface %s: %s", alias.c_str(), e.what());
        return false;
    }
}

void IntfMgr::removeHostSubIntf(const string &subIntf)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD " link del " << shellquote(subIntf);
    try
    {
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }
    catch (const runtime_error &)
    {
        errno = 0;
        if (if_nametoindex(subIntf.c_str()) != 0 || (errno != ENODEV && errno != ENXIO))
            throw;
    }
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
     * Enable gratuitous ARP by accepting unsolicited ARP replies and untracked neighbor advertisements
     */
    stringstream cmd;
    string res;
    string garp_enabled;
    int rc;

    if (grat_arp == "enabled")
    {
        garp_enabled = "2";
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

    cmd.clear();
    cmd.str(std::string());

    // `accept_untracked_na` is not available in all kernels, so check for it before trying to set it
    cmd << "test -f /proc/sys/net/ipv6/conf/" << alias << "/accept_untracked_na";
    rc = swss::exec(cmd.str(), res);

    if (rc == 0) {
        cmd.clear();
        cmd.str(std::string());
        cmd << ECHO_CMD << " " << garp_enabled << " > /proc/sys/net/ipv6/conf/" << alias << "/accept_untracked_na";
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
        SWSS_LOG_INFO("`accept_untracked_na` set to \"%s\" on interface \"%s\"",  grat_arp.c_str(), alias.c_str());
    }

    return true;
}

bool IntfMgr::setIntfProxyArp(const string &alias, const string &proxy_arp)
{
    stringstream cmd;
    string res;
    string proxy_arp_status;

    if (proxy_arp == "enabled")
    {
        proxy_arp_status = "1";
    }
    else if (proxy_arp == "disabled")
    {
        proxy_arp_status = "0";
    }
    else
    {
        SWSS_LOG_ERROR("Proxy ARP state is invalid: \"%s\"", proxy_arp.c_str());
        return false;
    }

    cmd << ECHO_CMD << " " << proxy_arp_status << " > /proc/sys/net/ipv4/conf/" << alias << "/proxy_arp_pvlan";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    cmd.clear();
    cmd.str(std::string());

    cmd << ECHO_CMD << " " << proxy_arp_status << " > /proc/sys/net/ipv4/conf/" << alias << "/proxy_arp";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_INFO("Proxy ARP set to \"%s\" on interface \"%s\"", proxy_arp.c_str(), alias.c_str());
    return true;
}

bool IntfMgr::isLagIntfConfigRequired(const string &key)
{
    vector<FieldValueTuple> temp;
    return m_cfgLagIntfTable.get(key, temp) ||
           m_cfgSubIntfTable.get(key, temp);
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
    string sag = "";
    string loopback_action = "";

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
        else if (field == "static_anycast_gateway")
        {
            sag = value;
        }
        else if (field == "vlan")
        {
            vlanId = value;
        }
        else if (field == "loopback_action")
        {
            loopback_action = value;
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

            if (adminStatus.empty())
            {
                adminStatus = "up";
            }
            else if (adminStatus != "up" && adminStatus != "down")
            {
                SWSS_LOG_WARN("Got incorrect value for admin_status as %s for intf %s, defaulting as up", adminStatus.c_str(), alias.c_str());
                adminStatus = "up";
            }

            try
            {
                if (setIntfAdminStatus(alias, adminStatus))
                {
                    FieldValueTuple newAdminFvTuple("admin_status", adminStatus);
                    data.push_back(newAdminFvTuple);
                }
            }
            catch (const std::runtime_error &e)
            {
                SWSS_LOG_WARN("Lo interface ip link set admin status %s failure. Runtime error: %s", adminStatus.c_str(), e.what());
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

            /* Set loopback action */
            if (!loopback_action.empty())
            {
                FieldValueTuple fvTuple("loopback_action", loopback_action);
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

            if (!ipv6_link_local_mode.empty())
            {
                if (ipv6_link_local_mode == "enable" && m_ipv6LinkLocalModeList.count(alias) == 0)
                {
                    m_ipv6LinkLocalModeList.insert(alias);
                    SWSS_LOG_INFO("Inserted ipv6 link local mode list for %s", alias.c_str());
                }
                else if (ipv6_link_local_mode == "disable" && m_ipv6LinkLocalModeList.erase(alias))
                {
                    delIpv6LinkLocalNeigh(alias);
                    SWSS_LOG_INFO("Erased ipv6 link local mode list for %s", alias.c_str());
                }
                data.emplace_back("ipv6_use_link_local_only", ipv6_link_local_mode);
            }
        }

        if (!parentAlias.empty())
        {
            subIntf subIf(alias);
            if (vlanId == "0" || vlanId.empty())
            {
                SWSS_LOG_INFO("Vlan ID not configured for sub interface %s", alias.c_str());
                return false;
            }
            if (!addHostSubIntf(parentAlias, alias, vlanId))
            {
                return false;
            }

            m_subIntfList[alias].vlanId = vlanId;

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
                if (adminStatus != "up" && adminStatus != "down")
                    throw runtime_error("Invalid admin status \"" + adminStatus + "\" for " + alias);
                string parentAdmin = getIntfAdminStatus(subIf.parentIntf());
                if (parentAdmin != "up" && adminStatus == "up")
                    return false;
                if (!setIntfAdminStatus(alias, adminStatus) &&
                    !isIntfAdminStatusSet(alias, adminStatus))
                    return false;
                FieldValueTuple fvTuple("admin_status", adminStatus);
                std::remove(data.begin(), data.end(), fvTuple);
                FieldValueTuple newAdminFvTuple("admin_status", adminStatus);
                data.push_back(newAdminFvTuple);
            }
            catch (const std::runtime_error &e)
            {
                SWSS_LOG_NOTICE("Sub interface ip link set admin status %s failure. Runtime error: %s", adminStatus.c_str(), e.what());
                return false;
            }
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
            if (!sag.empty())
            {
                // only VLAN interface can set static anycast gateway
                if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
                {
                    string gwmac = "";
                    if (m_cfgSagTable.hget("GLOBAL", "gateway_mac", gwmac))
                    {
                        // before change interface MAC, set interface down and up to regenerate IPv6 LL by MAC
                        if (sag == "true")
                        {
                            m_sagIntfList[alias] = true;

                            setIntfState(alias, false);
                            setIntfMac(alias, gwmac);
                            setIntfState(alias, true);
                            // add this MAC fdb into bridge
                            setSagFdbEntry("replace", alias, gwmac);

                            FieldValueTuple fvTuple("mac_addr", gwmac);
                            data.push_back(fvTuple);
                        }
                        else if (sag == "false")
                        {
                            m_sagIntfList[alias] = false;

                            // del the sag MAC fdb from bridge
                            setSagFdbEntry("del", alias, gSagMacAddress.to_string());

                            setIntfState(alias, false);
                            setIntfMac(alias, gMacAddress.to_string());
                            setIntfState(alias, true);

                            FieldValueTuple fvTuple("mac_addr", MacAddress().to_string());
                            data.push_back(fvTuple);
                        } else {
                            SWSS_LOG_ERROR("invalid SAG config \"%s\", it should be \"true\" or \"false\"", sag.c_str());
                        }
                    }
                }
            }
            else
            {
                FieldValueTuple fvTuple("mac_addr", MacAddress().to_string());
                data.push_back(fvTuple);
            }
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
        if (!parentAlias.empty())
            setSubIntfStateOk(alias);
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

        if (m_sagIntfList.find(alias) != m_sagIntfList.end())
        {
            if (m_sagIntfList[alias] == true)
            {
                // del the sag MAC fdb from bridge
                setSagFdbEntry("del", alias, gSagMacAddress.to_string());

                // recover to global mac address
                setIntfState(alias, false);
                setIntfMac(alias, gMacAddress.to_string());
                setIntfState(alias, true);
            }
            m_sagIntfList.erase(alias);
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

        if (!setIntfIp(alias, "add", ip_prefix))
        {
            return false;
        }

        if (!ip_prefix.isV4() && ip_prefix.getIp().getAddrScope() == IpAddress::AddrScope::LINK_SCOPE)
        {
            m_intfLLAddresses[alias].insert(ip_prefix.to_string());
        }

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
        if (!setIntfIp(alias, "del", ip_prefix))
        {
            return false;
        }

        if (!ip_prefix.isV4() && ip_prefix.getIp().getAddrScope() == IpAddress::AddrScope::LINK_SCOPE)
        {
            auto it = m_intfLLAddresses.find(alias);
            if (it != m_intfLLAddresses.end())
            {
                it->second.erase(ip_prefix.to_string());
                if (it->second.empty())
                {
                    m_intfLLAddresses.erase(it);
                }
            }
        }

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

void IntfMgr::doSagTask(const vector<string>& keys,
        const vector<FieldValueTuple> &data,
        const string& op)
{
    SWSS_LOG_ENTER();

    string mac = "";
    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);

        if (field == "gateway_mac")
        {
            mac = value;
        }
    }

    vector<FieldValueTuple> fvAppSag;
    if (op == SET_COMMAND)
    {
        if (mac.empty())
        {
            SWSS_LOG_ERROR("gateway_mac field is missing in SAG configuration");
            return;
        }
        FieldValueTuple gwmac("gateway_mac", MacAddress(mac).to_string());
        fvAppSag.push_back(gwmac);
        m_appSagTableProducer.set("GLOBAL", fvAppSag);

        updateSagMac(mac);
    }
    else if (op == DEL_COMMAND)
    {
        m_appSagTableProducer.del("GLOBAL");

        // reset mac address for enabled static-anycast-gateway's VLAN interfaces
        updateSagMac(gMacAddress.to_string());
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }
}

void IntfMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();
    if (table_name == STATE_LAG_TABLE_NAME)
        consumer.refillToSync();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        if (table_name == STATE_LAG_TABLE_NAME)
        {
            const string &key = kfvKey(t);
            const string &op = kfvOp(t);
            subIntf stateSubIf(key);
            const bool parentDel = op == DEL_COMMAND && !stateSubIf.isValid() &&
                                   !key.compare(0, strlen(LAG_PREFIX), LAG_PREFIX);

            doLagTableTask(key, kfvFieldsValues(t), op);
            if (parentDel)
            {
                for (auto stateIt = consumer.m_toSync.begin(); stateIt != consumer.m_toSync.end();)
                {
                    if (kfvOp(stateIt->second) == SET_COMMAND &&
                        isLagIntfKey(stateIt->first, key, config_db_key_delimiter))
                        stateIt = consumer.m_toSync.erase(stateIt);
                    else
                        ++stateIt;
                }

                vector<FieldValueTuple> current;
                if (m_stateLagTable.get(key, current))
                    doLagTableTask(key, current, SET_COMMAND);
            }
        }
        else if (table_name == STATE_PORT_TABLE_NAME)
        {
            doPortTableTask(kfvKey(t), kfvFieldsValues(t), kfvOp(t));
        }
        else
        {
            vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
            const vector<FieldValueTuple>& data = kfvFieldsValues(t);
            string op = kfvOp(t);

            string lag;
            if (!keys.empty() && table_name == CFG_LAG_INTF_TABLE_NAME)
                lag = keys[0];
            else if (!keys.empty() && table_name == CFG_VLAN_SUB_INTF_TABLE_NAME)
            {
                subIntf subIf(keys[0]);
                if (subIf.isValid() && !subIf.parentIntf().compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
                    lag = subIf.parentIntf();
            }
            if (!lag.empty())
            {
                // Reopening re-reads CONFIG_DB, including any changed/deleted child rows.
                if (op == SET_COMMAND && (m_readyLagIntfs.count(lag) == 0 || !isIntfStateOk(lag)))
                {
                    if (!isLagIntfConfigRequired(kfvKey(t)))
                        m_pendingReplayIntfList.erase(kfvKey(t));
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                auto *parent = getConsumerBase(CFG_LAG_INTF_TABLE_NAME);
                if (op == SET_COMMAND && table_name == CFG_VLAN_SUB_INTF_TABLE_NAME && parent &&
                    any_of(parent->m_toSync.begin(), parent->m_toSync.end(), [&](const auto &task) {
                        return isIntfKey(task.first, lag, config_db_key_delimiter);
                    }))
                {
                    ++it;
                    continue;
                }
            }

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

                if (table_name == CFG_SAG_TABLE_NAME)
                {
                    doSagTask(keys, data, op);
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                if (!doIntfGeneralTask(keys, data, op))
                {
                    // A retained CONFIG DEL must finish before the replacement SET for this key.
                    if (op == DEL_COMMAND && !lag.empty())
                        it = consumer.m_toSync.upper_bound(kfvKey(t));
                    else
                        ++it;
                    continue;
                }
                else
                {
                    // A replacement SET may have been cancelled while CONFIG still requires this entry.
                    if (op == SET_COMMAND ||
                        (consumer.m_toSync.count(kfvKey(t)) == 1 &&
                         (lag.empty() || !isLagIntfConfigRequired(kfvKey(t)))))
                        m_pendingReplayIntfList.erase(keys[0]);
                }
            }
            else if (keys.size() == 2)
            {
                // Retained base STATE metadata does not imply that its pending configuration was applied.
                if (op == SET_COMMAND && !lag.empty() && consumer.m_toSync.count(keys[0]))
                {
                    ++it;
                    continue;
                }
                if (!doIntfAddrTask(keys, data, op))
                {
                    if (op == DEL_COMMAND)
                        it = consumer.m_toSync.upper_bound(kfvKey(t));
                    else
                        ++it;
                    continue;
                }
                else
                {
                    if (op == SET_COMMAND ||
                        (consumer.m_toSync.count(kfvKey(t)) == 1 &&
                         (lag.empty() || !isLagIntfConfigRequired(kfvKey(t)))))
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

void IntfMgr::clearLagIntfTasks(const string &alias)
{
    for (const auto &table : {CFG_LAG_INTF_TABLE_NAME, CFG_VLAN_SUB_INTF_TABLE_NAME})
    {
        auto *consumer = getConsumerBase(table);
        if (!consumer)
            continue;
        // Include the subscriber's startup snapshot and already-buffered notifications.
        consumer->refillToSync();
        for (auto it = consumer->m_toSync.begin(); it != consumer->m_toSync.end();)
        {
            if (kfvOp(it->second) == SET_COMMAND && isLagIntfKey(it->first, alias, config_db_key_delimiter))
                it = consumer->m_toSync.erase(it);
            else
                ++it;
        }
    }

    for (auto it = m_pendingReplayIntfList.begin(); it != m_pendingReplayIntfList.end();)
    {
        // Cancelling executable SETs does not complete still-configured startup obligations.
        if (isLagIntfKey(*it, alias, config_db_key_delimiter) &&
            !isLagIntfConfigRequired(*it))
            it = m_pendingReplayIntfList.erase(it);
        else
            ++it;
    }
}

void IntfMgr::enqueueLagIntfConfig(const string &alias)
{
    clearLagIntfTasks(alias);
    // Successful normal address tasks rebuild the link-local replay cache.
    for (auto it = m_intfLLAddresses.begin(); it != m_intfLLAddresses.end();)
    {
        if (isLagIntfKey(it->first, alias, config_db_key_delimiter))
            it = m_intfLLAddresses.erase(it);
        else
            ++it;
    }
    vector<string> appKeys;
    m_appIntfTable.getKeys(appKeys);
    for (auto *table : {&m_cfgLagIntfTable, &m_cfgSubIntfTable})
    {
        auto *consumer = getConsumerBase(table->getTableName());
        if (!consumer)
            continue;
        vector<string> keys;
        table->getKeys(keys);
        map<string, vector<FieldValueTuple>> intfConfig;
        for (const auto &key : keys)
        {
            vector<FieldValueTuple> data;
            if (isLagIntfKey(key, alias, config_db_key_delimiter) && table->get(key, data))
            {
                // Queue a full CONFIG snapshot, not a partial field update.
                if (m_ipv6LinkLocalModeList.count(key) &&
                    none_of(data.begin(), data.end(), [](const auto &field) {
                        return fvField(field) == "ipv6_use_link_local_only";
                    }))
                    data.emplace_back("ipv6_use_link_local_only", "disable");
                intfConfig.emplace(key, data);
            }
        }
        // Retained projection/cache entries absent from current CONFIG need normal DEL cleanup.
        for (auto key : appKeys)
        {
            auto separator = key.find(':');
            auto intf = key.substr(0, separator);
            if (!isLagIntfKey(key, alias, ':') ||
                subIntf(intf).isValid() != (table == &m_cfgSubIntfTable))
                continue;
            if (separator != string::npos)
                key[separator] = config_db_key_delimiter;
            if (intfConfig.count(key) == 0)
                consumer->addToSync(KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
        }
        if (table == &m_cfgSubIntfTable)
        {
            for (const auto &entry : m_subIntfList)
                if (isLagIntfKey(entry.first, alias, config_db_key_delimiter) && intfConfig.count(entry.first) == 0)
                    consumer->addToSync(KeyOpFieldsValuesTuple(entry.first, DEL_COMMAND, {}));
        }
        // Queue parent work before child work, and each general key before its addresses.
        for (const auto &entry : intfConfig)
        {
            consumer->addToSync(KeyOpFieldsValuesTuple(entry.first, SET_COMMAND, entry.second));
            if (!m_replayDone && WarmStart::isWarmStart())
                m_pendingReplayIntfList.insert(entry.first);
        }
    }
}

void IntfMgr::withdrawLagIntf(const string &alias)
{
    vector<string> keys;
    m_stateIntfTable.getKeys(keys);
    for (const auto &key : keys)
    {
        if (key.find(state_db_key_delimiter) != string::npos &&
            isLagIntfKey(key, alias, state_db_key_delimiter))
        {
            m_stateIntfTable.del(key);
        }
    }

    m_stateLagTable.getKeys(keys);
    for (const auto &key : keys)
    {
        subIntf subIf(key);
        if (subIf.isValid() && subIf.parentIntf() == alias)
            m_stateLagTable.del(key);
    }
}

void IntfMgr::doLagTableTask(const string &key, const vector<FieldValueTuple> &data, const string &op)
{
    // Subinterface readiness is owned here, not a new parent LAG interval.
    if (subIntf(key).isValid() || key.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        doPortTableTask(key, data, op);
        return;
    }

    if (op == DEL_COMMAND)
    {
        m_readyLagIntfs.erase(key);
        clearLagIntfTasks(key);
        withdrawLagIntf(key);
        return;
    }

    bool ready = isIntfStateOk(key);
    bool reopened = false;
    if (op == SET_COMMAND && ready)
    {
        reopened = m_readyLagIntfs.insert(key).second;
        if (reopened)
            enqueueLagIntfConfig(key);
    }
    else if (op == SET_COMMAND && !ready)
    {
        m_readyLagIntfs.erase(key);
    }

    doPortTableTask(key, data, op, ready && !reopened);
}

void IntfMgr::doPortTableTask(const string& key, vector<FieldValueTuple> data, string op, bool replayLinkLocal)
{
    subIntf subIf(key);
    string parent = subIf.isValid() ? subIf.parentIntf() : key;
    if (!parent.compare(0, strlen(LAG_PREFIX), LAG_PREFIX) &&
        (m_readyLagIntfs.count(parent) == 0 || !isIntfStateOk(parent) ||
         (subIf.isValid() && !isIntfStateOk(key))))
        return;

    if (op == SET_COMMAND)
    {
        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);

            if (field == "admin_status")
            {
                SWSS_LOG_INFO("Port %s Admin %s", key.c_str(), value.c_str());

                if (replayLinkLocal && value == "up" && m_intfLLAddresses.count(key) > 0)
                {
                    replayLLIntfAddresses(key);
                }
            }
            else if (field == "mtu")
            {
                SWSS_LOG_INFO("Port %s MTU %s", key.c_str(), value.c_str());
                updateSubIntfMtu(key, value);
            }
        }
    }
}

void IntfMgr::updateSagMac(const std::string &macAddr)
{
    vector<string> keys;
    m_cfgVlanIntfTable.getKeys(keys);
    for (auto &key: keys)
    {
        vector<string> entryKeys = tokenize(key, config_db_key_delimiter);
        if (key.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
        {
            continue;
        }

        // only process the entry includes the SAG's config
        // e.g. VLAN_INTERFACE|Vlan201
        if (entryKeys.size() != 1)
        {
            continue;
        }

        string value = "";
        if (m_cfgVlanIntfTable.hget(key, "static_anycast_gateway", value))
        {
            if (value == "true")
            {
                SWSS_LOG_NOTICE("set %s mac address to %s", key.c_str(), macAddr.c_str());

                // enable SAG, set device down and up to regenerate IPv6 LL by MAC
                setIntfState(key, false);
                setIntfMac(key, macAddr);
                setIntfState(key, true);

                // remove the previous sag MAC fdb from bridge
                setSagFdbEntry("del", key, gSagMacAddress.to_string());
                // add this new MAC fdb into bridge, the "replace" could cover both "add" and "replace" cases.
                setSagFdbEntry("replace", key, macAddr);

                vector<FieldValueTuple> vlanIntFv;

                // Get other fields to set them all together
                m_cfgVlanIntfTable.get(key, vlanIntFv);

                // keep consistent with default MAC 00:00:00:00:00:00
                string entryMac = MacAddress().to_string();
                if (macAddr != gMacAddress.to_string())
                {
                    entryMac = macAddr;
                }

                FieldValueTuple fvTuple("mac_addr", entryMac);
                vlanIntFv.push_back(fvTuple);
                m_appIntfTableProducer.set(key, vlanIntFv);
            }
        } else {
            SWSS_LOG_INFO("can't get %s in VLAN_INTERFACE table", key.c_str());
        }
    }
    gSagMacAddress = MacAddress(macAddr);
}

bool IntfMgr::enableIpv6Flag(const string &alias)
{
    stringstream cmd;
    string temp_res;
    cmd << "sysctl -w net.ipv6.conf." << shellquote(alias) << ".disable_ipv6=0";
    int ret = swss::exec(cmd.str(), temp_res);
    SWSS_LOG_INFO("disable_ipv6 flag is set to 0 for iface: %s, cmd: %s, ret: %d", alias.c_str(), cmd.str().c_str(), ret);
    return (ret == 0) ? true : false;
}

void IntfMgr::replayLLIntfAddresses(const string &alias)
{
    auto it = m_intfLLAddresses.find(alias);
    if (it == m_intfLLAddresses.end())
    {
        return;
    }

    for (const auto &addr : it->second)
    {
        IpPrefix ipPrefix(addr);
        if (setIntfIp(alias, "add", ipPrefix))
        {
            SWSS_LOG_INFO("Replayed IPv6 link-local address %s on interface %s after admin up",
                addr.c_str(), alias.c_str());
        }
        else
        {
            SWSS_LOG_WARN("Failed to replay IPv6 link-local address %s on interface %s",
                addr.c_str(), alias.c_str());
        }
    }
}
