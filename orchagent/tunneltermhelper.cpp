#include "tunneltermhelper.h"
#include "swss/ipaddress.h"
#include "swss/ipprefix.h"
#include "directory.h"

extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern IntfsOrch *gIntfsOrch;

using namespace std;
using namespace swss;

TunnelTermHelper::TunnelTermHelper(DBConnector *cfgDb)
    : ports_orch_(nullptr), intfs_orch_(nullptr)
{
    SWSS_LOG_ENTER();

    port_table_ = make_unique<Table>(cfgDb, CFG_PORT_TABLE_NAME);
}

void TunnelTermHelper::initialize()
{
    SWSS_LOG_ENTER();

    ports_orch_ = gDirectory.get<PortsOrch*>();
    intfs_orch_ = gDirectory.get<IntfsOrch*>();

    assert(ports_orch_);
    assert(intfs_orch_);
}

std::vector<std::string> TunnelTermHelper::getBindPoints()
{
    std::vector<std::string> bind_points;
    std::set<std::string> internal_ports = findInternalPorts();
    auto all_ports = ports_orch_->getAllPorts();

    std::set<std::string> legitSet;

    /* Add physical and LAG prots */
    for (auto& it : all_ports)
    {
        Port& port = it.second;
        if (port.m_type == Port::PHY || port.m_type == Port::LAG)
        {
            legitSet.insert(it.first);
        }
    }

    /* Remove LAG members */
    for (auto& it : all_ports)
    {
        Port& port = it.second;
        if (port.m_type == Port::LAG)
        {
            for (auto member : port.m_members)
            {
                legitSet.erase(member);
            }
        }
    }

    for (auto& port : legitSet)
    {
        if (internal_ports.find(port) == internal_ports.end())
        {
            bind_points.push_back(port);
        }
    }

    return bind_points;
}

std::set<std::string> TunnelTermHelper::findInternalPorts()
{
    std::set<std::string> internal_ports;
    std::vector<std::string> all_ports;
    port_table_->getKeys(all_ports);

    for (auto& port : all_ports)
    {
        std::string role;
        if (port_table_->hget(port, PORT_ROLE, role))
        {
            if (role == PORT_ROLE_DPC)
            {
                internal_ports.insert(port);
            }
        }
    }

    return internal_ports;
}

std::string TunnelTermHelper::getNbrAlias(const swss::IpAddress& ip)
{
    return intfs_orch_->getRouterIntfsAlias(ip);
}

std::string TunnelTermHelper::getRuleName(const string& vnet_name, const swss::IpPrefix& vip)
{
    return std::string(VNET_TUNNEL_TERM_ACL_TABLE) + ":" + vnet_name + "_" + vip.to_string() + "_" + VNET_TUNNEL_TERM_ACL_RULE_NAME_SUFFIX;
}