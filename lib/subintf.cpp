#include <cerrno>
#include <cstring>
#include <array>
#include <net/if.h>
#include <stdexcept>
#include "subintf.h"

using namespace swss;

subIntf::subIntf(const std::string &ifName)
{
    alias = ifName;
    size_t found = alias.find(VLAN_SUB_INTERFACE_SEPARATOR);
    if (found != std::string::npos)
    {
        parentIf = alias.substr(0, found);
        if (!parentIf.compare(0, strlen("Eth"), "Eth"))
        {
            std::string parentIfName;
            if (!parentIf.compare(0, strlen("Ethernet"), "Ethernet"))
            {
                parentIfShortName = "Eth" + parentIf.substr(strlen("Ethernet"));
                isCompressed = false;
                parentIfName = "Ethernet" + parentIf.substr(strlen("Ethernet"));
            }
            else
            {
                parentIfShortName = "Eth" + parentIf.substr(strlen("Eth"));
                isCompressed = true;
                parentIfName = "Ethernet" + parentIf.substr(strlen("Eth"));
            }
            parentIf = parentIfName;
            subIfIdx = alias.substr(found + 1);
        }
        else if (!parentIf.compare(0, strlen("Po"), "Po"))
        {
            if (!parentIf.compare(0, strlen("PortChannel"), "PortChannel"))
            {
                isCompressed = false;
                parentIfShortName = "Po" + parentIf.substr(strlen("PortChannel"));
                parentIf = "PortChannel" + parentIf.substr(strlen("PortChannel"));
            }
            else
            {
                isCompressed = true;
                parentIfShortName = "Po" + parentIf.substr(strlen("Po"));
                parentIf = "PortChannel" + parentIf.substr(strlen("Po"));
            }
            subIfIdx = alias.substr(found + 1);
        }
        else
        {
            subIfIdx = "";
        }
    }
}

bool subIntf::isValid() const
{
    if (subIfIdx == "")
    {
        return false;
    }
    else if (alias.length() >= IFNAMSIZ)
    {
        return false;
    }

    return true;
}

std::string subIntf::parentIntf() const
{
    return parentIf;
}

int subIntf::subIntfIdx() const
{
    uint16_t id;
    try
    {
        id = static_cast<uint16_t>(stoul(subIfIdx));
    }
    catch (const std::invalid_argument &e)
    {
        return -1;
    }
    catch (const std::out_of_range &e)
    {
        return -1;
    }
    return id;
}

std::string subIntf::longName() const
{
    if (isValid() == false)
        return "";

    return (parentIf + VLAN_SUB_INTERFACE_SEPARATOR + subIfIdx);
}

std::string subIntf::shortName() const
{
    if (isValid() == false)
        return "";

    return (parentIfShortName + VLAN_SUB_INTERFACE_SEPARATOR + subIfIdx);
}

bool subIntf::isShortName() const
{
    return ((isCompressed == true) ? true : false);
}
