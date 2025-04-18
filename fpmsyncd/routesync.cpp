#include <netlink/route/link.h>
#include <netlink/route/route.h>
#include <netlink/route/nexthop.h>
#include "logger.h"
#include "select.h"
#include "netmsg.h"
#include "ipprefix.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "zmqproducerstatetable.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"
#include "macaddress.h"
#include "converter.h"
#include <string.h>
#include <arpa/inet.h>
#include <linux/nexthop.h>

using namespace std;
using namespace swss;

#define VXLAN_IF_NAME_PREFIX    "Brvxlan"
#define VNET_PREFIX             "Vnet"
#define VRF_PREFIX              "Vrf"
#define MGMT_VRF_PREFIX         "mgmt"

#define NHG_DELIMITER ','
#define MY_SID_KEY_DELIMITER ':'

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

#ifndef NDA_RTA
#define NDA_RTA(r)                                                             \
    ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#endif

#ifndef NHA__RTA
#define NHA_RTA(r)                                                             \
    ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct nhmsg))))
#endif

#define VXLAN_VNI             0
#define VXLAN_RMAC            1
#define NH_ENCAP_VXLAN      100

#define NH_ENCAP_SRV6_ROUTE         101

#define IPV4_MAX_BYTE       4
#define IPV6_MAX_BYTE      16
#define IPV4_MAX_BITLEN    32
#define IPV6_MAX_BITLEN    128

#define ETHER_ADDR_STRLEN (3*ETH_ALEN)

#define DEFAULT_SRV6_MY_SID_BLOCK_LEN "32"
#define DEFAULT_SRV6_MY_SID_NODE_LEN "16"
#define DEFAULT_SRV6_MY_SID_FUNC_LEN "16"
#define DEFAULT_SRV6_MY_SID_ARG_LEN "0"

enum srv6_localsid_action {
	SRV6_LOCALSID_ACTION_UNSPEC				= 0,
	SRV6_LOCALSID_ACTION_END				= 1,
	SRV6_LOCALSID_ACTION_END_X				= 2,
	SRV6_LOCALSID_ACTION_END_T				= 3,
	SRV6_LOCALSID_ACTION_END_DX2			= 4,
	SRV6_LOCALSID_ACTION_END_DX6			= 5,
	SRV6_LOCALSID_ACTION_END_DX4			= 6,
	SRV6_LOCALSID_ACTION_END_DT6			= 7,
	SRV6_LOCALSID_ACTION_END_DT4			= 8,
	SRV6_LOCALSID_ACTION_END_DT46			= 9,
	SRV6_LOCALSID_ACTION_B6_ENCAPS			= 10,
	SRV6_LOCALSID_ACTION_B6_ENCAPS_RED		= 11,
	SRV6_LOCALSID_ACTION_B6_INSERT			= 12,
	SRV6_LOCALSID_ACTION_B6_INSERT_RED		= 13,
	SRV6_LOCALSID_ACTION_UN					= 14,
	SRV6_LOCALSID_ACTION_UA					= 15,
	SRV6_LOCALSID_ACTION_UDX2				= 16,
	SRV6_LOCALSID_ACTION_UDX6				= 17,
	SRV6_LOCALSID_ACTION_UDX4				= 18,
	SRV6_LOCALSID_ACTION_UDT6				= 19,
	SRV6_LOCALSID_ACTION_UDT4				= 20,
	SRV6_LOCALSID_ACTION_UDT46				= 21,
};

enum {
	SRV6_LOCALSID_UNSPEC			= 0,
	SRV6_LOCALSID_SID_VALUE			= 1,
	SRV6_LOCALSID_FORMAT			= 2,
	SRV6_LOCALSID_ACTION			= 3,
	SRV6_LOCALSID_VRFNAME			= 4,
	SRV6_LOCALSID_NH6				= 5,
	SRV6_LOCALSID_NH4				= 6,
	SRV6_LOCALSID_IIF				= 7,
	SRV6_LOCALSID_OIF				= 8,
	SRV6_LOCALSID_BPF				= 9,
	SRV6_LOCALSID_SIDLIST			= 10,
	SRV6_LOCALSID_ENCAP_SRC_ADDR	= 11,
};

enum {
	SRV6_LOCALSID_FORMAT_UNSPEC			= 0,
	SRV6_LOCALSID_FORMAT_BLOCK_LEN		= 1,
	SRV6_LOCALSID_FORMAT_NODE_LEN		= 2,
	SRV6_LOCALSID_FORMAT_FUNC_LEN		= 3,
	SRV6_LOCALSID_FORMAT_ARG_LEN		= 4,
};

enum {
    ROUTE_ENCAP_SRV6_UNSPEC            = 0,
    ROUTE_ENCAP_SRV6_VPN_SID           = 1,
    ROUTE_ENCAP_SRV6_ENCAP_SRC_ADDR    = 2,
};

#define MAX_MULTIPATH_NUM 514

/* Returns name of the protocol passed number represents */
static string getProtocolString(int proto)
{
    static constexpr size_t protocolNameBufferSize = 128;
    char buffer[protocolNameBufferSize] = {};

    if (!rtnl_route_proto2str(proto, buffer, sizeof(buffer)))
    {
        return std::to_string(proto);
    }

    return buffer;
}

/* Helper to create unique pointer with custom destructor */
template<typename T, typename F>
static decltype(auto) makeUniqueWithDestructor(T* ptr, F func)
{
    return std::unique_ptr<T, F>(ptr, func);
}

template<typename T>
static decltype(auto) makeNlAddr(const T& ip)
{
    nl_addr* addr;
    nl_addr_parse(ip.to_string().c_str(), AF_UNSPEC, &addr);
    return makeUniqueWithDestructor(addr, nl_addr_put);
}


RouteSync::RouteSync(RedisPipeline *pipeline, std::shared_ptr<ZmqClient> zmqClient) :
    m_nexthop_groupTable(pipeline, APP_NEXTHOP_GROUP_TABLE_NAME, true),
    m_vnet_routeTable(pipeline, APP_VNET_RT_TABLE_NAME, true),
    m_vnet_tunnelTable(pipeline, APP_VNET_RT_TUNNEL_TABLE_NAME, true),
    m_srv6MySidTable(pipeline, APP_SRV6_MY_SID_TABLE_NAME, true),
    m_srv6SidListTable(pipeline, APP_SRV6_SID_LIST_TABLE_NAME, true),
    m_nl_sock(NULL), m_link_cache(NULL)
{
    if (zmqClient != nullptr) {
        m_routeTable = unique_ptr<ProducerStateTable>(new ZmqProducerStateTable(pipeline, APP_ROUTE_TABLE_NAME, *zmqClient, true));
        m_label_routeTable = unique_ptr<ProducerStateTable>(new ZmqProducerStateTable(pipeline, APP_LABEL_ROUTE_TABLE_NAME, *zmqClient, true));
    }
    else {
        m_routeTable = make_unique<ProducerStateTable>(pipeline, APP_ROUTE_TABLE_NAME, true);
        m_label_routeTable = make_unique<ProducerStateTable>(pipeline, APP_LABEL_ROUTE_TABLE_NAME, true);
    }
    
    m_warmStartHelper = make_unique<WarmStartHelper>(pipeline, m_routeTable.get(), APP_ROUTE_TABLE_NAME, "bgp", "bgp");
    m_nl_sock = nl_socket_alloc();
    nl_connect(m_nl_sock, NETLINK_ROUTE);
    rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);
}

char *RouteSync::prefixMac2Str(char *mac, char *buf, int size)
{
    char *ptr = buf;

    if (!mac)
    {
        return NULL;
    }
    if (!buf)
    {
        return NULL;
    }

    snprintf(ptr, (ETHER_ADDR_STRLEN), "%02x:%02x:%02x:%02x:%02x:%02x",
            (uint8_t)mac[0], (uint8_t)mac[1],
            (uint8_t)mac[2], (uint8_t)mac[3],
            (uint8_t)mac[4], (uint8_t)mac[5]);
    return ptr;
}

/**
 * parseRtAttrNested() - Parses a nested route attribute
 * @tb:         Pointer to array for storing rtattr in.
 * @max:        Max number to store.
 * @rta:        Pointer to rtattr to look for nested items in.
 */
void RouteSync::parseRtAttrNested(struct rtattr **tb, int max,
                                                            struct rtattr *rta)
{
	netlink_parse_rtattr(tb, max, (struct rtattr *)RTA_DATA(rta), (int)RTA_PAYLOAD(rta));
}

/**
 * @parseEncap() - Parses encapsulated attributes
 * @tb:         Pointer to rtattr to look for nested items in.
 * @labels:     Pointer to store vni in.
 *
 * Return:      void.
 */
void RouteSync::parseEncap(struct rtattr *tb, uint32_t &encap_value, string &rmac)
{
    struct rtattr *tb_encap[3] = {0};
    char mac_buf[MAX_ADDR_SIZE+1];
    char mac_val[MAX_ADDR_SIZE+1];

    parseRtAttrNested(tb_encap, 3, tb);
    encap_value = *(uint32_t *)RTA_DATA(tb_encap[VXLAN_VNI]);
    memcpy(&mac_buf, RTA_DATA(tb_encap[VXLAN_RMAC]), MAX_ADDR_SIZE);

    SWSS_LOG_INFO("Rx MAC %s VNI %d",
        prefixMac2Str(mac_buf, mac_val, ETHER_ADDR_STRLEN), encap_value);
    rmac = mac_val;

    return;
}

/**
 * @parseEncapSrv6SteerRoute() - Parses encapsulated SRv6 attributes
 * @tb:         Pointer to rtattr to look for nested items in.
 * @vpn_sid:    (output) VPN SID.
 * @src_addr:   (output) source address for SRv6 encapsulation
 *
 * Return:      void.
 */
void RouteSync::parseEncapSrv6SteerRoute(struct rtattr *tb, string &vpn_sid,
                               string &src_addr)
{
    struct rtattr *tb_encap[256] = {};
    char vpn_sid_buf[MAX_ADDR_SIZE + 1] = {0};
    char src_addr_buf[MAX_ADDR_SIZE + 1] = {0};

    parseRtAttrNested(tb_encap, 256, tb);

    if (tb_encap[ROUTE_ENCAP_SRV6_VPN_SID])
    {
        vpn_sid += inet_ntop(AF_INET6, RTA_DATA(tb_encap[ROUTE_ENCAP_SRV6_VPN_SID]),
                             vpn_sid_buf, MAX_ADDR_SIZE);
    }

    if (tb_encap[ROUTE_ENCAP_SRV6_ENCAP_SRC_ADDR])
    {
        src_addr +=
            inet_ntop(AF_INET6, RTA_DATA(tb_encap[ROUTE_ENCAP_SRV6_ENCAP_SRC_ADDR]),
                      src_addr_buf, MAX_ADDR_SIZE);
    }

    SWSS_LOG_INFO("Rx vpn_sid:%s src_addr:%s ", vpn_sid.c_str(),
                  src_addr.c_str());

    return;
}

