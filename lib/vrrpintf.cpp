#include "schema.h"
#include "tokenize.h"
#include "converter.h"
#include "stringutility.h"
#include "logger.h"

#include "vrrpintf.h"

using namespace swss;

VrrpIntf::VrrpIntf()
{
    parent_name = "";
    vrrp_name = "";
    vrrp_vmac = "";
    vrid = 0;
    is_ipv4 = false;
}

VrrpIntf::VrrpIntf(const std::string &parentName, const int vrid, const bool isIpv4) :
    parent_name(parentName), vrid(vrid), is_ipv4(isIpv4)
{
    vrrp_vmac = "";
    vrrp_name = join(vrrp_name_delimiter, (is_ipv4 ? VRRP_V4_PREFIX : VRRP_V6_PREFIX), std::to_string(vrid));
}

VrrpIntf::VrrpIntf(const std::string &parentName, const std::string &vridStr, const bool isIpv4) :
    parent_name(parentName), is_ipv4(isIpv4)
{
    vrid = 0;
    vrrp_vmac = "";

    if (std::all_of(vridStr.begin(), vridStr.end(), ::isdigit))
    {
        vrid = to_int<int>(vridStr);
        vrrp_name = join(vrrp_name_delimiter, (is_ipv4 ? VRRP_V4_PREFIX : VRRP_V6_PREFIX), vridStr);
    }
}

VrrpIntf::VrrpIntf(const std::string &parentName, const std::string &vridStr, const bool isIpv4, const std::string &vmacStr) :
    parent_name(parentName), is_ipv4(isIpv4), vrrp_vmac(vmacStr)
{
    vrid = 0;

    if (std::all_of(vridStr.begin(), vridStr.end(), ::isdigit))
    {
        vrid = to_int<int>(vridStr);
        vrrp_name = join(vrrp_name_delimiter, (is_ipv4 ? VRRP_V4_PREFIX : VRRP_V6_PREFIX), vridStr);
    }
}

VrrpIntf::VrrpIntf(const std::string &parentName, const std::string &vrrpName) :
    parent_name(parentName), vrrp_name(vrrpName)
{
    vrid = 0;
    vrrp_vmac = "";

    auto name_list = tokenize(vrrpName, vrrp_name_delimiter);
    if (name_list.size() == 2)
    {
        std::string vrrp_name_prefix = name_list[0];
        std::string vridStr = name_list[1];
        if (std::all_of(vridStr.begin(), vridStr.end(), ::isdigit))
        {
            vrid = to_int<int>(vridStr);
        }
        if (vrrp_name_prefix == VRRP_V4_PREFIX)
        {
            is_ipv4 = true;
        }
        else if (vrrp_name_prefix == VRRP_V6_PREFIX)
        {
            is_ipv4 = false;
        }
        else
        {
            vrid = 0;
            is_ipv4 = false;
        }
    }
}

VrrpIntf::VrrpIntf(const std::string &parentName, const std::string &vridStr, const std::string &ipType) :
    parent_name(parentName)
{
    SWSS_LOG_INFO("parentName %s", parentName.c_str());
    vrid = 0;
    vrrp_vmac = "";

    if (std::all_of(vridStr.begin(), vridStr.end(), ::isdigit))
    {
        vrid = to_int<int>(vridStr);
    }
    SWSS_LOG_INFO("vridStr %s, vrid %d", vridStr.c_str(), vrid);
    if (ipType == IPV4_NAME)
    {
        is_ipv4 = true;
        SWSS_LOG_INFO("ipv4 %s", ipType.c_str());
    }
    else if (ipType == IPV6_NAME)
    {
        is_ipv4 = false;
        SWSS_LOG_INFO("ipv6 %s", ipType.c_str());
    }
    else
    {
        vrid = 0;
        is_ipv4 = false;
        SWSS_LOG_INFO("ip fail %s", ipType.c_str());
    }
    vrrp_name = join(vrrp_name_delimiter, (is_ipv4 ? VRRP_V4_PREFIX : VRRP_V6_PREFIX), vridStr);
    SWSS_LOG_INFO("vrrp_name %s", vrrp_name.c_str());
}

bool VrrpIntf::isValid() const
{
    if (vrid > 255 || vrid < 1)
    {
        return false;
    }

    if (parent_name.empty() || vrrp_name.empty())
    {
        return false;
    }

    return true;
}

std::string VrrpIntf::getParentName() const
{
    return isValid() ? parent_name : "";
}

std::string VrrpIntf::getVrrpName() const
{
    return isValid() ? vrrp_name : "";
}

MacAddress swss::VrrpIntf::getMacAddress()
{
    if (!isValid())
    {
        return MacAddress();
    }

    if (!vrrp_vmac.empty())
    {
        return MacAddress(vrrp_vmac);
    }

    std::stringstream vmac;
    std::string hex_vrid;

    // just need one bit to mac
    hex_vrid = binary_to_hex(&vrid, sizeof(char));

    if (is_ipv4)
    {
        vmac << VRRP_V4_MAC_PREFIX << std::setw(2) << std::setfill('0') << hex_vrid;
    }
    else
    {
        vmac << VRRP_V6_MAC_PREFIX << std::setw(2) << std::setfill('0') << hex_vrid;
    }
    vrrp_vmac = vmac.str();
    return MacAddress(vmac.str());
}

int VrrpIntf::getVrid() const
{
    return isValid() ? vrid : 0;
}

bool VrrpIntf::isIpv4() const
{
    return is_ipv4;
}
