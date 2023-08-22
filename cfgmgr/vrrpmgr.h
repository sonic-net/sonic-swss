#pragma once

#include "orch.h"
#include "producerstatetable.h"
#include "vrrpintf.h"

#include <map>
#include <string>

namespace swss {

struct VrrpIntfConf
{
    std::string alias{};
    std::set<IpPrefix> vips;
    std::string v4mac{};
    std::string v6mac{};
};

class VrrpMgr : public Orch
{
public:
    VrrpMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    Table m_cfgVrrpTable;
    Table m_appPortTable;
    ProducerStateTable m_appVrrpTableProducer;

    // <vid, VrrpIntfConf>
    std::map<std::string, VrrpIntfConf> m_vrrpList;

    void doTask(Consumer &consumer);
    
    bool setIntfArpAccept(const std::string &intf_alias, const bool back_default = false);

    bool setVrrpIntf(const std::string &intf_alias, const std::string &vrid, const std::set<IpPrefix> &vip_list);
    bool removeVrrpIntf(const std::string &intf_alias, const std::string &vrid);

    bool addVirtualInterface(const std::string &intf_alias, const std::string &vrrp_name, const MacAddress &vrrp_mac);
    bool delVirtualInterface(const std::string &intf_alias, const std::string &vrrp_name);
    bool addVirtualInterfaceIp(const std::string &vrid, const IpPrefix &ip_addr);
    bool delVirtualInterfaceIp(const std::string &vrid, const IpPrefix &ip_addr);

    bool isPortStateUp(const std::string &alias);
    bool isVrrpOnIntf(const std::string &alias);

    bool parseVrrpMac(const std::string &vrid, const bool is_ipv4, MacAddress& vrrp_mac);
};

}