const char *RouteSync::mySidAction2Str(uint32_t action)
{
    switch (action)
    {
        case SRV6_LOCALSID_ACTION_UNSPEC:
            return "unspec";
        case SRV6_LOCALSID_ACTION_END:
            return "end";
        case SRV6_LOCALSID_ACTION_END_X:
            return "end.x";
        case SRV6_LOCALSID_ACTION_END_T:
            return "end.t";
        case SRV6_LOCALSID_ACTION_END_DX6:
            return "end.dx6";
        case SRV6_LOCALSID_ACTION_END_DX4:
            return "end.dx4";
        case SRV6_LOCALSID_ACTION_END_DT6:
            return "end.dt6";
        case SRV6_LOCALSID_ACTION_END_DT4:
            return "end.dt4";
        case SRV6_LOCALSID_ACTION_END_DT46:
            return "end.dt46";
        case SRV6_LOCALSID_ACTION_UN:
            return "un";
        case SRV6_LOCALSID_ACTION_UA:
            return "ua";
        case SRV6_LOCALSID_ACTION_UDX6:
            return "udx6";
        case SRV6_LOCALSID_ACTION_UDX4:
            return "udx4";
        case SRV6_LOCALSID_ACTION_UDT6:
            return "udt6";
        case SRV6_LOCALSID_ACTION_UDT4:
            return "udt4";
        case SRV6_LOCALSID_ACTION_UDT46:
            return "udt46";
        default:
            return "unknown";
    }
}

/**
 * @parseSrv6MySidFormat() - Parses srv6 MySid format
 * @tb:         Pointer to rtattr to look for nested items in.
 * @block_len:  (output) locator block length
 * @node_len:   (output) locator node length
 * @func_len:   (output) function length
 * @arg_len:    (output) argument length
 *
 * Return:      true on success, false otherwise.
 */
bool RouteSync::parseSrv6MySidFormat(struct rtattr *tb,
                                        string &block_len,
                                        string &node_len, string &func_len,
                                        string &arg_len)
{
    struct rtattr *tb_my_sid_format[256] = {};
    uint8_t block_len_buf, node_len_buf, func_len_buf, arg_len_buf;

    parseRtAttrNested(tb_my_sid_format, 4, tb);

    if (tb_my_sid_format[SRV6_LOCALSID_FORMAT_BLOCK_LEN])
    {
        block_len_buf = *(uint8_t *)RTA_DATA(
            tb_my_sid_format[SRV6_LOCALSID_FORMAT_BLOCK_LEN]);
        block_len += to_string(block_len_buf);
    }
    else
    {
        block_len += DEFAULT_SRV6_MY_SID_BLOCK_LEN;
    }

    if (tb_my_sid_format[SRV6_LOCALSID_FORMAT_NODE_LEN])
    {
        node_len_buf = *(uint8_t *)RTA_DATA(
            tb_my_sid_format[SRV6_LOCALSID_FORMAT_NODE_LEN]);
        node_len += to_string(node_len_buf);
    }
    else
    {
        node_len += DEFAULT_SRV6_MY_SID_NODE_LEN;
    }

    if (tb_my_sid_format[SRV6_LOCALSID_FORMAT_FUNC_LEN])
    {
        func_len_buf = *(uint8_t *)RTA_DATA(
            tb_my_sid_format[SRV6_LOCALSID_FORMAT_FUNC_LEN]);
        func_len += to_string(func_len_buf);
    }
    else
    {
        func_len += DEFAULT_SRV6_MY_SID_FUNC_LEN;
    }

    if (tb_my_sid_format[SRV6_LOCALSID_FORMAT_ARG_LEN])
    {
        arg_len_buf = *(uint8_t *)RTA_DATA(
            tb_my_sid_format[SRV6_LOCALSID_FORMAT_ARG_LEN]);
        arg_len += to_string(arg_len_buf);
    }
    else
    {
        /* arg_len is optional, by default arg_len is 0 */
        arg_len += DEFAULT_SRV6_MY_SID_ARG_LEN;
    }

    SWSS_LOG_INFO("Rx Srv6 MySid block_len:%s node_len:%s func_len:%s arg_len:%s",
                  block_len.c_str(), node_len.c_str(), func_len.c_str(),
                  arg_len.c_str());

    return true;
}

/**
 * @parseSrv6MySid() - Parses sRv6 MySid attributes
 * @tb:         Pointer to rtattr to look for nested items in.
 * @block_len:  (output) locator block length
 * @node_len:   (output) locator node length
 * @func_len:   (output) function length
 * @arg_len:    (output) argument length
 * @action:     (output) behavior defined for the MySID.
 * @vrf:        (output) VRF name.
 * @adj:        (output) adjacency.
 *
 * Return:      true on success, false otherwise.
 */
bool RouteSync::parseSrv6MySid(struct rtattr *tb[], string &block_len,
                                  string &node_len, string &func_len,
                                  string &arg_len, string &action,
                                  string &vrf, string &adj)
{
    uint32_t action_buf = SRV6_LOCALSID_ACTION_UNSPEC;
    char vrf_buf[IFNAMSIZ + 1] = {0};
    char adj_buf[MAX_ADDR_SIZE + 1] = {0};

    if (tb[SRV6_LOCALSID_FORMAT])
    {
        if (!parseSrv6MySidFormat(tb[SRV6_LOCALSID_FORMAT], block_len,
                                node_len, func_len, arg_len))
        {
            SWSS_LOG_ERROR("Invalid Srv6 MySid format: block_len=%s, "
                "node_len=%s, func_len=%s, arg_len=%s",
                block_len.c_str(), node_len.c_str(), func_len.c_str(), arg_len.c_str());

            return false;
        }
    }

    if (tb[SRV6_LOCALSID_ACTION])
    {
        action_buf = *(uint32_t *)RTA_DATA(tb[SRV6_LOCALSID_ACTION]);
    }

    if (tb[SRV6_LOCALSID_NH6])
    {
        struct in6_addr *nh6 =
            (struct in6_addr *)RTA_DATA(tb[SRV6_LOCALSID_NH6]);

        inet_ntop(AF_INET6, nh6, adj_buf, MAX_ADDR_SIZE);
    }

    if (tb[SRV6_LOCALSID_NH4])
    {
        struct in_addr *nh4 =
            (struct in_addr *)RTA_DATA(tb[SRV6_LOCALSID_NH4]);

        inet_ntop(AF_INET, nh4, adj_buf, MAX_ADDR_SIZE);
    }

    if (tb[SRV6_LOCALSID_VRFNAME])
    {
        memcpy(vrf_buf, (char *)RTA_DATA(tb[SRV6_LOCALSID_VRFNAME]),
               strlen((char *)RTA_DATA(tb[SRV6_LOCALSID_VRFNAME])));
    }

    action = mySidAction2Str(action_buf);
    vrf = vrf_buf;
    adj = adj_buf;

    if (action == "unknown")
    {
        SWSS_LOG_ERROR("Invalid Srv6 MySid: action=%s", action.c_str());
        return false;
    }

    SWSS_LOG_INFO("Rx block_len:%s node_len:%s func_len:%s arg_len:%s "
                  "action:%s vrf:%s adj:%s",
                  block_len.c_str(), node_len.c_str(), func_len.c_str(),
                  arg_len.c_str(), action.c_str(), vrf.c_str(), adj.c_str());

    return true;
}

void RouteSync::getEvpnNextHopSep(string& nexthops, string& vni_list,  
                   string& mac_list, string& intf_list)
{
    nexthops  += NHG_DELIMITER;
    vni_list  += NHG_DELIMITER;
    mac_list  += NHG_DELIMITER;
    intf_list += NHG_DELIMITER;

    return;
}

void RouteSync::getEvpnNextHopGwIf(char *gwaddr, int vni_value, 
                               string& nexthops, string& vni_list,  
                               string& mac_list, string& intf_list,
                               string rmac, string vlan_id)
{
    nexthops+= gwaddr;
    vni_list+= to_string(vni_value);
    mac_list+=rmac;
    intf_list+=vlan_id;
}

