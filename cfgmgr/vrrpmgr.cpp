#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "vrrpmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include <swss/redisutility.h>
#include <swss/stringutility.h>

using namespace std;
using namespace swss;

#define VRRP_V4_MAC_PREFIX "00:00:5e:00:01:"
#define VRRP_V6_MAC_PREFIX "00:00:5e:00:02:"

VrrpMgr::VrrpMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tableNames) : 
        Orch(cfgDb, tableNames),
        m_cfgVrrpTable(cfgDb, CFG_PORT_TABLE_NAME),
        m_appPortTable(appDb, APP_PORT_TABLE_NAME),
        m_appVrrpTableProducer(appDb, APP_VRRP_TABLE_NAME)
{
}

bool VrrpMgr::setIntfArpAccept(const std::string &intf_alias, const bool back_default)
{
    stringstream cmd;
    string res;

    if (!back_default)
    {
        cmd << ECHO_CMD << " 2 > /proc/sys/net/ipv4/conf/" << shellquote(intf_alias) << "/arp_announce && ";
        cmd << ECHO_CMD << " 2 > /proc/sys/net/ipv4/conf/" << shellquote(intf_alias) << "/rp_filter && ";
        cmd << ECHO_CMD << " 1 > /proc/sys/net/ipv4/conf/" << shellquote(intf_alias) << "/accept_local";
    }
    else
    {
        cmd << ECHO_CMD << " 0 > /proc/sys/net/ipv4/conf/" << shellquote(intf_alias) << "/arp_announce && ";
        cmd << ECHO_CMD << " 0 > /proc/sys/net/ipv4/conf/" << shellquote(intf_alias) << "/rp_filter && ";
        cmd << ECHO_CMD << " 0 > /proc/sys/net/ipv4/conf/" << shellquote(intf_alias) << "/accept_local";
    }

    try
    {
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to set intf arp %s on interface[%s], retry. Runtime error: %s", back_default ? "default" : "accept", intf_alias.c_str(), e.what());
        return false;
    }

    SWSS_LOG_INFO("Set vrrp arp %s on interface[%s]", back_default ? "default" : "accept", intf_alias.c_str());
    return true;
}

bool VrrpMgr::setVrrpIntf(const std::string &intf_alias, const std::string &vrid, const set<IpPrefix> &vip_list)
{
    auto vrrp = m_vrrpList.find(vrid);
    set<IpPrefix> original_vips = vrrp != m_vrrpList.end() ? vrrp->second.vips : set<IpPrefix>();
    set<IpPrefix> diff_vips;
    set<IpPrefix> apply_vips;

    for (const IpPrefix &vip : vip_list)
    {
        MacAddress vmac;
        bool is_ipv4 = vip.isV4();
        bool addVrrpMacTable = false;

        if (!parseVrrpMac(vrid, is_ipv4, vmac))
        {
            continue;
        }
        if (!isVrrpOnIntf(intf_alias))
        {
            SWSS_LOG_INFO("No vrrp on interface[%s], set arp accept", intf_alias.c_str());
            if (!setIntfArpAccept(intf_alias))
            {
                continue;
            }
        }

        // if vrrp has added new mac(new ip type) to vrif
        if (vrrp == m_vrrpList.end())
        {
            m_vrrpList[vrid] = VrrpIntfConf{intf_alias, {}, "", ""};
            addVrrpMacTable = true;

            vrrp = m_vrrpList.find(vrid);
        }
        else
        {
            if (none_of(m_vrrpList[vrid].vips.begin(), m_vrrpList[vrid].vips.end(),
                        [is_ipv4](const auto &has_vip){ return has_vip.isV4() == is_ipv4; }))
            {
                addVrrpMacTable = true;
            }
        }
        // add vrrp mac to appl
        if (addVrrpMacTable)
        {
            SWSS_LOG_INFO("Add vrrp mac to vrif");
            VrrpIntf vrrp_intf(intf_alias, vrid, is_ipv4);
            addVirtualInterface(intf_alias, vrrp_intf.getVrrpName(), vmac);
            if (is_ipv4)
            {
                m_vrrpList[vrid].v4mac = vmac;
            }
            else
            {
                m_vrrpList[vrid].v6mac = vmac;
            }

            string appKey = join(delimiter, intf_alias, vrid, vrrp_intf.isIpv4() ? IPV4_NAME : IPV6_NAME);
            std::vector<FieldValueTuple> fvVector;
            fvVector.push_back(FieldValueTuple("vmac", vmac.to_string()));
            m_appVrrpTableProducer.set(appKey, fvVector);

            // add to collection to avoid retrigger
            vrrp->second.vips.emplace(vip);
        }
        apply_vips.emplace(vip);
    }

    // if no vip
    if (vrrp == m_vrrpList.end())
    {
        m_vrrpList[vrid] = VrrpIntfConf{intf_alias, {}, "", ""};
    }

    set_symmetric_difference(original_vips.begin(), original_vips.end(), apply_vips.begin(), apply_vips.end(), std::inserter(diff_vips, diff_vips.begin()));
    SWSS_LOG_INFO("original_ip size:%d, apply_vips size:%d, diff size:%d", (int)original_vips.size(), (int)apply_vips.size(), (int)diff_vips.size());

    for (const IpPrefix &diff_ip : diff_vips)
    {
        if (apply_vips.find(diff_ip) != apply_vips.end())
        {
            // add vip
            addVirtualInterfaceIp(vrid, diff_ip);
        }

        if (original_vips.find(diff_ip) != original_vips.end())
        {
            // del vip
            delVirtualInterfaceIp(vrid, diff_ip);
        }
    }
    m_vrrpList[vrid].vips = apply_vips;

    SWSS_LOG_NOTICE("Set vrrp on intf[%s] vrid[%s]", intf_alias.c_str(), vrid.c_str());
    return true;
}

