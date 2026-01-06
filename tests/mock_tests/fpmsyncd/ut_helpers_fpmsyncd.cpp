#include "ut_helpers_fpmsyncd.h"
#include "ipaddress.h"
#include "ipprefix.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <cstdlib>
#include <mutex>

#define IPV6_MAX_BYTE 16
#define IPV6_MAX_BITLEN 128

/*
 * Mock rtnl_link_i2name() call
 * We simulate the existence of a VRF called Vrf10 with ifindex 10.
 * Calling rtnl_link_i2name(_, 10, _, _) will return the name of the VRF (i.e., "Vrf10" string)
 */
extern "C" {
char *__wrap_rtnl_link_i2name(struct nl_cache *cache, int ifindex, char *dst, size_t len)
{
    switch (ifindex)
    {
        case 10:
            strncpy(dst, "Vrf10", 6);
            return dst;
        case 30:
            strncpy(dst, "invalidVrf", 11);
            return dst;
        default:
            return NULL;
    }
}
}

namespace ut_fpmsyncd
{
    /* Add a unspecific attribute to netlink message */
    bool nl_attr_put(struct nlmsghdr *n, unsigned int maxlen, int type,
                     const void *data, unsigned int alen)
    {
        int len;
        struct rtattr *rta;

        len = (int)RTA_LENGTH(alen);

        if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
            return false;

        rta = reinterpret_cast<struct rtattr *>(static_cast<void *>(((char *)n) + NLMSG_ALIGN(n->nlmsg_len)));
        rta->rta_type = (uint16_t)type;
        rta->rta_len = (uint16_t)len;

        if (data)
            memcpy(RTA_DATA(rta), data, alen);
        else
            assert(alen == 0);

        n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);