bool RouteSync::getEvpnNextHop(struct nlmsghdr *h, int received_bytes, 
                               struct rtattr *tb[], string& nexthops, 
                               string& vni_list, string& mac_list, 
                               string& intf_list)
{
    void *gate = NULL;
    char nexthopaddr[MAX_ADDR_SIZE] = {0};
    char gateaddr[MAX_ADDR_SIZE] = {0};
    uint32_t encap_value = 0;
    uint32_t ecmp_count = 0;
    uint16_t encap = 0;
    int gw_af;
    struct in6_addr ipv6_address;
    string rmac;
    string vlan;
    int index;
    char if_name[IFNAMSIZ] = "0";
    char ifname_unknown[IFNAMSIZ] = "unknown";

    if (tb[RTA_GATEWAY])
        gate = RTA_DATA(tb[RTA_GATEWAY]);

    if (h->nlmsg_type == RTM_NEWROUTE) 
    {
        if (!tb[RTA_MULTIPATH]) 
        {
            gw_af = AF_INET; // default value
            if (gate)
            {
                if (RTA_PAYLOAD(tb[RTA_GATEWAY]) <= IPV4_MAX_BYTE)
                {
                    memcpy(gateaddr, gate, IPV4_MAX_BYTE);
                    gw_af = AF_INET;
                }
                else
                {
                    memcpy(ipv6_address.s6_addr, gate, IPV6_MAX_BYTE);
                    gw_af = AF_INET6;                    
                }
            }

            if(gw_af == AF_INET6)
            {
                if (IN6_IS_ADDR_V4MAPPED(&ipv6_address))
                {
                    memcpy(gateaddr, (ipv6_address.s6_addr+12), IPV4_MAX_BYTE);
                    gw_af = AF_INET;
                }
                else
                {
                    SWSS_LOG_NOTICE("IPv6 tunnel nexthop not supported Nexthop:%s encap:%d encap_value:%d",
                                    inet_ntop(gw_af, ipv6_address.s6_addr, nexthopaddr, MAX_ADDR_SIZE), encap, encap_value);
                    return false;
                }
            }

            inet_ntop(gw_af, gateaddr, nexthopaddr, MAX_ADDR_SIZE);

            if (tb[RTA_OIF])
            {
                index = *(int *)RTA_DATA(tb[RTA_OIF]);

                /* If we cannot get the interface name */
                if (!getIfName(index, if_name, IFNAMSIZ))
                {
                    strcpy(if_name, ifname_unknown);
                }

                vlan = if_name;
            }

            if (tb[RTA_ENCAP_TYPE])
            {
                encap = *(uint16_t *)RTA_DATA(tb[RTA_ENCAP_TYPE]);
            }

            if (tb[RTA_ENCAP] && tb[RTA_ENCAP_TYPE]
                && (*(uint16_t *)RTA_DATA(tb[RTA_ENCAP_TYPE]) == NH_ENCAP_VXLAN)) 
            {
                parseEncap(tb[RTA_ENCAP], encap_value, rmac);
            }
            SWSS_LOG_DEBUG("Rx MsgType:%d Nexthop:%s encap:%d encap_value:%d rmac:%s vlan:%s", h->nlmsg_type,
                            nexthopaddr, encap, encap_value, rmac.c_str(), vlan.c_str());

            if (encap_value == 0 || !(vlan.compare(ifname_unknown)) || MacAddress(rmac) == MacAddress("00:00:00:00:00:00"))
            {
                return false;
            }

            getEvpnNextHopGwIf(nexthopaddr, encap_value, nexthops, vni_list, mac_list, intf_list, rmac, vlan);
        }
        else
        {
            /* This is a multipath route */
            /* Need to add the code for multipath */
            int len;
            struct rtattr *subtb[RTA_MAX + 1];
            struct rtnexthop *rtnh = (struct rtnexthop *)RTA_DATA(tb[RTA_MULTIPATH]);
            len = (int)RTA_PAYLOAD(tb[RTA_MULTIPATH]);

            for (;;) 
            {
                uint16_t encap = 0;
                if (len < (int)sizeof(*rtnh) || rtnh->rtnh_len > len)
                {
                    break;
                }

                gate = 0;
                if (rtnh->rtnh_len > sizeof(*rtnh)) 
                {
                    memset(subtb, 0, sizeof(subtb));

                    netlink_parse_rtattr(subtb, RTA_MAX, RTNH_DATA(rtnh),
                                          (int)(rtnh->rtnh_len - sizeof(*rtnh)));

                    if (subtb[RTA_GATEWAY])
                    {
                        gate = RTA_DATA(subtb[RTA_GATEWAY]);
                    }

                    if (gate)
                    {
                        if (RTA_PAYLOAD(subtb[RTA_GATEWAY]) <= IPV4_MAX_BYTE)
                        {
                            memcpy(gateaddr, gate, IPV4_MAX_BYTE);
                            gw_af = AF_INET;
                        }
                        else
                        {
                            memcpy(ipv6_address.s6_addr, gate, IPV6_MAX_BYTE);
                            gw_af = AF_INET6;                    
                        }
                    }
                    
                    if(gw_af == AF_INET6)
                    {
                        if (IN6_IS_ADDR_V4MAPPED(&ipv6_address))
                        {
                            memcpy(gateaddr, (ipv6_address.s6_addr+12), IPV4_MAX_BYTE);
                            gw_af = AF_INET;
                        }
                        else
                        {
                            SWSS_LOG_NOTICE("IPv6 tunnel nexthop not supported Nexthop:%s encap:%d encap_value:%d",
                                            inet_ntop(gw_af, ipv6_address.s6_addr, nexthopaddr, MAX_ADDR_SIZE), encap, encap_value);
                            return false;
                        }
                    }
                    
                    inet_ntop(gw_af, gateaddr, nexthopaddr, MAX_ADDR_SIZE);


                    if (rtnh->rtnh_ifindex)
                    {
                        index = rtnh->rtnh_ifindex;

                        /* If we cannot get the interface name */
                        if (!getIfName(index, if_name, IFNAMSIZ))
                        {
                            strcpy(if_name, ifname_unknown);
                        }

                        vlan = if_name;
                    }

                    if (subtb[RTA_ENCAP_TYPE])
                    {
                        encap = *(uint16_t *)RTA_DATA(subtb[RTA_ENCAP_TYPE]);
                    }

                    if (subtb[RTA_ENCAP] && subtb[RTA_ENCAP_TYPE]
                        && (*(uint16_t *)RTA_DATA(subtb[RTA_ENCAP_TYPE]) == NH_ENCAP_VXLAN))
                    {
                        parseEncap(subtb[RTA_ENCAP], encap_value, rmac);
                    }
                    SWSS_LOG_DEBUG("Multipath Nexthop:%s encap:%d encap_value:%d rmac:%s vlan:%s",
                                    nexthopaddr, encap, encap_value, rmac.c_str(), vlan.c_str());

                    if (encap_value == 0 || !(vlan.compare(ifname_unknown)) || MacAddress(rmac) == MacAddress("00:00:00:00:00:00"))
                    {
                        return false;
                    }

                    if (gate)
                    {
                        if (ecmp_count)
                        {
                            getEvpnNextHopSep(nexthops, vni_list, mac_list, intf_list);
                        }

                        getEvpnNextHopGwIf(nexthopaddr, encap_value, nexthops, vni_list, mac_list, intf_list, rmac, vlan);
                        ecmp_count++;
                    }
                }

                if (rtnh->rtnh_len == 0)
                {
                    break;
                }

                len -= NLMSG_ALIGN(rtnh->rtnh_len);
                rtnh = RTNH_NEXT(rtnh);
            }			
        }
    }
    return true;
}

void RouteSync::onEvpnRouteMsg(struct nlmsghdr *h, int len)
{
    struct rtmsg *rtm;
    struct rtattr *tb[RTA_MAX + 1] = {0};
    void *dest = NULL;
    char anyaddr[16] = {0};
    char dstaddr[16] = {0};
    int  dst_len = 0;
    char buf[MAX_ADDR_SIZE];
    char destipprefix[IFNAMSIZ + MAX_ADDR_SIZE + 2] = {0};
    int nlmsg_type = h->nlmsg_type;
    unsigned int vrf_index;

    rtm = (struct rtmsg *)NLMSG_DATA(h);

    /* Parse attributes and extract fields of interest. */
    netlink_parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);

    if (tb[RTA_DST])
    {
        dest = RTA_DATA(tb[RTA_DST]);
    }
    else
    {
        dest = anyaddr;
    }

    if (rtm->rtm_family == AF_INET)
    {
        if (rtm->rtm_dst_len > IPV4_MAX_BITLEN)
        {
            return;
        }
        memcpy(dstaddr, dest, IPV4_MAX_BYTE);
        dst_len = rtm->rtm_dst_len;
    }
    else if (rtm->rtm_family == AF_INET6)
    {
        if (rtm->rtm_dst_len > IPV6_MAX_BITLEN) 
        {
            return;
        }
        memcpy(dstaddr, dest, IPV6_MAX_BYTE);
        dst_len = rtm->rtm_dst_len;
    }

    SWSS_LOG_DEBUG("Rx MsgType:%d Family:%d Prefix:%s/%d", h->nlmsg_type, rtm->rtm_family,
                    inet_ntop(rtm->rtm_family, dstaddr, buf, MAX_ADDR_SIZE), dst_len);

    /* Table corresponding to route. */
    if (tb[RTA_TABLE])
    {
        vrf_index = *(int *)RTA_DATA(tb[RTA_TABLE]);
    }
    else
    {
        vrf_index = rtm->rtm_table;
    }

    if (vrf_index)
    {
        if (!getIfName(vrf_index, destipprefix, IFNAMSIZ))
        {
            SWSS_LOG_ERROR("Fail to get the VRF name (ifindex %u)", vrf_index);
            return;
        }
        /*
         * Now vrf device name is required to start with VRF_PREFIX,
         * it is difficult to split vrf_name:ipv6_addr.
         */
        if (memcmp(destipprefix, VRF_PREFIX, strlen(VRF_PREFIX)))
        {
            SWSS_LOG_ERROR("Invalid VRF name %s (ifindex %u)", destipprefix, vrf_index);
            return;
        }
        destipprefix[strlen(destipprefix)] = ':';
    }

    if((rtm->rtm_family == AF_INET && dst_len == IPV4_MAX_BITLEN)
        || (rtm->rtm_family == AF_INET6 && dst_len == IPV6_MAX_BITLEN))
    {
        snprintf(destipprefix + strlen(destipprefix), sizeof(destipprefix) - strlen(destipprefix), "%s",
                inet_ntop(rtm->rtm_family, dstaddr, buf, MAX_ADDR_SIZE));
    }
    else
    {
        snprintf(destipprefix + strlen(destipprefix), sizeof(destipprefix) - strlen(destipprefix), "%s/%u",
                inet_ntop(rtm->rtm_family, dstaddr, buf, MAX_ADDR_SIZE), dst_len);
    }

    auto proto_str = getProtocolString(rtm->rtm_protocol);
    SWSS_LOG_INFO("Receive route message dest ip prefix: %s Op:%s", 
                    destipprefix,
                    nlmsg_type == RTM_NEWROUTE ? "add":"del");

    /*
     * Upon arrival of a delete msg we could either push the change right away,
     * or we could opt to defer it if we are going through a warm-reboot cycle.
     */
    bool warmRestartInProgress = m_warmStartHelper->inProgress();

    if (nlmsg_type == RTM_DELROUTE)
    {
        if (!warmRestartInProgress)
        {
        	SWSS_LOG_ERROR("[TEST] RouteSync::onEvpnRouteMsg, m_routeTable del: %s",destipprefix);
            m_routeTable->del(destipprefix);
            return;
        }
        else
        {
            SWSS_LOG_INFO("Warm-Restart mode: Receiving delete msg: %s",
                          destipprefix);

            vector<FieldValueTuple> fvVector;
            const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                               DEL_COMMAND,
                                                               fvVector);
            m_warmStartHelper->insertRefreshMap(kfv);
            return;
        }
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        return;
    }

    sendOffloadReply(h);

    switch (rtm->rtm_type)
    {
        case RTN_BLACKHOLE:
        case RTN_UNREACHABLE:
        case RTN_PROHIBIT:
        {
            SWSS_LOG_ERROR("RTN_BLACKHOLE route not expected (%s)", destipprefix);
            return;
        }
        case RTN_UNICAST:
            break;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            SWSS_LOG_NOTICE("BUM routes aren't supported yet (%s)", destipprefix);
            return;

        default:
            return;
    }

    /* Get nexthop lists */
    string nexthops;
    string vni_list;
    string mac_list;
    string intf_list;
    bool ret;

    ret = getEvpnNextHop(h, len, tb, nexthops, vni_list, mac_list, intf_list);
    if (ret == false)
    {
        SWSS_LOG_NOTICE("EVPN Route issue with RouteTable msg: %s vtep:%s vni:%s mac:%s intf:%s",
                       destipprefix, nexthops.c_str(), vni_list.c_str(), mac_list.c_str(), intf_list.c_str());
        return;
    }

    if (nexthops.empty() || mac_list.empty())
    {
        SWSS_LOG_NOTICE("EVPN IP Prefix: %s nexthop or rmac is empty", destipprefix);
        return;
    }

    vector<FieldValueTuple> fvVector;
    FieldValueTuple nh("nexthop", nexthops);
    FieldValueTuple intf("ifname", intf_list);
    FieldValueTuple vni("vni_label", vni_list);
    FieldValueTuple mac("router_mac", mac_list);
    FieldValueTuple proto("protocol", proto_str);

    fvVector.push_back(nh);
    fvVector.push_back(intf);
    fvVector.push_back(vni);
    fvVector.push_back(mac);
    fvVector.push_back(proto);

    if (!warmRestartInProgress)
    {
    	SWSS_LOG_ERROR("[TEST] RouteSync::onEvpnRouteMsg, m_routeTable set: %s",destipprefix);
        m_routeTable->set(destipprefix, fvVector);
        SWSS_LOG_DEBUG("RouteTable set msg: %s vtep:%s vni:%s mac:%s intf:%s protocol:%s",
                       destipprefix, nexthops.c_str(), vni_list.c_str(), mac_list.c_str(), intf_list.c_str(),
                       proto_str.c_str());
    }

    /*
     * During routing-stack restarting scenarios route-updates will be temporarily
     * put on hold by warm-reboot logic.
     */
    else
    {
        SWSS_LOG_INFO("Warm-Restart mode: RouteTable set msg: %s vtep:%s vni:%s mac:%s",
                      destipprefix, nexthops.c_str(), vni_list.c_str(), mac_list.c_str());

        const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                           SET_COMMAND,
                                                           fvVector);
        m_warmStartHelper->insertRefreshMap(kfv);
    }
    return;
}

