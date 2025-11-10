#ifndef _TUNNELTERMHELPER_H
#define _TUNNELTERMHELPER_H

#include <memory>
#include <vector>
#include <set>
#include <string>
#include "dbconnector.h"
#include "portsorch.h"
#include "intfsorch.h"

#define VNET_TUNNEL_TERM_ACL_TABLE_TYPE "VNET_LOCAL_ENDPOINT_REDIRECT"
#define VNET_TUNNEL_TERM_ACL_TABLE "VNET_LOCAL_ENDPOINT"
#define VNET_TUNNEL_TERM_ACL_BASE_PRIORITY 9998
#define VNET_TUNNEL_TERM_ACL_RULE_NAME_SUFFIX "TUNN_TERM"

class TunnelTermHelper
{
public:
    TunnelTermHelper(DBConnector *cfgDb);

    virtual void initialize();

    std::vector<std::string> getBindPoints();
    std::set<std::string> findInternalPorts();
    std::string getNbrAlias(const swss::IpAddress& ip);
    std::string getRuleName(const std::string& vnet_name, const swss::IpPrefix& vip);

private:
    unique_ptr<swss::Table> port_table_;
    PortsOrch *ports_orch_;
    IntfsOrch *intfs_orch_;
};

#endif // _TUNNELTERMHELPER_H
