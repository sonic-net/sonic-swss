// Header file defining base types and simple helper functions for network in SAI
// Should keep the dependency as minimal as possible
//
#pragma once

#include <assert.h>
#include <memory.h>
#include <stdexcept>
#include <net/if.h>
extern "C" {
#include "sai.h"
}
#include "ipaddress.h"
#include "ipprefix.h"

namespace swss {

inline static sai_ip_address_t& copy(sai_ip_address_t& dst, const IpAddress& src)
{
    auto sip = src.getIp();
    switch(sip.family)
    {
        case AF_INET:
            memset((void*)&dst.addr, 0, sizeof(dst.addr));
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            dst.addr.ip4 = sip.ip_addr.ipv4_addr;
            break;
        case AF_INET6:
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
            memcpy(dst.addr.ip6, sip.ip_addr.ipv6_addr, 16);
            break;
        default:
            throw std::logic_error("Invalid family");
    }
    return dst;
}

inline static sai_ip_prefix_t& copy(sai_ip_prefix_t& dst, const IpPrefix& src)
{
    auto ia = src.getIp().getIp();
    auto ma = src.getMask().getIp();
    switch(ia.family)
    {
        case AF_INET:
            memset((void*)&dst, 0, sizeof(dst));
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            dst.addr.ip4 = ia.ip_addr.ipv4_addr;
            dst.mask.ip4 = ma.ip_addr.ipv4_addr;
            break;
        case AF_INET6:
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
            memcpy(dst.addr.ip6, ia.ip_addr.ipv6_addr, 16);
            memcpy(dst.mask.ip6, ma.ip_addr.ipv6_addr, 16);
            break;
        default:
            throw std::logic_error("Invalid family");
    }
    return dst;
}

inline static sai_ip_prefix_t& copy(sai_ip_prefix_t& dst, const IpAddress& src)
{
    auto sip = src.getIp();
    switch(sip.family)
    {
        case AF_INET:
            memset((void*)&dst, 0, sizeof(dst));
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            dst.addr.ip4 = sip.ip_addr.ipv4_addr;
            dst.mask.ip4 = 0xFFFFFFFF;
            break;
        case AF_INET6:
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
            memcpy(dst.addr.ip6, sip.ip_addr.ipv6_addr, 16);
            memset(dst.mask.ip6, 0xFF, 16);
            break;
        default:
            throw std::logic_error("Invalid family");
    }
    return dst;
}

static int getPrefixLenFromAddrMask(const uint8_t *addr, int len)
{
    int i = 0;
    uint8_t non_zero = 0xFF;
    for (i = len - 1; i >=0; i--)
    {
        if (addr[i] != 0)
        {
            non_zero = addr[i];
            break;
        }
    }

    if (non_zero == 0xFF)
    {
        return (i + 1) * 8;
    }
    else
    {
        int j = 2;
        while(((non_zero >> j) & 0x1) == 0)
        {
            ++j;
        }
        return (i + 1) * 8 - (j + 1);
    }
    
}

inline static IpPrefix getIpPrefixFromSaiPrefix(const sai_ip_prefix_t& src)
{
    ip_addr_t ip;
    switch(src.addr_family)
    {
        case SAI_IP_ADDR_FAMILY_IPV4:
            ip.family = AF_INET;
            ip.ip_addr.ipv4_addr = src.addr.ip4;
            return IpPrefix(ip, getPrefixLenFromAddrMask(reinterpret_cast<const uint8_t*>(&src.mask.ip4), 4));
        case SAI_IP_ADDR_FAMILY_IPV6:
            ip.family = AF_INET6;
            memcpy(ip.ip_addr.ipv6_addr, src.addr.ip6, 16);
            return IpPrefix(ip, getPrefixLenFromAddrMask(src.mask.ip6, 16));
        default:
            throw std::logic_error("Invalid family");
    }
}

inline static sai_ip_prefix_t& subnet(sai_ip_prefix_t& dst, const sai_ip_prefix_t& src)
{
    dst.addr_family = src.addr_family;
    switch(src.addr_family)
    {
        case SAI_IP_ADDR_FAMILY_IPV4:
            dst.addr.ip4 = src.addr.ip4 & src.mask.ip4;
            dst.mask.ip4 = src.mask.ip4;
            break;
        case SAI_IP_ADDR_FAMILY_IPV6:
            for (size_t i = 0; i < 16; i++)
            {
                dst.addr.ip6[i] = src.addr.ip6[i] & src.mask.ip6[i];
                dst.mask.ip6[i] = src.mask.ip6[i];
            }
            break;
        default:
            throw std::logic_error("Invalid family");
    }
    return dst;
}

}