bool RouteSync::getSrv6SteerRouteNextHop(struct nlmsghdr *h, int received_bytes,
                               struct rtattr *tb[], string &vpn_sid,
                               string &src_addr)
{
    uint16_t encap = 0;

    if (!tb[RTA_MULTIPATH])
    {
        if (tb[RTA_ENCAP_TYPE])
        {
            encap = *(uint16_t *)RTA_DATA(tb[RTA_ENCAP_TYPE]);
        }

        if (tb[RTA_ENCAP] && tb[RTA_ENCAP_TYPE] &&
            *(uint16_t *)RTA_DATA(tb[RTA_ENCAP_TYPE]) ==
                NH_ENCAP_SRV6_ROUTE)
        {
            parseEncapSrv6SteerRoute(tb[RTA_ENCAP], vpn_sid, src_addr);
        }
        SWSS_LOG_DEBUG("Rx MsgType:%d encap:%d vpn_sid:%s src_addr:%s",
                        h->nlmsg_type, encap, vpn_sid.c_str(),
                        src_addr.c_str());

        if (vpn_sid.empty())
        {
            SWSS_LOG_ERROR("Received an invalid SRv6 route: vpn_sid is empty");
            return false;
        }
    }
    else
    {
        /* This is a multipath route */
        SWSS_LOG_NOTICE("Multipath SRv6 routes aren't supported");
        return false;
    }

    return true;
}

void RouteSync::onSrv6SteerRouteMsg(struct nlmsghdr *h, int len)
{
    struct rtmsg *rtm;
    struct rtattr *tb[RTA_MAX + 1];
    void *dest = NULL;
    char dstaddr[IPV6_MAX_BYTE] = {0};
    int dst_len = 0;
    char destipprefix[MAX_ADDR_SIZE + 1] = {0};
    char routeTableKey[IFNAMSIZ + MAX_ADDR_SIZE + 2] = {0};
    int nlmsg_type = h->nlmsg_type;
    unsigned int vrf_index;

    rtm = (struct rtmsg *)NLMSG_DATA(h);

    /* Parse attributes and extract fields of interest. */
    memset(tb, 0, sizeof(tb));
    netlink_parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);

    if (!tb[RTA_DST])
    {
        SWSS_LOG_ERROR(
            "Received an invalid SRv6 route: missing RTA_DST attribute");
        return;
    }

    dest = RTA_DATA(tb[RTA_DST]);

    if (rtm->rtm_family == AF_INET)
    {
        if (rtm->rtm_dst_len > IPV4_MAX_BITLEN)
        {
            SWSS_LOG_ERROR(
                "Received an invalid SRv6 route: prefix len %d is out of range",
                rtm->rtm_dst_len);
            return;
        }
        memcpy(dstaddr, dest, IPV4_MAX_BYTE);
        dst_len = rtm->rtm_dst_len;
    }
    else if (rtm->rtm_family == AF_INET6)
    {
        if (rtm->rtm_dst_len > IPV6_MAX_BITLEN)
        {
            SWSS_LOG_ERROR(
                "Received an invalid SRv6 route: prefix len %d is out of range",
                rtm->rtm_dst_len);
            return;
        }
        memcpy(dstaddr, dest, IPV6_MAX_BYTE);
        dst_len = rtm->rtm_dst_len;
    }
    else
    {
        SWSS_LOG_ERROR(
            "Received an invalid SRv6 route: invalid address family %d",
            rtm->rtm_family);
        return;
    }

    inet_ntop(rtm->rtm_family, dstaddr, destipprefix, MAX_ADDR_SIZE);

    SWSS_LOG_DEBUG("Rx MsgType:%d Family:%d Prefix:%s/%d", nlmsg_type,
                   rtm->rtm_family, destipprefix, dst_len);

    /* Table corresponding to route. */
    if (tb[RTA_TABLE])
    {
        vrf_index = *(int *)RTA_DATA(tb[RTA_TABLE]);
    }
    else
    {
        vrf_index = rtm->rtm_table;
    }

    if (vrf_index)
    {
        if (!getIfName(vrf_index, routeTableKey, IFNAMSIZ))
        {
            SWSS_LOG_ERROR("Fail to get the VRF name (ifindex %u)", vrf_index);
            return;
        }
        /*
         * Now vrf device name is required to start with VRF_PREFIX
         */
        if (memcmp(routeTableKey, VRF_PREFIX, strlen(VRF_PREFIX)))
        {
            SWSS_LOG_ERROR("Invalid VRF name %s (ifindex %u)", routeTableKey,
                           vrf_index);
            return;
        }
        routeTableKey[strlen(routeTableKey)] = ':';
    }

    if ((rtm->rtm_family == AF_INET && dst_len == IPV4_MAX_BITLEN) ||
        (rtm->rtm_family == AF_INET6 && dst_len == IPV6_MAX_BITLEN))
    {
        snprintf(routeTableKey + strlen(routeTableKey),
                 sizeof(routeTableKey) - strlen(routeTableKey), "%s",
                 destipprefix);
    }
    else
    {
        snprintf(routeTableKey + strlen(routeTableKey),
                 sizeof(routeTableKey) - strlen(routeTableKey), "%s/%u",
                 destipprefix, dst_len);
    }

    SWSS_LOG_INFO("Received route message dest ip prefix: %s Op:%s",
                  destipprefix, nlmsg_type == RTM_NEWROUTE ? "add" : "del");

    if (nlmsg_type != RTM_NEWROUTE && nlmsg_type != RTM_DELROUTE)
    {
        SWSS_LOG_ERROR("Unknown message-type: %d for %s", nlmsg_type,
                       destipprefix);
        return;
    }

    switch (rtm->rtm_type)
    {
        case RTN_BLACKHOLE:
        case RTN_UNREACHABLE:
        case RTN_PROHIBIT:
            SWSS_LOG_ERROR(
                "RTN_BLACKHOLE route not expected (%s)", destipprefix);
            return;
        case RTN_UNICAST:
            break;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            SWSS_LOG_NOTICE(
                "BUM routes aren't supported yet (%s)", destipprefix);
            return;

        default:
            return;
    }

    /* Get nexthop lists */
    string vpn_sid_str;
    string src_addr_str;
    bool ret;

    ret = getSrv6SteerRouteNextHop(h, len, tb, vpn_sid_str, src_addr_str);
    if (ret == false)
    {
        SWSS_LOG_NOTICE(
            "SRv6 Route issue with RouteTable msg: %s vpn_sid:%s src_addr:%s",
            destipprefix, vpn_sid_str.c_str(), src_addr_str.c_str());
        return;
    }

    if (vpn_sid_str.empty())
    {
        SWSS_LOG_NOTICE("SRv6 IP Prefix: %s vpn_sid is empty", destipprefix);
        return;
    }

    bool warmRestartInProgress = m_warmStartHelper->inProgress();

    if (nlmsg_type == RTM_DELROUTE)
    {
        string srv6SidListTableKey = routeTableKey;

        if (!warmRestartInProgress)
        {
            SWSS_LOG_ERROR("[TEST] RouteSync::onSrv6SteerRouteMsg, m_routeTable del: %s",routeTableKey);
            m_routeTable->del(routeTableKey);
            m_srv6SidListTable.del(srv6SidListTableKey);
            return;
        }
        else
        {
            SWSS_LOG_INFO("Warm-Restart mode: Receiving delete msg: %s",
                          routeTableKey);

            vector<FieldValueTuple> fvVector;
            const KeyOpFieldsValuesTuple kfv = std::make_tuple(routeTableKey,
                                                               DEL_COMMAND,
                                                               fvVector);
            m_warmStartHelper->insertRefreshMap(kfv);
            return;
        }
    }
    else if (nlmsg_type == RTM_NEWROUTE)
    {
        /* Write SID list to SRV6_SID_LIST_TABLE */

        string srv6SidListTableKey = routeTableKey;

        vector<FieldValueTuple> fvVectorSidList;

        FieldValueTuple path("path", vpn_sid_str);
        fvVectorSidList.push_back(path);

        m_srv6SidListTable.set(srv6SidListTableKey, fvVectorSidList);
        SWSS_LOG_DEBUG("Srv6SidListTable set msg: %s path: %s",
                        srv6SidListTableKey.c_str(), vpn_sid_str.c_str());

        /* Write route to ROUTE_TABLE */

        vector<FieldValueTuple> fvVectorRoute;

        FieldValueTuple vpn_sid("segment", srv6SidListTableKey);
        fvVectorRoute.push_back(vpn_sid);

        if (!src_addr_str.empty())
        {
            FieldValueTuple seg_src("seg_src", src_addr_str);
            fvVectorRoute.push_back(seg_src);
        }
        if (!warmRestartInProgress)
        {
            SWSS_LOG_ERROR("[TEST] RouteSync::onSrv6SteerRouteMsg, m_routeTable set: %s",routeTableKey);
            m_routeTable->set(routeTableKey, fvVectorRoute);
            SWSS_LOG_DEBUG("RouteTable set msg: %s vpn_sid: %s src_addr:%s",
                        routeTableKey, vpn_sid_str.c_str(),
                        src_addr_str.c_str());
        }

        /*
        * During routing-stack restarting scenarios route-updates will be
        * temporarily put on hold by warm-reboot logic.
        */
        else
        {
            SWSS_LOG_INFO(
                "Warm-Restart mode: RouteTable set msg: %s vpn_sid:%s src_addr:%s",
                routeTableKey, vpn_sid_str.c_str(), src_addr_str.c_str());

            const KeyOpFieldsValuesTuple kfv =
                std::make_tuple(routeTableKey, SET_COMMAND, fvVectorRoute);
            m_warmStartHelper->insertRefreshMap(kfv);
        }
    }

    return;
}

