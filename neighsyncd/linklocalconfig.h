#ifndef SWSS_LINKLOCALCONFIG_H
#define SWSS_LINKLOCALCONFIG_H

#include <string>

#include "schema.h"

namespace swss
{

inline std::string getLinkLocalInterfaceTable(const std::string &interface)
{
    // Sub-ports own their mode independently of their parent. Preserve the
    // shortened Linux Eth/Po names as well as Ethernet/PortChannel names.
    if (interface.find('.') != std::string::npos &&
        (interface.compare(0, 3, "Eth") == 0 || interface.compare(0, 2, "Po") == 0))
    {
        return CFG_VLAN_SUB_INTF_TABLE_NAME;
    }
    if (interface.compare(0, 4, "Vlan") == 0)
    {
        return CFG_VLAN_INTF_TABLE_NAME;
    }
    if (interface.compare(0, 11, "PortChannel") == 0)
    {
        return CFG_LAG_INTF_TABLE_NAME;
    }
    if (interface.compare(0, 8, "Ethernet") == 0)
    {
        return CFG_INTF_TABLE_NAME;
    }
    return {};
}

}

#endif
