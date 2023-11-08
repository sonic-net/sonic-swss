#include "pbutils.h"


using namespace std;
using namespace swss;
using namespace google::protobuf;

bool to_sai(const dash::types::IpVersion &pb_version, sai_ip_addr_family_t &sai_ip_family)
{
    switch (pb_version)
    {
    case dash::types::IP_VERSION_IPV4:
        sai_ip_family = SAI_IP_ADDR_FAMILY_IPV4;
        break;
    case dash::types::IP_VERSION_IPV6:
        sai_ip_family = SAI_IP_ADDR_FAMILY_IPV6;
        break;
    default:
        return false;
    }

    return true;
}

bool to_sai(const dash::types::IpAddress &pb_address, sai_ip_address_t &sai_address)
{
    SWSS_LOG_ENTER();

    if (pb_address.has_ipv4())
    {
        sai_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        sai_address.addr.ip4 = pb_address.ipv4();
    }
    else if (pb_address.has_ipv6())
    {
        sai_address.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        memcpy(sai_address.addr.ip6, pb_address.ipv6().c_str(), sizeof(sai_address.addr.ip6));
    }
    else
    {
        SWSS_LOG_WARN("The protobuf IP address %s is invalid", pb_address.DebugString().c_str());
        return false;
    }

    return true;
}

bool to_sai(const dash::types::IpPrefix &pb_prefix, sai_ip_prefix_t &sai_prefix)
{
    SWSS_LOG_ENTER();

    if (pb_prefix.ip().has_ipv4() && pb_prefix.mask().has_ipv4())
    {
        sai_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        sai_prefix.addr.ip4 = pb_prefix.ip().ipv4();
        sai_prefix.mask.ip4 = pb_prefix.mask().ipv4();
    }
    else if (pb_prefix.ip().has_ipv6() && pb_prefix.mask().has_ipv6())
    {
        sai_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        memcpy(sai_prefix.addr.ip6, pb_prefix.ip().ipv6().c_str(), sizeof(sai_prefix.addr.ip6));
        memcpy(sai_prefix.mask.ip6, pb_prefix.mask().ipv6().c_str(), sizeof(sai_prefix.mask.ip6));
    }
    else
    {
        SWSS_LOG_WARN("The protobuf IP prefix %s is invalid", pb_prefix.DebugString().c_str());
        return false;
    }

    return true;
}

bool to_sai(const RepeatedPtrField<dash::types::IpPrefix> &pb_prefixes, vector<sai_ip_prefix_t> &sai_prefixes)
{
    SWSS_LOG_ENTER();

    sai_prefixes.clear();
    sai_prefixes.reserve(pb_prefixes.size());

    for (auto &pb_prefix : pb_prefixes)
    {
        sai_ip_prefix_t sai_prefix;
        if (!to_sai(pb_prefix, sai_prefix))
        {
            sai_prefixes.clear();
            return false;
        }
        sai_prefixes.push_back(sai_prefix);
    }

    return true;
}

ip_addr_t to_swss(const dash::types::IpAddress &pb_address)
{
    SWSS_LOG_ENTER();

    ip_addr_t ip_address;
    if (pb_address.has_ipv4())
    {
        ip_address.family = AF_INET;
        ip_address.ip_addr.ipv4_addr = pb_address.ipv4();
    }
    else if (pb_address.has_ipv6())
    {
        ip_address.family = AF_INET6;
        memcpy(ip_address.ip_addr.ipv6_addr, pb_address.ipv6().c_str(), sizeof(ip_address.ip_addr.ipv6_addr));
    }
    else
    {
        SWSS_LOG_THROW("The protobuf IP address %s is invalid", pb_address.DebugString().c_str());
    }

    return ip_address;
}

std::string to_string(const dash::types::IpAddress &pb_address)
{
    SWSS_LOG_ENTER();

    return IpAddress(to_swss(pb_address)).to_string();
}