void RouteSync::onSrv6MySidMsg(struct nlmsghdr *h, int len)
{
    struct rtmsg *rtm;
    struct rtattr *tb[RTA_MAX + 1];
    void *sid_value_tmp = NULL;
    char sid_value[IPV6_MAX_BYTE] = {0};
    char sid_value_str[MAX_ADDR_SIZE];
    int nlmsg_type = h->nlmsg_type;

    rtm = (struct rtmsg *)NLMSG_DATA(h);

    /* Parse attributes and extract fields of interest. */
    memset(tb, 0, sizeof(tb));
    netlink_parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);

    if (!tb[SRV6_LOCALSID_SID_VALUE])
    {
        SWSS_LOG_ERROR(
            "Received an invalid MySid route: missing SRV6_MY_SID_SID_VALUE attribute");
        return;
    }

    sid_value_tmp = RTA_DATA(tb[SRV6_LOCALSID_SID_VALUE]);

    /*
     * Only AF_INET6 is allowed for MySid routes
     */
    if (rtm->rtm_family == AF_INET)
    {
        SWSS_LOG_ERROR(
            "AF_INET address family is not allowed for MySid");
        return;
    }
    else if (rtm->rtm_family == AF_INET6)
    {
        if (rtm->rtm_dst_len > IPV6_MAX_BITLEN)
        {
            SWSS_LOG_ERROR("Received an invalid MySid: prefix len %d "
                           "is out of range",
                           rtm->rtm_dst_len);
            return;
        }
        memcpy(sid_value, sid_value_tmp, IPV6_MAX_BYTE);
    }
    else
    {
        SWSS_LOG_ERROR(
            "Received an invalid MySid route: invalid address family %d",
            rtm->rtm_family);
        return;
    }

    inet_ntop(AF_INET6, sid_value, sid_value_str, MAX_ADDR_SIZE);

    SWSS_LOG_INFO("Rx MsgType:%d SidValue:%s", nlmsg_type,
                   sid_value_str);

    if (nlmsg_type != RTM_NEWSRV6LOCALSID && nlmsg_type != RTM_DELSRV6LOCALSID)
    {
        SWSS_LOG_ERROR("Unknown message-type: %d for %s", nlmsg_type,
                       sid_value_str);
        return;
    }

    /* Get nexthop lists */
    string block_len_str;
    string node_len_str;
    string func_len_str;
    string arg_len_str;
    string action_str;
    string vrf_str;
    string adj_str;
    string my_sid_table_key;

    if (!parseSrv6MySid(tb, block_len_str, node_len_str,
                      func_len_str, arg_len_str, action_str, vrf_str,
                      adj_str))
    {
        SWSS_LOG_ERROR("Invalid Srv6 MySid");
        return;
    }

    if (block_len_str.empty())
    {
        block_len_str = DEFAULT_SRV6_MY_SID_BLOCK_LEN;
    }

    if (node_len_str.empty())
    {
        node_len_str = DEFAULT_SRV6_MY_SID_NODE_LEN;
    }

    if (func_len_str.empty())
    {
        func_len_str = DEFAULT_SRV6_MY_SID_FUNC_LEN;
    }

    if (arg_len_str.empty())
    {
        arg_len_str = DEFAULT_SRV6_MY_SID_ARG_LEN;
    }

    my_sid_table_key += block_len_str + MY_SID_KEY_DELIMITER;
    my_sid_table_key += node_len_str + MY_SID_KEY_DELIMITER;
    my_sid_table_key += func_len_str + MY_SID_KEY_DELIMITER;
    my_sid_table_key += arg_len_str + MY_SID_KEY_DELIMITER;
    my_sid_table_key += sid_value_str;

    if (nlmsg_type == RTM_DELSRV6LOCALSID)
    {
        m_srv6MySidTable.del(my_sid_table_key);
        return;
    }

    if (action_str.empty() || !(action_str.compare("unspec")) ||
        !(action_str.compare("unknown")))
    {
        SWSS_LOG_NOTICE("Mysid IP Prefix: %s act is empty or invalid",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("end.dt6")) && vrf_str.empty())
    {
        SWSS_LOG_NOTICE("Mysid End.DT6 IP Prefix: %s vrf is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("end.dt4")) && vrf_str.empty())
    {
        SWSS_LOG_NOTICE("Mysid End.DT4 IP Prefix: %s vrf is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("end.dt46")) && vrf_str.empty())
    {
        SWSS_LOG_NOTICE("Mysid End.DT46 IP Prefix: %s vrf is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("udt6")) && vrf_str.empty())
    {
        SWSS_LOG_NOTICE("Mysid uDT6 IP Prefix: %s vrf is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("udt4")) && vrf_str.empty())
    {
        SWSS_LOG_NOTICE("Mysid uDT4 IP Prefix: %s vrf is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("udt46")) && vrf_str.empty())
    {
        SWSS_LOG_NOTICE("Mysid uDT46 IP Prefix: %s vrf is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("end.t")) && vrf_str.empty())
    {
        SWSS_LOG_NOTICE("Mysid End.T IP Prefix: %s vrf is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("end.x")) && adj_str.empty())
    {
        SWSS_LOG_NOTICE("MySid End.X IP Prefix: %s adj is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("end.dx6")) && adj_str.empty())
    {
        SWSS_LOG_NOTICE("MySid End.DX6 IP Prefix: %s adj is empty",
                        sid_value_str);
        return;
    }

    if (!(action_str.compare("end.dx4")) && adj_str.empty())
    {
        SWSS_LOG_NOTICE("MySid End.DX4 IP Prefix: %s adj is empty",
                        sid_value_str);
        return;
    }

    vector<FieldValueTuple> fvVector;
    FieldValueTuple act("action", action_str);
    fvVector.push_back(act);
    if (!vrf_str.empty())
    {
        FieldValueTuple vrf("vrf", vrf_str);
        fvVector.push_back(vrf);
    }
    if (!adj_str.empty())
    {
        FieldValueTuple adj("adj", adj_str);
        fvVector.push_back(adj);
    }

    m_srv6MySidTable.set(my_sid_table_key, fvVector);

    return;
}

uint16_t RouteSync::getEncapType(struct nlmsghdr *h)
{
    int len;
    uint16_t encap_type = 0;
    struct rtmsg *rtm;
    struct rtattr *tb[RTA_MAX + 1];

    rtm = (struct rtmsg *)NLMSG_DATA(h);

    if (h->nlmsg_type != RTM_NEWROUTE && h->nlmsg_type != RTM_DELROUTE)
    {
        return 0;
    }

    len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg)));
    if (len < 0)
    {
        return 0;
    }

    memset(tb, 0, sizeof(tb));
    netlink_parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);

    if (!tb[RTA_MULTIPATH])
    {
        if (tb[RTA_ENCAP_TYPE])
        {
            encap_type = *(short *)RTA_DATA(tb[RTA_ENCAP_TYPE]);
        }
    }
    else
    {
        /* This is a multipath route */
        int len;
        struct rtnexthop *rtnh =
            (struct rtnexthop *)RTA_DATA(tb[RTA_MULTIPATH]);
        len = (int)RTA_PAYLOAD(tb[RTA_MULTIPATH]);
        struct rtattr *subtb[RTA_MAX + 1];

        for (;;)
        {
            if (len < (int)sizeof(*rtnh) || rtnh->rtnh_len > len)
            {
                break;
            }

            if (rtnh->rtnh_len > sizeof(*rtnh))
            {
                memset(subtb, 0, sizeof(subtb));
                netlink_parse_rtattr(subtb, RTA_MAX, RTNH_DATA(rtnh),
                                     (int)(rtnh->rtnh_len - sizeof(*rtnh)));
                if (subtb[RTA_ENCAP_TYPE])
                {
                    encap_type = *(uint16_t *)RTA_DATA(subtb[RTA_ENCAP_TYPE]);
                    break;
                }
            }

            if (rtnh->rtnh_len == 0)
            {
                break;
            }

            len -= NLMSG_ALIGN(rtnh->rtnh_len);
            rtnh = RTNH_NEXT(rtnh);
        }
    }

    SWSS_LOG_INFO("Rx MsgType:%d Encap:%d", h->nlmsg_type, encap_type);

    return encap_type;
}

void RouteSync::onMsgRaw(struct nlmsghdr *h)
{
    int len;

    if ((h->nlmsg_type != RTM_NEWROUTE)
        && (h->nlmsg_type != RTM_DELROUTE)
        && (h->nlmsg_type != RTM_NEWSRV6LOCALSID)
        && (h->nlmsg_type != RTM_DELSRV6LOCALSID)
        && (h->nlmsg_type != RTM_NEWNEXTHOP)
        && (h->nlmsg_type != RTM_DELNEXTHOP)
    )
        return;

    if(h->nlmsg_type == RTM_NEWNEXTHOP || h->nlmsg_type == RTM_DELNEXTHOP)
    {
        len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg)));
    }
    else
    {
        len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(struct ndmsg)));
    }
    /* Length validity. */
    if (len < 0) 
    {
        SWSS_LOG_ERROR("%s: Message received from netlink is of a broken size %d %zu",
            __PRETTY_FUNCTION__, h->nlmsg_len,
            (size_t)NLMSG_LENGTH(sizeof(struct ndmsg)));
        return;
    }

    if(h->nlmsg_type == RTM_NEWNEXTHOP || h->nlmsg_type == RTM_DELNEXTHOP)
    {
        onNextHopMsg(h, len);
        return;
    }
    
    if ((h->nlmsg_type == RTM_NEWSRV6LOCALSID)
        || (h->nlmsg_type == RTM_DELSRV6LOCALSID))
    {
        onSrv6MySidMsg(h, len);
        return;
    }

    switch (getEncapType(h))
    {
        case NH_ENCAP_SRV6_ROUTE:
            onSrv6SteerRouteMsg(h, len);
            break;
        default:
            /*
             * Currently only SRv6 route, SRv6 My SID, and EVPN
             * encapsulation types are supported. If the encapsulation
             * type is not SRv6 route or SRv6 My SID, we fall back
             * to EVPN. The onEvpnRouteMsg() handler will verify that the
             * route is actually an EVPN route. If it is not, this handler
             * will reject the route.
             */
            onEvpnRouteMsg(h, len);
            break;
    }
}

void RouteSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    if (nlmsg_type == RTM_NEWLINK || nlmsg_type == RTM_DELLINK)
    {
        nl_cache_refill(m_nl_sock, m_link_cache);
        return;
    }

    struct rtnl_route *route_obj = (struct rtnl_route *)obj;

    /* Supports IPv4 or IPv6 address, otherwise return immediately */
    auto family = rtnl_route_get_family(route_obj);
    /* Check for Label route. */
    if (family == AF_MPLS)
    {
        onLabelRouteMsg(nlmsg_type, obj);
        return;
    }
    if (family != AF_INET && family != AF_INET6)
    {
        SWSS_LOG_INFO("Unknown route family support (object: %s)", nl_object_get_type(obj));
        return;
    }

    /* Get the index of the master device */
    unsigned int master_index = rtnl_route_get_table(route_obj);
    char master_name[IFNAMSIZ] = {0};

    /* if the table_id is not set in the route obj then route is for default vrf. */
    if (master_index)
    {
        /* Get the name of the master device */
        getIfName(master_index, master_name, IFNAMSIZ);
    
        /* If the master device name starts with VNET_PREFIX, it is a VNET route.
           The VNET name is exactly the name of the associated master device. */
        if (string(master_name).find(VNET_PREFIX) == 0)
        {
            onVnetRouteMsg(nlmsg_type, obj, string(master_name));
        }
        /* Otherwise, it is a regular route (include VRF route). */
        else
        {
            onRouteMsg(nlmsg_type, obj, master_name);
        }
    }
    else
    {
        onRouteMsg(nlmsg_type, obj, NULL);
    }
}

/* 
 * Handle regular route (include VRF route) 
 * @arg nlmsg_type      Netlink message type
 * @arg obj             Netlink object
 * @arg vrf             Vrf name
 */
void RouteSync::onRouteMsg(int nlmsg_type, struct nl_object *obj, char *vrf)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;
    struct nl_addr *dip;
    char destipprefix[IFNAMSIZ + MAX_ADDR_SIZE + 2] = {0};

    if (vrf)
    {
        /*
         * Now vrf device name is required to start with VRF_PREFIX,
         * it is difficult to split vrf_name:ipv6_addr.
         */
        if (memcmp(vrf, VRF_PREFIX, strlen(VRF_PREFIX)))
        {
            if(memcmp(vrf, MGMT_VRF_PREFIX, strlen(MGMT_VRF_PREFIX)))
            {
                SWSS_LOG_ERROR("Invalid VRF name %s (ifindex %u)", vrf, rtnl_route_get_table(route_obj));
            }
            else
            {
                dip = rtnl_route_get_dst(route_obj);
                nl_addr2str(dip, destipprefix, MAX_ADDR_SIZE);
                SWSS_LOG_INFO("Skip routes for Mgmt VRF name %s (ifindex %u) prefix: %s", vrf,
                        rtnl_route_get_table(route_obj), destipprefix);
            }
            return;
        }
        memcpy(destipprefix, vrf, strlen(vrf));
        destipprefix[strlen(vrf)] = ':';
    }

    dip = rtnl_route_get_dst(route_obj);
    nl_addr2str(dip, destipprefix + strlen(destipprefix), MAX_ADDR_SIZE);

    /*
     * Upon arrival of a delete msg we could either push the change right away,
     * or we could opt to defer it if we are going through a warm-reboot cycle.
     */
    bool warmRestartInProgress = m_warmStartHelper->inProgress();

    if (nlmsg_type == RTM_DELROUTE)
    {
        if (!warmRestartInProgress)
        {
            SWSS_LOG_ERROR("[TEST] RouteSync::onRouteMsg, m_routeTable del: %s", destipprefix);
            m_routeTable->del(destipprefix);
            return;
        }
        else
        {
            SWSS_LOG_INFO("Warm-Restart mode: Receiving delete msg: %s",
                          destipprefix);

            vector<FieldValueTuple> fvVector;
            const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                               DEL_COMMAND,
                                                               fvVector);
            m_warmStartHelper->insertRefreshMap(kfv);
            return;
        }
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        SWSS_LOG_INFO("Unknown message-type: %d for %s", nlmsg_type, destipprefix);
        return;
    }

    if (!isSuppressionEnabled())
    {
        sendOffloadReply(route_obj);
    }

    switch (rtnl_route_get_type(route_obj))
    {
        case RTN_BLACKHOLE:
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple fv("blackhole", "true");
            fvVector.push_back(fv);
            SWSS_LOG_ERROR("[TEST] RouteSync::onRouteMsg, m_routeTable set: %s", destipprefix);
            m_routeTable->set(destipprefix, fvVector);
            return;
        }
        case RTN_UNICAST:
            break;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            SWSS_LOG_INFO("BUM routes aren't supported yet (%s)", destipprefix);
            return;

        default:
            return;
    }

    vector<FieldValueTuple> fvVector;
    string gw_list;
    string intf_list;
    string mpls_list;

    string nhg_id_key;
    uint32_t nhg_id = rtnl_route_get_nh_id(route_obj);
    if(nhg_id)
    {
        const auto itg = m_nh_groups.find(nhg_id);
        if(itg == m_nh_groups.end())
        {
            SWSS_LOG_ERROR("NextHop group id %d not found. Dropping the route %s", nhg_id, destipprefix);
            return;
        }
        NextHopGroup& nhg = itg->second;
        if(nhg.group.size() == 0)
        {
        // Using route-table only for single next-hop
        string nexthops = nhg.nexthop.empty() ? (rtnl_route_get_family(route_obj) == AF_INET ? "0.0.0.0" : "::") : nhg.nexthop;
        string ifnames, weights;

        getNextHopGroupFields(nhg, nexthops, ifnames, weights, rtnl_route_get_family(route_obj));

        FieldValueTuple gw("nexthop", nexthops.c_str());
        FieldValueTuple intf("ifname", ifnames.c_str());
        fvVector.push_back(gw);
        fvVector.push_back(intf);

        SWSS_LOG_DEBUG("NextHop group id %d is a single nexthop address. Filling the route table %s with nexthop and ifname", nhg_id, destipprefix);
        }
        else
        {
            nhg_id_key = getNextHopGroupKeyAsString(nhg_id);
            FieldValueTuple nhg("nexthop_group", nhg_id_key.c_str());
            fvVector.push_back(nhg);
            installNextHopGroup(nhg_id);
        }

        auto proto_num = rtnl_route_get_protocol(route_obj);
        auto proto_str = getProtocolString(proto_num);
        FieldValueTuple proto("protocol", proto_str);
        fvVector.push_back(proto);

    }
    else
    {
        struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
        if (!nhs)
        {
            SWSS_LOG_INFO("Nexthop list is empty for %s", destipprefix);
            return;
        }

        /* Get nexthop lists */

        getNextHopList(route_obj, gw_list, mpls_list, intf_list);
        string weights = getNextHopWt(route_obj);

        vector<string> alsv = tokenize(intf_list, NHG_DELIMITER);

        if (alsv.size() == 1)
        {
            if (alsv[0] == "eth0" || alsv[0] == "docker0")
            {
                SWSS_LOG_DEBUG("Skip routes to eth0 or docker0: %s %s %s",
                            destipprefix, gw_list.c_str(), intf_list.c_str());

                if (!warmRestartInProgress)
                {
                    SWSS_LOG_NOTICE("RouteTable del msg for route with only one nh on eth0/docker0: %s %s %s %s",
                                    destipprefix, gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());

                    SWSS_LOG_ERROR("[TEST] RouteSync::onRouteMsg, m_routeTable del 2: %s", destipprefix);
                    m_routeTable->del(destipprefix);
                }
                else
                {
                    SWSS_LOG_NOTICE("Warm-Restart mode: Receiving delete msg for route with only nh on eth0/docker0: %s %s %s %s",
                                    destipprefix, gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());

                    vector<FieldValueTuple> fvVector;
                    const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                                       DEL_COMMAND,
                                                                       fvVector);
                    m_warmStartHelper->insertRefreshMap(kfv);
                }
                return;
            }
        }
        else
        {
            for (auto alias : alsv)
            {
                /*
                * A change in FRR behavior from version 7.2 to 7.5 causes the default route to be updated to eth0
                * during interface up/down events. This skips routes to eth0 or docker0 to avoid such behavior.
                */
                if (alias == "eth0" || alias == "docker0")
                {
                    SWSS_LOG_DEBUG("Skip routes to eth0 or docker0: %s %s %s",
                                destipprefix, gw_list.c_str(), intf_list.c_str());
                    continue;
                }
            }
        }

        auto proto_num = rtnl_route_get_protocol(route_obj);
        auto proto_str = getProtocolString(proto_num);


        FieldValueTuple proto("protocol", proto_str);
        FieldValueTuple gw("nexthop", gw_list);
        FieldValueTuple intf("ifname", intf_list);

        fvVector.push_back(proto);
        fvVector.push_back(gw);
        fvVector.push_back(intf);
        if (!mpls_list.empty())
        {
            FieldValueTuple mpls_nh("mpls_nh", mpls_list);
            fvVector.push_back(mpls_nh);
        }
        if (!weights.empty())
        {
            FieldValueTuple wt("weight", weights);
            fvVector.push_back(wt);
        }
    }

    if (!warmRestartInProgress)
    {
        m_routeTable->set(destipprefix, fvVector);
        if(nhg_id)
        {
            SWSS_LOG_INFO("RouteTable set msg: %s %d ", destipprefix, nhg_id);
        }
        else
        {
            SWSS_LOG_INFO("RouteTable set msg: %s %s %s %s", destipprefix,
                       gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());
        }
    }

    /*
     * During routing-stack restarting scenarios route-updates will be temporarily
     * put on hold by warm-reboot logic.
     */
    else
    {
        SWSS_LOG_INFO("Warm-Restart mode: RouteTable set msg: %s %s %s %s", destipprefix,
                      gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());

        const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                           SET_COMMAND,
                                                           fvVector);
        m_warmStartHelper->insertRefreshMap(kfv);
    }
}

