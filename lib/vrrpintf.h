#ifndef SWSS_COMMON_VRRP_INTF_H
#define SWSS_COMMON_VRRP_INTF_H


#include <string>

#include "ipprefix.h"
#include "macaddress.h"

#define VRRP_PREFIX "Vrrp"
#define VRRP_V4_PREFIX "Vrrp4"
#define VRRP_V6_PREFIX "Vrrp6"

#define VRRP_V4_MAC_PREFIX "00:00:5e:00:01:"
#define VRRP_V6_MAC_PREFIX "00:00:5e:00:02:"

const char vrrp_name_delimiter = '-';

namespace swss {

class VrrpIntf
{
    public:
        VrrpIntf();
        VrrpIntf(const std::string &parentName, const int vrid, const bool isIpv4);
        VrrpIntf(const std::string &parentName, const std::string &vridStr, const bool isIpv4);
        VrrpIntf(const std::string &parentName, const std::string &vridStr, const bool isIpv4, const std::string &vmacStr);
        VrrpIntf(const std::string &parentName, const std::string &vridStr, const std::string &ipType);
        VrrpIntf(const std::string &parentName, const std::string &vrrpName);

        bool isValid() const;
        std::string getParentName() const;
        std::string getVrrpName() const;
        MacAddress getMacAddress();
        int getVrid() const;
        bool isIpv4() const;

    private:
        std::string parent_name;
        std::string vrrp_name;
        std::string vrrp_vmac;
        int vrid;
        bool is_ipv4;
};

}

#endif /* SWSS_COMMON_VRRP_INTF_H */