bool VrrpMgr::removeVrrpIntf(const std::string &intf_alias, const std::string &vrid)
{
    auto it = m_vrrpList.find(vrid);
    if (it == m_vrrpList.end())
    {
        SWSS_LOG_INFO("Not found vrid: %s", vrid.c_str());
        return true;
    }

    VrrpIntf vrrp_intf;
    if (it->second.v4mac != "")
    {
        vrrp_intf = VrrpIntf(intf_alias, vrid, true);
    }
    if (it->second.v6mac != "")
    {
        vrrp_intf = VrrpIntf(intf_alias, vrid, false);
    }
    delVirtualInterface(intf_alias, vrrp_intf.getVrrpName());
    string appKey = join(delimiter, intf_alias, vrid, vrrp_intf.isIpv4() ? IPV4_NAME : IPV6_NAME);
    m_appVrrpTableProducer.del(appKey);

    m_vrrpList.erase(it);

    if (!isVrrpOnIntf(intf_alias))
    {
        SWSS_LOG_INFO("No vrrp on intf[%s], arp accept back to default", intf_alias.c_str());
        setIntfArpAccept(intf_alias, true);
    }

    SWSS_LOG_NOTICE("Remove vrrp on intf[%s] vrid[%s]", intf_alias.c_str(), vrid.c_str());
    return true;
}

bool VrrpMgr::addVirtualInterface(const std::string &intf_alias, const std::string &vrrp_name, const MacAddress &vrrp_mac)
{
    stringstream cmd;
    string res;

    // link add vrrp type macvlan
    cmd << IP_CMD << " link add " << shellquote(vrrp_name) << " link " << shellquote(intf_alias) << " addrgenmode random type macvlan mode bridge && ";
    cmd << IP_CMD << " link set dev " << shellquote(vrrp_name) << " address " << vrrp_mac.to_string() << " && ";
    cmd << IP_CMD << " link set dev " << shellquote(vrrp_name) << " up";
    SWSS_LOG_DEBUG("Add vrrp virtual intf cmd: %s", cmd.str().c_str());

    try
    {
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to add vitrual intf[%s] on interface[%s], retry. Runtime error: %s", vrrp_name.c_str(), intf_alias.c_str(), e.what());
        return false;
    }

    SWSS_LOG_INFO("Add vitrual intf[%s] on interface[%s]", vrrp_name.c_str(), intf_alias.c_str());
    return true;
}

bool VrrpMgr::delVirtualInterface(const std::string &intf_alias, const std::string &vrrp_name)
{
    stringstream cmd;
    string res;

    // link del vrrp
    cmd << IP_CMD << " link del " << shellquote(vrrp_name);

    try
    {
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to del vitrual intf[%s] on interface[%s], retry. Runtime error: %s", vrrp_name.c_str(), intf_alias.c_str(), e.what());
        return false;
    }

    SWSS_LOG_INFO("Del vitrual intf[%s] on interface[%s]", vrrp_name.c_str(), intf_alias.c_str());
    return true;
}

bool swss::VrrpMgr::addVirtualInterfaceIp(const std::string &vrid, const IpPrefix &ip_addr)
{
    stringstream cmd;
    string res;

    bool ip_ipv4 = ip_addr.isV4();
    string vrrp_name = join(vrrp_name_delimiter, (ip_ipv4 ? VRRP_V4_PREFIX : VRRP_V6_PREFIX), vrid);
    string ipPrefixStr = ip_addr.to_string();
    // link add ip dev vrrp
    cmd << IP_CMD << (ip_ipv4 ? "" : " -6 ") << " address add " << shellquote(ipPrefixStr) << " dev " << shellquote(vrrp_name);

    try
    {
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to add ip[%s] on vitrual intf[%s], retry. Runtime error: %s", vrrp_name.c_str(), vrrp_name.c_str(), e.what());
        return false;
    }

    SWSS_LOG_INFO("Add ip[%s] on vitrual intf[%s]", ipPrefixStr.c_str(), vrrp_name.c_str());
    return true;
}