/*
 * Handle Nexthop msg
 * @arg nlmsghdr      Netlink messaged
 */
void RouteSync::onNextHopMsg(struct nlmsghdr *h, int len)
{
    int nlmsg_type = h->nlmsg_type;
    uint32_t id = 0;
    unsigned char addr_family;
    int32_t ifindex = -1, grp_count = 0;
    string ifname;
    struct nhmsg *nhm = NULL;
    struct rtattr *tb[NHA_MAX + 1] = {};
    struct in_addr ipv4 = {0};
    struct in6_addr ipv6 = {0};
    char gateway[INET6_ADDRSTRLEN] = {0};
    char ifname_unknown[IFNAMSIZ] = "unknown";

    nhm = (struct nhmsg *)NLMSG_DATA(h);

    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wcast-align"
    struct rtattr* rta = NHA_RTA(nhm);
    #pragma GCC diagnostic pop

    netlink_parse_rtattr(tb, NHA_MAX, rta, len);

    if (!tb[NHA_ID]) {
        SWSS_LOG_ERROR(
            "Nexthop group without an ID received from the zebra");
        return;
    }

    /* We use the ID key'd nhg table for kernel updates */
    id = *((uint32_t *)RTA_DATA(tb[NHA_ID]));

    addr_family = nhm->nh_family;

    if (nlmsg_type == RTM_NEWNEXTHOP)
    {
        if (tb[NHA_GROUP])
        {
            SWSS_LOG_INFO("New nexthop group message!");

            struct nexthop_grp *nha_grp = (struct nexthop_grp *)RTA_DATA(tb[NHA_GROUP]);
            grp_count = (int)(RTA_PAYLOAD(tb[NHA_GROUP]) / sizeof(*nha_grp));

            if (grp_count > MAX_MULTIPATH_NUM)
            {
                SWSS_LOG_ERROR("Nexthop group count (%d) exceeds the maximum allowed (%d). Clamping to maximum.", grp_count, MAX_MULTIPATH_NUM);
                grp_count = MAX_MULTIPATH_NUM;
            }

            vector<pair<uint32_t, uint8_t>> group(grp_count);
            for (int i = 0; i < grp_count; i++)
            {
                group[i] = std::make_pair(nha_grp[i].id, nha_grp[i].weight + 1);
            }

            auto it = m_nh_groups.find(id);
            if (it != m_nh_groups.end())
            {
                NextHopGroup &nhg = it->second;
                nhg.group = group;
                if (nhg.installed)
                {
                    updateNextHopGroupDb(nhg);
                }
            }
            else
            {
                m_nh_groups.insert({id, NextHopGroup(id, group)});
            }
        }
        else
        {
            if (tb[NHA_GATEWAY])
            {
                if (addr_family == AF_INET)
                {
                    memcpy(&ipv4, (void *)RTA_DATA(tb[NHA_GATEWAY]), 4);
                    inet_ntop(AF_INET, &ipv4, gateway, INET_ADDRSTRLEN);
                }
                else if (addr_family == AF_INET6)
                {
                    memcpy(&ipv6, (void *)RTA_DATA(tb[NHA_GATEWAY]), 16);
                    inet_ntop(AF_INET6, &ipv6, gateway, INET6_ADDRSTRLEN);
                }
                else
                {
                    SWSS_LOG_ERROR("Unexpected nexthop address family");
                    return;
                }
            }

            if (tb[NHA_OIF])
            {
                ifindex = *((int32_t *)RTA_DATA(tb[NHA_OIF]));
                char if_name[IFNAMSIZ] = {0};
                if (!getIfName(ifindex, if_name, IFNAMSIZ))
                {
                    strcpy(if_name, ifname_unknown);
                }
                ifname = string(if_name);
                if (ifname == "eth0" || ifname == "docker0")
                {
                    SWSS_LOG_DEBUG("Skip routes to interface: %s id[%d]", ifname.c_str(), id);
                    return;
                }
            }

            SWSS_LOG_DEBUG("Received: id[%d], if[%d/%s] address[%s]", id, ifindex, ifname.c_str(), gateway);
            m_nh_groups.insert({id, NextHopGroup(id, string(gateway), ifname)});
        }
    }
    else if (nlmsg_type == RTM_DELNEXTHOP)
    {
        SWSS_LOG_DEBUG("NextHopGroup del event: %d", id);
        deleteNextHopGroup(id);
    }

    return;
}

/*
 * Handle label route
 * @arg nlmsg_type      Netlink message type
 * @arg obj             Netlink object
 */
