#ifndef __VLANCFGMGR__
#define __VLANCFGMGR__

#include "dbconnector.h"
#include "table.h"
#include <set>
#include <map>
#include <string>

namespace swss {

/*
 * Record the vlan tagging setting for each VLAN member
 */
typedef std::map<std::string, std::string> Vlan_Master_Mode; // ex. <"Vlan1003", "tagged">
typedef std::map<std::string, Vlan_Master_Mode> Vlan_Members; //ex. <"Ethernet8", <"Vlan1003", "tagged">>

class VlanCfgMgr
{
public:
    enum Operation {
        ADD,
        DELETE,
        SHOW,
    } ;
    enum { MAX_ADDR_SIZE = 64 };

    VlanCfgMgr(DBConnector *db);
    int vlan_modify(Operation cmd, int argc, char **argv);
    int vlan_show(int argc, char **argv);

private:
    Table m_cfgVlanTable, m_cfgVlanMemberTable;

    std::map<std::string, std::set<std::string>> m_vlanMap;
    Vlan_Members m_vlanMemberMap;

    void vlan_redis_show(std::string &redis_cmd_keys, int vid, std::string filter_dev);
};

}

#endif