bool swss::VrrpMgr::delVirtualInterfaceIp(const std::string &vrid, const IpPrefix &ip_addr)
{
    stringstream cmd;
    string res;

    bool ip_ipv4 = ip_addr.isV4();
    string vrrp_name = join(vrrp_name_delimiter, (ip_ipv4 ? VRRP_V4_PREFIX : VRRP_V6_PREFIX), vrid);
    string ipPrefixStr = ip_addr.to_string();
    // link del ip dev vrrp
    cmd << IP_CMD << (ip_ipv4 ? "" : " -6 ") << " address del " << shellquote(ipPrefixStr) << " dev " << shellquote(vrrp_name);

    try
    {
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to del ip[%s] on vitrual intf[%s], retry. Runtime error: %s", vrrp_name.c_str(), vrrp_name.c_str(), e.what());
        return false;
    }

    SWSS_LOG_INFO("Del ip[%s] on vitrual intf[%s]", ipPrefixStr.c_str(), vrrp_name.c_str());
    return true;
}

bool VrrpMgr::isPortStateUp(const string &alias)
{
    string port_oper_status;
    if (m_appPortTable.hget(alias, "oper_status", port_oper_status) && port_oper_status == "up")
    {
        SWSS_LOG_DEBUG("Port %s is up", alias.c_str());
        return true;
    }

    return false;
}

bool VrrpMgr::isVrrpOnIntf(const std::string &alias)
{
    return find_if(m_vrrpList.begin(), m_vrrpList.end(), [alias](const auto &pair){ return pair.second.alias == alias; }) != m_vrrpList.end();
}

bool VrrpMgr::parseVrrpMac(const std::string &vrid, const bool is_ipv4, MacAddress &vrrp_mac)
{
    stringstream vmac;
    string hex_vrid;

    if (!all_of(vrid.begin(), vrid.end(), ::isdigit))
    {
        SWSS_LOG_WARN("vrid : %s, must be a number", vrid.c_str());
        return false;
    }

    int ivrid = stoi(vrid);
    if (ivrid >= 256 || ivrid <= 0)
    {
        SWSS_LOG_WARN("vrid : %s, must less than 256, greater than 0", vrid.c_str());
        return false;
    }

    // just need one bit to mac
    hex_vrid = binary_to_hex(&ivrid, sizeof(char));
    SWSS_LOG_INFO("vrid: %s, hex_vrid: %s", vrid.c_str(), hex_vrid.c_str());

    if (is_ipv4)
    {
        vmac << VRRP_V4_MAC_PREFIX << std::setw(2) << std::setfill('0') << hex_vrid;
    }
    else
    {
        vmac << VRRP_V6_MAC_PREFIX << std::setw(2) << std::setfill('0') << hex_vrid;
    }

    vrrp_mac = MacAddress(vmac.str());
    return true;
}

void VrrpMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
        const string &op = kfvOp(t);
        const vector<FieldValueTuple> &data = kfvFieldsValues(t);

        if (keys.size() != 2)
        {
            SWSS_LOG_WARN("vrrp table need intf and vrid, ignore it: %s", kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string intf_alias(keys[0]);
        string vrrp_id(keys[1]);
        if (!isPortStateUp(intf_alias))
        {
            SWSS_LOG_INFO("Port %s is not ready, pending...", intf_alias.c_str());
            it++;
            continue;
        }

        if (m_vrrpList.find(vrrp_id) != m_vrrpList.end() && m_vrrpList[vrrp_id].alias != intf_alias)
        {
            SWSS_LOG_WARN("vrid[%s] has been created on interface[%s], ignore it: %s",
                          vrrp_id.c_str(), m_vrrpList[vrrp_id].alias.c_str(), kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string vrid, vip_str;
        for (auto i : data)
        {
            if (fvField(i) == "vrid")
            {
                vrid = fvValue(i);
            }
            else if (fvField(i) == "vip")
            {
                vip_str = fvValue(i);
            }
        }

        if (op == SET_COMMAND)
        {
            if (vip_str.empty())
            {
                SWSS_LOG_INFO("Creat macvlan link on intf[%s] vrid[%s], need ip address, ignore", intf_alias.c_str(), vrrp_id.c_str());
            }

            set<IpPrefix> vips;
            vector<string> vip_list = tokenize(vip_str, list_item_delimiter);
            try
            {
                transform(vip_list.begin(), vip_list.end(), inserter(vips, vips.begin()), [](const string &vip){ return IpPrefix(vip); });
            }
            catch (const std::exception &e)
            {
                SWSS_LOG_ERROR("vip has invaild ip addr on vrrp table %s, ignore. Runtime error: %s", kfvKey(t).c_str(), e.what());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!setVrrpIntf(intf_alias, vrrp_id, vips))
            {
                SWSS_LOG_WARN("Set vrrp on intf[%s] vrid[%s] failed.", intf_alias.c_str(), vrrp_id.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!removeVrrpIntf(intf_alias, vrrp_id))
            {
                SWSS_LOG_WARN("Del vrrp on intf[%s] vrid[%s] failed.", intf_alias.c_str(), vrrp_id.c_str());
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}