void RouteSync::onLabelRouteMsg(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;
    struct nl_addr *daddr;
    char destaddr[MAX_ADDR_SIZE + 1] = {0};

    daddr = rtnl_route_get_dst(route_obj);
    nl_addr2str(daddr, destaddr, MAX_ADDR_SIZE);
    SWSS_LOG_INFO("Receive new LabelRoute message dest addr: %s", destaddr);
    if (nl_addr_iszero(daddr)) return;

    if (nlmsg_type == RTM_DELROUTE)
    {
        m_label_routeTable->del(destaddr);
        return;
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        SWSS_LOG_INFO("Unknown message-type: %d for LabelRoute %s", nlmsg_type, destaddr);
        return;
    }

    sendOffloadReply(route_obj);

    /* Get the index of the master device */
    uint32_t master_index = rtnl_route_get_table(route_obj);
    /* if the table_id is not set in the route obj then route is for default vrf. */
    if (master_index)
    {
        SWSS_LOG_INFO("Unsupported Non-default VRF: %d for LabelRoute %s",
                      master_index, destaddr);
        return;
    }

    switch (rtnl_route_get_type(route_obj))
    {
        case RTN_BLACKHOLE:
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple fv("blackhole", "true");
            fvVector.push_back(fv);
            m_label_routeTable->set(destaddr, fvVector);
            return;
        }
        case RTN_UNICAST:
            break;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            SWSS_LOG_INFO("BUM routes aren't supported yet (%s)", destaddr);
            return;

        default:
            return;
    }

    struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
    if (!nhs)
    {
        SWSS_LOG_INFO("Nexthop list is empty for LabelRoute %s", destaddr);
        return;
    }

    /* Get nexthop lists */
    string gw_list;
    string intf_list;
    string mpls_list;
    getNextHopList(route_obj, gw_list, mpls_list, intf_list);

    vector<FieldValueTuple> fvVector;
    FieldValueTuple gw("nexthop", gw_list);
    FieldValueTuple intf("ifname", intf_list);
    FieldValueTuple mpls_pop("mpls_pop", "1");

    fvVector.push_back(gw);
    fvVector.push_back(intf);
    if (!mpls_list.empty())
    {
        FieldValueTuple mpls_nh("mpls_nh", mpls_list);
        fvVector.push_back(mpls_nh);
    }
    fvVector.push_back(mpls_pop);

    m_label_routeTable->set(destaddr, fvVector);
    SWSS_LOG_INFO("LabelRouteTable set msg: %s %s %s %s", destaddr,
                  gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());
}

/*
 * Handle vnet route 
 * @arg nlmsg_type      Netlink message type
 * @arg obj             Netlink object
 * @arg vnet            Vnet name
 */     
void RouteSync::onVnetRouteMsg(int nlmsg_type, struct nl_object *obj, string vnet)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;

    /* Get the destination IP prefix */
    struct nl_addr *dip = rtnl_route_get_dst(route_obj);
    char destipprefix[MAX_ADDR_SIZE + 1] = {0};
    nl_addr2str(dip, destipprefix, MAX_ADDR_SIZE);

    string vnet_dip =  vnet + string(":") + destipprefix;
    SWSS_LOG_DEBUG("Receive new vnet route message %s", vnet_dip.c_str());

    /* Ignore IPv6 link-local and mc addresses as Vnet routes */
    auto family = rtnl_route_get_family(route_obj);
    if (family == AF_INET6 &&
       (IN6_IS_ADDR_LINKLOCAL(nl_addr_get_binary_addr(dip)) || IN6_IS_ADDR_MULTICAST(nl_addr_get_binary_addr(dip))))
    {
        SWSS_LOG_INFO("Ignore linklocal vnet routes %d for %s", nlmsg_type, vnet_dip.c_str());
        return;
    }

    if (nlmsg_type == RTM_DELROUTE)
    {
        /* Duplicated delete as we do not know if it is a VXLAN tunnel route*/
        m_vnet_routeTable.del(vnet_dip);
        m_vnet_tunnelTable.del(vnet_dip);
        return;
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        SWSS_LOG_INFO("Unknown message-type: %d for %s", nlmsg_type, vnet_dip.c_str());
        return;
    }

    sendOffloadReply(route_obj);

    switch (rtnl_route_get_type(route_obj))
    {
        case RTN_UNICAST:
            break;

        /* We may support blackhole in the future */
        case RTN_BLACKHOLE:
            SWSS_LOG_INFO("Blackhole route is supported yet (%s)", vnet_dip.c_str());
            return;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            SWSS_LOG_INFO("BUM routes aren't supported yet (%s)", vnet_dip.c_str());
            return;

        default:
            return;
    }

    struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
    if (!nhs)
    {
        SWSS_LOG_INFO("Nexthop list is empty for %s", vnet_dip.c_str());
        return;
    }

    /* Get nexthop lists */
    string nexthops = getNextHopGw(route_obj);
    string ifnames = getNextHopIf(route_obj);

    /* If the the first interface name starts with VXLAN_IF_NAME_PREFIX,
       the route is a VXLAN tunnel route. */
    if (ifnames.find(VXLAN_IF_NAME_PREFIX) == 0)
    {
        vector<FieldValueTuple> fvVector;
        FieldValueTuple ep("endpoint", nexthops);
        fvVector.push_back(ep);

        m_vnet_tunnelTable.set(vnet_dip, fvVector);
        SWSS_LOG_DEBUG("%s set msg: %s %s",
                       APP_VNET_RT_TUNNEL_TABLE_NAME, vnet_dip.c_str(), nexthops.c_str());
        return;
    }
    /* Regular VNET route */
    else
    {
        vector<FieldValueTuple> fvVector;
        FieldValueTuple idx("ifname", ifnames);
        fvVector.push_back(idx);

        /* If the route has at least one next hop gateway, e.g., nexthops does not only have ',' */
        if (nexthops.length() + 1 > (unsigned int)rtnl_route_get_nnexthops(route_obj))
        {
            FieldValueTuple nh("nexthop", nexthops);
            fvVector.push_back(nh);
            SWSS_LOG_DEBUG("%s set msg: %s %s %s",
                           APP_VNET_RT_TABLE_NAME, vnet_dip.c_str(), ifnames.c_str(), nexthops.c_str());
        }
        else
        {
            SWSS_LOG_DEBUG("%s set msg: %s %s",
                           APP_VNET_RT_TABLE_NAME, vnet_dip.c_str(), ifnames.c_str());
        }

        m_vnet_routeTable.set(vnet_dip, fvVector);
    }
}

/*
 * Get interface/VRF name based on interface/VRF index
 * @arg if_index          Interface/VRF index
 * @arg if_name           String to store interface name
 * @arg name_len          Length of destination string, including terminating zero byte
 *
 * Return true if we successfully gets the interface/VRF name.
 */
bool RouteSync::getIfName(int if_index, char *if_name, size_t name_len)
{
    if (!if_name || name_len == 0)
    {
        return false;
    }

    memset(if_name, 0, name_len);

    /* Cannot get interface name. Possibly the interface gets re-created. */
    if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
    {
        /* Trying to refill cache */
        nl_cache_refill(m_nl_sock, m_link_cache);
        if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
        {
            return false;
        }
    }

    return true;
}

rtnl_link* RouteSync::getLinkByName(const char *name)
{
    auto link = rtnl_link_get_by_name(m_link_cache, name);
    if (link == nullptr)
    {
        /* Trying to refill cache */
        nl_cache_refill(m_nl_sock ,m_link_cache);
        link = rtnl_link_get_by_name(m_link_cache, name);
    }
    return link;
}

/*
 * getNextHopList() - parses next hop list attached to route_obj
 * @arg route_obj     (input) Netlink route object
 * @arg gw_list       (output) comma-separated list of NH IP gateways
 * @arg mpls_list     (output) comma-separated list of NH MPLS info
 * @arg intf_list     (output) comma-separated list of NH interfaces
 *
 * Return void
 */
void RouteSync::getNextHopList(struct rtnl_route *route_obj, string& gw_list,
                               string& mpls_list, string& intf_list)
{
    bool mpls_found = false;

    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        struct nl_addr *addr = NULL;

        /* RTA_GATEWAY is NH gateway info for IP routes only */
        if ((addr = rtnl_route_nh_get_gateway(nexthop)))
        {
            char gw_ip[MAX_ADDR_SIZE + 1] = {0};
            nl_addr2str(addr, gw_ip, MAX_ADDR_SIZE);
            gw_list += gw_ip;

            /* LWTUNNEL_ENCAP_MPLS RTA_DST is MPLS NH label stack for IP routes only */
            if ((addr = rtnl_route_nh_get_encap_mpls_dst(nexthop)))
            {
                char labelstack[MAX_ADDR_SIZE + 1] = {0};
                nl_addr2str(addr, labelstack, MAX_ADDR_SIZE);
                mpls_list += string("push");
                mpls_list += labelstack;
                mpls_found = true;
            }
            /* Filler for proper parsing in routeorch */
            else
            {
                mpls_list += string("na");
            }
        }
        /* RTA_VIA is NH gateway info for MPLS routes only */
        else if ((addr = rtnl_route_nh_get_via(nexthop)))
        {
            char gw_ip[MAX_ADDR_SIZE + 1] = {0};
            nl_addr2str(addr, gw_ip, MAX_ADDR_SIZE);
            gw_list += gw_ip;

            /* RTA_NEWDST is MPLS NH label stack for MPLS routes only */
            if ((addr = rtnl_route_nh_get_newdst(nexthop)))
            {
                char labelstack[MAX_ADDR_SIZE + 1] = {0};
                nl_addr2str(addr, labelstack, MAX_ADDR_SIZE);
                mpls_list += string("swap");
                mpls_list += labelstack;
                mpls_found = true;
            }
            /* Filler for proper parsing in routeorch */
            else
            {
                mpls_list += string("na");
            }
        }
        else
        {
            if (rtnl_route_get_family(route_obj) == AF_INET6)
            {
                gw_list += "::";
            }
            /* for MPLS route, use IPv4 as default gateway. */
            else
            {
                gw_list += "0.0.0.0";
            }
            mpls_list += string("na");
        }

        /* Get the ID of next hop interface */
        unsigned if_index = rtnl_route_nh_get_ifindex(nexthop);
        char if_name[IFNAMSIZ] = "0";
        if (getIfName(if_index, if_name, IFNAMSIZ))
        {
            intf_list += if_name;
        }
        /* If we cannot get the interface name */
        else
        {
            intf_list += "unknown";
        }

        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            gw_list += NHG_DELIMITER;
            mpls_list += NHG_DELIMITER;
            intf_list += NHG_DELIMITER;
        }
    }

    if (!mpls_found)
    {
        mpls_list.clear();
    }
}

/*
 * Get next hop gateway IP addresses
 * @arg route_obj     route object
 *
 * Return concatenation of IP addresses: gw0 + "," + gw1 + .... + "," + gwN
 */
string RouteSync::getNextHopGw(struct rtnl_route *route_obj)
{
    string result = "";

    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        struct nl_addr *addr = rtnl_route_nh_get_gateway(nexthop);

        /* Next hop gateway is not empty */
        if (addr)
        {
            char gw_ip[MAX_ADDR_SIZE + 1] = {0};
            nl_addr2str(addr, gw_ip, MAX_ADDR_SIZE);
            result += gw_ip;
        }
        else
        {
            if (rtnl_route_get_family(route_obj) == AF_INET)
            {
                result += "0.0.0.0";
            }
            else
            {
                result += "::";
            }
        }

        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            result += NHG_DELIMITER;
        }
    }

    return result;
}