        return true;
    }

    /* Add 8 bit integer attribute to netlink message */
    bool nl_attr_put8(struct nlmsghdr *n, unsigned int maxlen, int type,
                      uint16_t data)
    {
        return nl_attr_put(n, maxlen, type, &data, sizeof(uint8_t));
    }

    /* Add 16 bit integer attribute to netlink message */
    bool nl_attr_put16(struct nlmsghdr *n, unsigned int maxlen, int type,
                       uint16_t data)
    {
        return nl_attr_put(n, maxlen, type, &data, sizeof(uint16_t));
    }

    /* Add 32 bit integer attribute to netlink message */
    bool nl_attr_put32(struct nlmsghdr *n, unsigned int maxlen, int type,
                       uint32_t data)
    {
        return nl_attr_put(n, maxlen, type, &data, sizeof(uint32_t));
    }

    /* Start a new level of nested attributes */
    struct rtattr *nl_attr_nest(struct nlmsghdr *n, unsigned int maxlen, int type)
    {
        struct rtattr *nest = NLMSG_TAIL(n);

        if (!nl_attr_put(n, maxlen, type, NULL, 0))
            return NULL;

        nest->rta_type |= NLA_F_NESTED;
        return nest;
    }

    /* Finalize nesting of attributes */
    int nl_attr_nest_end(struct nlmsghdr *n, struct rtattr *nest)
    {
        nest->rta_len = (uint16_t)((uint8_t *)NLMSG_TAIL(n) - (uint8_t *)nest);
        return n->nlmsg_len;
    }

    /* Build a Netlink object containing an SRv6 VPN Route */
    struct nlmsg *create_srv6_vpn_route_nlmsg(
        uint16_t cmd,
        IpPrefix *dst,
        IpAddress *encap_src_addr,
        IpAddress *vpn_sid,
        uint16_t table_id,
        uint8_t prefixlen,
        uint8_t address_family,
        uint8_t rtm_type)
    {
        struct rtattr *nest;

        /* Allocate memory for the Netlink objct */
        struct nlmsg *nl_obj = (struct nlmsg *)calloc(1, sizeof(struct nlmsg));
        if (!nl_obj)
            throw std::runtime_error("netlink: nlmsg object allocation failed");

        nl_obj->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
        nl_obj->n.nlmsg_flags = NLM_F_CREATE | NLM_F_REQUEST;

        if (cmd == RTM_NEWROUTE &&
            dst && dst->isV4())
            nl_obj->n.nlmsg_flags |= NLM_F_REPLACE;

        nl_obj->n.nlmsg_type = cmd;

        nl_obj->n.nlmsg_pid = 100;

        if (address_family > 0)
            nl_obj->r.rtm_family = address_family;
        else
            nl_obj->r.rtm_family = dst ? dst->getIp().getIp().family : AF_INET6;
        if (prefixlen > 0)
            nl_obj->r.rtm_dst_len = prefixlen;
        else
            nl_obj->r.rtm_dst_len = dst ? (unsigned char)(dst->getMaskLength()) : IPV6_MAX_BITLEN;
        nl_obj->r.rtm_scope = RT_SCOPE_UNIVERSE;

        nl_obj->r.rtm_protocol = 11; // ZEBRA protocol

        if (rtm_type > 0)
            nl_obj->r.rtm_type = rtm_type;
        else
            nl_obj->r.rtm_type = RTN_UNICAST;

        /* Add the destination address */
        if (dst)
        {
            if (dst->isV4())
            {
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   RTA_DST, dst->getIp().getV4Addr()))
                    return NULL;
            }
            else
            {
                if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                 RTA_DST, dst->getIp().getV6Addr(), IPV6_MAX_BYTE))
                    return NULL;
            }
        }

        /* Add the table ID */
        if (table_id < 256)
            nl_obj->r.rtm_table = (unsigned char)table_id;
        else
        {
            nl_obj->r.rtm_table = RT_TABLE_UNSPEC;
            if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj), RTA_TABLE, table_id))
                return NULL;
        }

        /* Add encapsulation type NH_ENCAP_SRV6_ROUTE (SRv6 Route) */
        if (!nl_attr_put16(&nl_obj->n, sizeof(*nl_obj), RTA_ENCAP_TYPE,
                           NH_ENCAP_SRV6_ROUTE))
            return NULL;

        /* Add encapsulation information */
        nest = nl_attr_nest(&nl_obj->n, sizeof(*nl_obj), RTA_ENCAP);
        if (!nest)
            return NULL;

        /* Add source address for SRv6 encapsulation */
        if (!nl_attr_put(
                &nl_obj->n, sizeof(*nl_obj), ROUTE_ENCAP_SRV6_ENCAP_SRC_ADDR,
                encap_src_addr->getV6Addr(), 16))
            return NULL;

        /* Add the VPN SID */
        if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj), ROUTE_ENCAP_SRV6_VPN_SID,
                         vpn_sid->getV6Addr(), 16))
            return NULL;

        nl_attr_nest_end(&nl_obj->n, nest);

        return nl_obj;
    }

    /* Build a Netlink object containing an SRv6 My SID */
    struct nlmsg *create_srv6_mysid_nlmsg(
        uint16_t cmd,
        IpAddress *mysid,
        int8_t block_len,
        int8_t node_len,
        int8_t func_len,
        int8_t arg_len,
        uint32_t action,
        char *vrf,
        IpAddress *adj,
        uint16_t table_id,
        uint8_t prefixlen,
        uint8_t address_family)
    {
        struct rtattr *nest;

        /* Allocate memory for the Netlink object */
        struct nlmsg *nl_obj = (struct nlmsg *)malloc(sizeof(struct nlmsg));
        if (!nl_obj)
            throw std::runtime_error("netlink: nlmsg object allocation failed");

        memset(nl_obj, 0, sizeof(*nl_obj));

        nl_obj->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
        nl_obj->n.nlmsg_flags = NLM_F_CREATE | NLM_F_REQUEST;

        nl_obj->n.nlmsg_type = cmd;

        nl_obj->n.nlmsg_pid = 100;

        if (address_family > 0)
            nl_obj->r.rtm_family = address_family;
        else
            nl_obj->r.rtm_family = mysid ? mysid->getIp().family : AF_INET6;
        if (prefixlen > 0)
            nl_obj->r.rtm_dst_len = prefixlen;
        else
            nl_obj->r.rtm_dst_len = IPV6_MAX_BITLEN;
        nl_obj->r.rtm_scope = RT_SCOPE_UNIVERSE;

        nl_obj->r.rtm_protocol = 11; // Protocol ZEBRA

        if (cmd != RTM_DELROUTE)
            nl_obj->r.rtm_type = RTN_UNICAST;

        /* Add my SID address */
        if (mysid)
        {
            if (mysid->isV4())
            {
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   RTA_DST, mysid->getV4Addr()))
                    return NULL;
            }
            else
            {
                if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                 RTA_DST, mysid->getV6Addr(), 16))
                    return NULL;
            }
        }

        /* Add table ID */
        if (table_id < 256)
            nl_obj->r.rtm_table = (unsigned char)table_id;
        else
        {
            nl_obj->r.rtm_table = RT_TABLE_UNSPEC;
            if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj), RTA_TABLE, table_id))
                return NULL;
        }

        /* Add SID format information */
        if (block_len > 0 ||
            node_len > 0 ||
            func_len > 0 ||
            arg_len > 0)
        {
            nest =
                nl_attr_nest(&nl_obj->n, sizeof(*nl_obj),
                             SRV6_LOCALSID_FORMAT);

            /* Add block bits length */
            if (block_len >= 0)
                if (!nl_attr_put8(
                        &nl_obj->n, sizeof(*nl_obj),
                        SRV6_LOCALSID_FORMAT_BLOCK_LEN,
                        block_len))
                    return NULL;

            /* Add node bits length */
            if (node_len >= 0)
                if (!nl_attr_put8(
                        &nl_obj->n, sizeof(*nl_obj),
                        SRV6_LOCALSID_FORMAT_NODE_LEN,
                        node_len))
                    return NULL;

            /* Add function bits length */
            if (func_len >= 0)
                if (!nl_attr_put8(
                        &nl_obj->n, sizeof(*nl_obj),
                        SRV6_LOCALSID_FORMAT_FUNC_LEN,
                        func_len))
                    return NULL;

            /* Add argument bits length */
            if (arg_len >= 0)
                if (!nl_attr_put8(
                        &nl_obj->n, sizeof(*nl_obj),
                        SRV6_LOCALSID_FORMAT_ARG_LEN,
                        arg_len))
                    return NULL;

            nl_attr_nest_end(&nl_obj->n, nest);
        }

        /* If the Netlink message is a Delete Route message, we have done */
        if (cmd == RTM_DELROUTE)
        {
            NLMSG_ALIGN(nl_obj->n.nlmsg_len);
            return nl_obj;
        }

        /* Add my SID behavior (action and parameters) */
        switch (action)
        {
            case SRV6_LOCALSID_ACTION_END:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END))
                    return NULL;
                break;
            case SRV6_LOCALSID_ACTION_END_X:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END_X))
                    return NULL;
                if (adj)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_NH6,
                                     adj->getV6Addr(), 16))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_END_T:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END_T))
                    return NULL;
                if (vrf)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_VRFNAME,
                                     vrf, (uint32_t)strlen(vrf)))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_END_DX4:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END_DX4))
                    return NULL;
                if (adj)
                {
                    if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                       SRV6_LOCALSID_NH4,
                                       adj->getV4Addr()))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_END_DX6:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END_DX6))
                    return NULL;
                if (adj)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_NH6,
                                     adj->getV6Addr(), 16))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_END_DT4:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END_DT4))
                    return NULL;
                if (vrf)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_VRFNAME,
                                     vrf, (uint32_t)strlen(vrf)))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_END_DT6:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END_DT6))
                    return NULL;
                if (vrf)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_VRFNAME,
                                     vrf, (uint32_t)strlen(vrf)))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_END_DT46:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_END_DT46))
                    return NULL;
                if (vrf)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_VRFNAME,
                                     vrf, (uint32_t)strlen(vrf)))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_UN:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_UN))
                    return NULL;
                break;
            case SRV6_LOCALSID_ACTION_UA:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_UA))
                    return NULL;
                if (adj)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_NH6,
                                     adj->getV6Addr(), 16))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_UDX4:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_UDX4))
                    return NULL;
                if (adj)
                {
                    if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                       SRV6_LOCALSID_NH4,
                                       adj->getV4Addr()))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_UDX6:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_UDX6))
                    return NULL;
                if (adj)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_NH6,
                                     adj->getV6Addr(), 16))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_UDT4:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_UDT4))
                    return NULL;
                if (vrf)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_VRFNAME,
                                     vrf, (uint32_t)strlen(vrf)))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_UDT6:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_UDT6))
                    return NULL;
                if (vrf)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_VRFNAME,
                                     vrf, (uint32_t)strlen(vrf)))
                        return NULL;
                }
                break;
            case SRV6_LOCALSID_ACTION_UDT46:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   SRV6_LOCALSID_ACTION_UDT46))
                    return NULL;
                if (vrf)
                {
                    if (!nl_attr_put(&nl_obj->n, sizeof(*nl_obj),
                                     SRV6_LOCALSID_VRFNAME,
                                     vrf, (uint32_t)strlen(vrf)))
                        return NULL;
                }
                break;
            default:
                if (!nl_attr_put32(&nl_obj->n, sizeof(*nl_obj),
                                   SRV6_LOCALSID_ACTION,
                                   action))
                    return NULL;
        }

        return nl_obj;
    }

    std::once_flag srand_flag;

    void srand_init()
    {
        srand((unsigned)time(NULL));
    }

    NextHopGroupFull createSingleIPv4NextHopNHGFull(const char *gateway, const char *src_addr, std::uint32_t id)
    {
        std::call_once(srand_flag, srand_init);
        std::uint32_t id_in = id;
        std::uint32_t key_in = rand() % 10000 + 1;
        fib::nexthop_types_t type_in = fib::NEXTHOP_TYPE_IPV4_IFINDEX;
        fib::vrf_id_t vrf_id_in = rand() % 10000 + 1;
        fib::ifindex_t ifindex_t_in = rand() % 10000 + 1;
        std::string ifname_in = "Ethernet" + to_string(ifindex_t_in);
        fib::lsp_types_t label_type_in = fib::ZEBRA_LSP_NONE;
        fib::blackhole_type bh_type_in = fib::BLACKHOLE_UNSPEC;
        fib::g_addr gateway_in;
        inet_pton(AF_INET, gateway, &gateway_in.ipv4);
        fib::g_addr src_in;
        inet_pton(AF_INET, src_addr, &src_in.ipv4);
        fib::g_addr rmap_src_in;
        std::uint8_t weight_in = 1;
        std::uint8_t flags_in = 0;
        bool has_srv6 = false;
        bool has_seg6_segs = false;
        fib::nexthop_srv6 *nh_srv6_in = NULL;
        fib::seg6_seg_stack *nh_seg6_segs_in = NULL;
        std::vector<struct in6_addr> nh_segs_in;
        std::vector<uint32_t> dependents_in;
        std::vector<uint32_t> depends_in;
        NextHopGroupFull nhg = NextHopGroupFull(id_in, key_in, type_in, vrf_id_in, ifindex_t_in, ifname_in, depends_in, dependents_in,
                                                label_type_in, bh_type_in, gateway_in, src_in, rmap_src_in, weight_in,
                                                flags_in, has_srv6, has_seg6_segs, nh_srv6_in, nh_seg6_segs_in, nh_segs_in);
        return nhg;
    }

    NextHopGroupFull createSingleIPv6NextHopNHGFull(const char *gateway, const char *src_addr, std::uint32_t id)
    {
        std::call_once(srand_flag, srand_init);
        std::uint32_t id_in = (id == 0) ? (rand() % 10000 + 1) : id;
        std::uint32_t key_in = rand() % 10000 + 1;
        fib::nexthop_types_t type_in = fib::NEXTHOP_TYPE_IPV6_IFINDEX;
        fib::vrf_id_t vrf_id_in = rand() % 10000 + 1;
        fib::ifindex_t ifindex_t_in = rand() % 10000 + 1;
        std::string ifname_in = "Ethernet" + to_string(ifindex_t_in);
        fib::lsp_types_t label_type_in = fib::ZEBRA_LSP_NONE;
        fib::blackhole_type bh_type_in = fib::BLACKHOLE_UNSPEC;
        fib::g_addr gateway_in;
        inet_pton(AF_INET6, gateway, &gateway_in.ipv6);
        fib::g_addr src_in;
        inet_pton(AF_INET6, src_addr, &src_in.ipv6);
        fib::g_addr rmap_src_in;
        std::uint8_t weight_in = 1;
        std::uint8_t flags_in = 0;
        bool has_srv6 = false;
        bool has_seg6_segs = false;
        fib::nexthop_srv6 *nh_srv6_in = NULL;
        fib::seg6_seg_stack *nh_seg6_segs_in = NULL;
        std::vector<struct in6_addr> nh_segs_in;
        std::vector<uint32_t> dependents_in;
        std::vector<uint32_t> depends_in;
        NextHopGroupFull nhg = NextHopGroupFull(id_in, key_in, type_in, vrf_id_in, ifindex_t_in, ifname_in, depends_in, dependents_in,
                                                label_type_in, bh_type_in, gateway_in, src_in, rmap_src_in, weight_in,
                                                flags_in, has_srv6, has_seg6_segs, nh_srv6_in, nh_seg6_segs_in, nh_segs_in);
        return nhg;
    }

    NextHopGroupFull createMultiNextHopNHGFull(const std::map<uint32_t, NextHopGroupFull> nhGrpFullListIn, const std::map<uint32_t, uint32_t> weights, const std::map<uint32_t, uint32_t> numDirects, const vector<uint32_t> depends, const vector<uint32_t> dependents, std::uint32_t id)
    {
        std::call_once(srand_flag, srand_init);
        std::uint32_t id_in = id;
        std::uint32_t key_in = rand() % 10000 + 1;
        std::vector<nh_grp_full> nh_grp_full_list_in;
        std::vector<uint32_t> depends_in;
        std::vector<uint32_t> dependents_in;
        for (auto it = nhGrpFullListIn.begin(); it != nhGrpFullListIn.end(); it++)
        {
            nh_grp_full ngf;
            ngf.id = it->second.id;
            ngf.weight = (uint8_t)weights.at(ngf.id);
            ngf.num_direct = numDirects.at(ngf.id);
            nh_grp_full_list_in.push_back(ngf);
        }

        NextHopGroupFull nhg = NextHopGroupFull(id_in, key_in, nh_grp_full_list_in, depends, dependents);
        return nhg;
    }

    vector<string> splitResults(string result, string delimiter)
    {
        vector<string> ret;
        if (delimiter.size() > 1 || delimiter.size() <= 0)
        {
            return ret;
        }
        string subStr;
        for (size_t i = 0; i < result.size(); i++)
        {
            if (result[i] != delimiter[0])
            {
                subStr.push_back(result[i]);
            }
            else
            {
                ret.push_back(subStr);
                subStr = "";
            }
        }
        if (!subStr.empty())
        {
            ret.push_back(subStr);
        }
        return ret;
    }
}