/*
 * Get next hop interface names
 * @arg route_obj     route object
 *
 * Return concatenation of interface names: if0 + "," + if1 + .... + "," + ifN
 */
string RouteSync::getNextHopIf(struct rtnl_route *route_obj)
{
    string result = "";

    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        /* Get the ID of next hop interface */
        unsigned if_index = rtnl_route_nh_get_ifindex(nexthop);
        char if_name[IFNAMSIZ] = "0";

        /* If we cannot get the interface name */
        if (!getIfName(if_index, if_name, IFNAMSIZ))
        {
            strcpy(if_name, "unknown");
        }

        result += if_name;

        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            result += NHG_DELIMITER;
        }
    }

    return result;
}

/*
 * Get next hop weights
 * @arg route_obj     route object
 *
 * Return concatenation of interface names: wt0 + "," + wt1 + .... + "," + wtN
 */
string RouteSync::getNextHopWt(struct rtnl_route *route_obj)
{
    string result = "";

    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        /* Get the weight of next hop */
        uint8_t weight = rtnl_route_nh_get_weight(nexthop);
        if (weight)
        {
            result += to_string(weight);
        }
        else
        {
            return "";
        }

        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            result += string(",");
        }
    }

    return result;
}

bool RouteSync::sendOffloadReply(struct nlmsghdr* hdr)
{
    SWSS_LOG_ENTER();

    if (hdr->nlmsg_type != RTM_NEWROUTE)
    {
        return false;
    }

    // Add request flag (required by zebra)
    hdr->nlmsg_flags |= NLM_F_REQUEST;

    rtmsg *rtm = static_cast<rtmsg*>(NLMSG_DATA(hdr));

    // Add offload flag
    rtm->rtm_flags |= RTM_F_OFFLOAD;

    if (!m_fpmInterface)
    {
        SWSS_LOG_ERROR("Cannot send offload reply to zebra: FPM is disconnected");
        return false;
    }

    // Send to zebra
    if (!m_fpmInterface->send(hdr))
    {
        SWSS_LOG_ERROR("Failed to send reply to zebra");
        return false;
    }

    return true;
}

bool RouteSync::sendOffloadReply(struct rtnl_route* route_obj)
{
    SWSS_LOG_ENTER();
    int ret = 0;

    nl_msg* msg{};
    ret = rtnl_route_build_add_request(route_obj, NLM_F_CREATE, &msg);

    if (ret !=0)
    {
        SWSS_LOG_ERROR("Route build add returned %d", ret);
        return false;
    }
    auto nlMsg = makeUniqueWithDestructor(msg, nlmsg_free);

    if (nlMsg.get() == NULL)
    {
        SWSS_LOG_ERROR("Error in allocation for sending offload reply");
        return false;
    }
    return sendOffloadReply(nlmsg_hdr(nlMsg.get()));
}

void RouteSync::setSuppressionEnabled(bool enabled)
{
    SWSS_LOG_ENTER();

    m_isSuppressionEnabled = enabled;

    SWSS_LOG_NOTICE("Pending routes suppression is %s", (m_isSuppressionEnabled ? "enabled": "disabled"));
}

void RouteSync::onRouteResponse(const std::string& key, const std::vector<FieldValueTuple>& fieldValues)
{
    IpPrefix prefix;
    std::string vrfName;
    std::string protocol;

    bool isSetOperation{false};
    bool isSuccessReply{false};

    if (!isSuppressionEnabled())
    {
        return;
    }

    auto colon = key.find(':');
    if (colon != std::string::npos && key.substr(0, colon).find(VRF_PREFIX) != std::string::npos)
    {
        vrfName = key.substr(0, colon);
        prefix = IpPrefix{key.substr(colon + 1)};
    }
    else
    {
        prefix = IpPrefix{key};
    }

    for (const auto& fieldValue: fieldValues)
    {
        std::string field = fvField(fieldValue);
        std::string value = fvValue(fieldValue);

        if (field == "err_str")
        {
            isSuccessReply = (value == "SWSS_RC_SUCCESS");
        }
        else if (field == "protocol")
        {
            // If field "protocol" is present in the field values then
            // it is a SET operation. This field is absent only if we are
            // processing DEL operation.
            isSetOperation = true;
            protocol = value;
        }
    }

    if (!isSetOperation)
    {
        SWSS_LOG_DEBUG("Received response for prefix %s(%s) deletion, ignoring ",
            prefix.to_string().c_str(), vrfName.c_str());
        return;
    }

    if (!isSuccessReply)
    {
        SWSS_LOG_INFO("Received failure response for prefix %s(%s)",
            prefix.to_string().c_str(), vrfName.c_str());
        return;
    }

    auto routeObject = makeUniqueWithDestructor(rtnl_route_alloc(), rtnl_route_put);
    auto dstAddr = makeNlAddr(prefix);

    rtnl_route_set_dst(routeObject.get(), dstAddr.get());

    auto proto = rtnl_route_str2proto(protocol.c_str());
    if (proto < 0)
    {
        proto = swss::to_uint<uint8_t>(protocol);
    }

    rtnl_route_set_protocol(routeObject.get(), static_cast<uint8_t>(proto));
    rtnl_route_set_family(routeObject.get(), prefix.isV4() ? AF_INET : AF_INET6);

    unsigned int vrfIfIndex = 0;
    if (!vrfName.empty())
    {
        auto* link = getLinkByName(vrfName.c_str());
        if (!link)
        {
            SWSS_LOG_DEBUG("Failed to find VRF when constructing response message for prefix %s(%s). "
                "This message is probably outdated", prefix.to_string().c_str(),
                vrfName.c_str());
            return;
        }
        vrfIfIndex = rtnl_link_get_ifindex(link);
    }

    rtnl_route_set_table(routeObject.get(), vrfIfIndex);

    if (!sendOffloadReply(routeObject.get()))
    {
        SWSS_LOG_ERROR("Failed to send RTM_NEWROUTE message to zebra on prefix %s(%s)",
            prefix.to_string().c_str(), vrfName.c_str());
        return;
    }

    SWSS_LOG_INFO("Sent response to zebra for prefix %s(%s)",
        prefix.to_string().c_str(), vrfName.c_str());
}

void RouteSync::sendOffloadReply(DBConnector& db, const std::string& tableName)
{
    SWSS_LOG_ENTER();

    Table routeTable{&db, tableName};

    std::vector<std::string> keys;
    routeTable.getKeys(keys);

    for (const auto& key: keys)
    {
        std::vector<FieldValueTuple> fieldValues;
        routeTable.get(key, fieldValues);
        fieldValues.emplace_back("err_str", "SWSS_RC_SUCCESS");

        onRouteResponse(key, fieldValues);
    }
}

void RouteSync::markRoutesOffloaded(swss::DBConnector& db)
{
    SWSS_LOG_ENTER();

    sendOffloadReply(db, APP_ROUTE_TABLE_NAME);
}

void RouteSync::onWarmStartEnd(DBConnector& applStateDb)
{
    SWSS_LOG_ENTER();

    if (isSuppressionEnabled())
    {
        markRoutesOffloaded(applStateDb);
    }

    if (m_warmStartHelper->inProgress())
    {
        m_warmStartHelper->reconcile();
        SWSS_LOG_NOTICE("Warm-Restart reconciliation processed.");
    }
}

/*
 * Get nexthop group key as string
 * @arg id     next hop group id
 *
 * Return nexthop group key
 */
const string RouteSync::getNextHopGroupKeyAsString(uint32_t id) const
{
    return to_string(id);
}

/*
 * update the nexthop group entry
 * @arg nh_id     nexthop group id
 *
 */
void RouteSync::installNextHopGroup(uint32_t nh_id)
{
    auto git = m_nh_groups.find(nh_id);
    if(git == m_nh_groups.end())
    {
        SWSS_LOG_ERROR("Nexthop not found: %d", nh_id);
        return;
    }

    NextHopGroup& nhg = git->second;

    if(nhg.installed)
    {
        //Nexthop group already installed
        return;
    }
    nhg.installed = true;
    updateNextHopGroupDb(nhg);
}

/*
 * delete the nexthop group entry
 * @arg nh_id     nexthop group id
 *
 */
void RouteSync::deleteNextHopGroup(uint32_t nh_id)
{
    auto git = m_nh_groups.find(nh_id);
    if(git == m_nh_groups.end())
    {
        SWSS_LOG_ERROR("Nexthop not found: %d", nh_id);
        return;
    }

    NextHopGroup& nhg = git->second;

    if(nhg.installed)
    {
        string key = getNextHopGroupKeyAsString(nh_id);
        m_nexthop_groupTable.del(key.c_str());
        SWSS_LOG_DEBUG("NextHopGroup table del: key [%s]", key.c_str());
    }
    m_nh_groups.erase(git);
}

/*
 * update the nexthop group table in database
 * @arg nhg     the nexthop group
 *
 */
void RouteSync::updateNextHopGroupDb(const NextHopGroup& nhg)
{
    vector<FieldValueTuple> fvVector;
    string nexthops;
    string ifnames;
    string weights;
    string key = getNextHopGroupKeyAsString(nhg.id);
    getNextHopGroupFields(nhg, nexthops, ifnames, weights);

    FieldValueTuple nh("nexthop", nexthops.c_str());
    FieldValueTuple ifname("ifname", ifnames.c_str());
    fvVector.push_back(nh);
    fvVector.push_back(ifname);
    if(!weights.empty())
    {
        FieldValueTuple wg("weight", weights.c_str());
        fvVector.push_back(wg);
    }
    SWSS_LOG_INFO("NextHopGroup table set: key [%s] nexthop[%s] ifname[%s] weight[%s]", key.c_str(), nexthops.c_str(), ifnames.c_str(), weights.c_str());

    m_nexthop_groupTable.set(key.c_str(), fvVector);
}

/*
 * generate the database fields.
 * @arg nhg     the nexthop group
 *
 */
void RouteSync::getNextHopGroupFields(const NextHopGroup& nhg, string& nexthops, string& ifnames, string& weights, uint8_t af /*= AF_INET*/)
{
    if(nhg.group.size() == 0)
    {
        if(!nhg.nexthop.empty())
        {
            nexthops = nhg.nexthop;
        }
        else
        {
            nexthops = af == AF_INET ? "0.0.0.0" : "::";
        }
        ifnames = nhg.intf;
    }
    else
    {
        int i = 0;
        for(const auto& nh : nhg.group)
        {
            uint32_t id = nh.first;
            auto itr = m_nh_groups.find(id);
            if(itr == m_nh_groups.end())
            {
                SWSS_LOG_ERROR("NextHop group is incomplete: %d", nhg.id);
                return;
            }

            NextHopGroup& nhgr = itr->second;
            string weight = to_string(nh.second);
            if(i)
            {
                nexthops += NHG_DELIMITER;
                ifnames += NHG_DELIMITER;
                weights += NHG_DELIMITER;
            }
            nexthops += nhgr.nexthop.empty() ? (af == AF_INET ? "0.0.0.0" : "::") : nhgr.nexthop;
            ifnames += nhgr.intf;
            weights += weight;
            ++i;
        }
    }
}