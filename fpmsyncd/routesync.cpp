#include <netlink/route/link.h>
#include <netlink/route/route.h>
#include <netlink/route/nexthop.h>
#include "logger.h"
#include "select.h"
#include "netmsg.h"
#include "ipprefix.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"
#include "macaddress.h"
#include "converter.h"
#include <string.h>
#include <arpa/inet.h>
#include <yaml-cpp/yaml.h>

using namespace std;
using namespace swss;

#define VXLAN_IF_NAME_PREFIX    "Brvxlan"
#define VNET_PREFIX             "Vnet"
#define VRF_PREFIX              "Vrf"
#define MGMT_VRF_PREFIX         "mgmt"

#define NHG_DELIMITER ','

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

#ifndef NDA_RTA
#define NDA_RTA(r)                                                             \
    ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#endif

#define VXLAN_VNI             0
#define VXLAN_RMAC            1
#define NH_ENCAP_VXLAN      100


#define IPV4_MAX_BYTE       4
#define IPV6_MAX_BYTE      16
#define IPV4_MAX_BITLEN    32
#define IPV6_MAX_BITLEN    128

#define ETHER_ADDR_STRLEN (3*ETH_ALEN)

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


RouteSync::RouteSync(RedisPipeline *pipeline) :
    m_routeTable(pipeline, APP_ROUTE_TABLE_NAME, true),
    m_label_routeTable(pipeline, APP_LABEL_ROUTE_TABLE_NAME, true),
    m_vnet_routeTable(pipeline, APP_VNET_RT_TABLE_NAME, true),
    m_vnet_tunnelTable(pipeline, APP_VNET_RT_TUNNEL_TABLE_NAME, true),
    m_warmStartHelper(pipeline, &m_routeTable, APP_ROUTE_TABLE_NAME, "bgp", "bgp"),
    m_nl_sock(NULL), m_link_cache(NULL)
{
    m_nl_sock = nl_socket_alloc();
    nl_connect(m_nl_sock, NETLINK_ROUTE);
    rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);

    YAML::Node root;
    try
    {
        root = YAML::LoadFile("/etc/sonic/constants.yml");
        route_tag_not_to_appdb = root["constants"]["bgp"]["route_do_not_send_appdb_tag"].as<int>();
        route_tag_fallback_to_default_route = root["constants"]["bgp"]["route_eligible_for_fallback_to_default_tag"].as<int>();
    }
    catch (const exception &e)
    {
        cout << "Exception \"" << e.what() << "\" had been thrown in daemon in loading constants.yml" << endl;
        route_tag_not_to_appdb = 0xffffffff;
        route_tag_fallback_to_default_route = 0xffffffff;
    }
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
    bool warmRestartInProgress = m_warmStartHelper.inProgress();

    if (nlmsg_type == RTM_DELROUTE)
    {
        if (!warmRestartInProgress)
        {
            m_routeTable.del(destipprefix);
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
            m_warmStartHelper.insertRefreshMap(kfv);
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
        m_routeTable.set(destipprefix, fvVector);
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
        m_warmStartHelper.insertRefreshMap(kfv);
    }
    return;
}

void RouteSync::onMsgRaw(struct nlmsghdr *h)
{
    int len;

    if ((h->nlmsg_type != RTM_NEWROUTE)
        && (h->nlmsg_type != RTM_DELROUTE))
        return;
    /* Length validity. */
    len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(struct ndmsg)));
    if (len < 0) 
    {
        SWSS_LOG_ERROR("%s: Message received from netlink is of a broken size %d %zu",
            __PRETTY_FUNCTION__, h->nlmsg_len,
            (size_t)NLMSG_LENGTH(sizeof(struct ndmsg)));
        return;
    }
    onEvpnRouteMsg(h, len);
    return;
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
    uint32_t tag = 0;
    bool route_eligible_for_fallback_to_default_route = false;

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

    tag = rtnl_route_get_priority(route_obj);

    if (tag == route_tag_not_to_appdb)
        return;
    else if (tag == route_tag_fallback_to_default_route)
        route_eligible_for_fallback_to_default_route = true;

    dip = rtnl_route_get_dst(route_obj);
    nl_addr2str(dip, destipprefix + strlen(destipprefix), MAX_ADDR_SIZE);

    /*
     * Upon arrival of a delete msg we could either push the change right away,
     * or we could opt to defer it if we are going through a warm-reboot cycle.
     */
    bool warmRestartInProgress = m_warmStartHelper.inProgress();

    if (nlmsg_type == RTM_DELROUTE)
    {
        if (!warmRestartInProgress)
        {
            m_routeTable.del(destipprefix);
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
            m_warmStartHelper.insertRefreshMap(kfv);
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
            m_routeTable.set(destipprefix, fvVector);
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

    struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
    if (!nhs)
    {
        SWSS_LOG_INFO("Nexthop list is empty for %s", destipprefix);
        return;
    }

    /* Get nexthop lists */
    string gw_list;
    string intf_list;
    string mpls_list;
    getNextHopList(route_obj, gw_list, mpls_list, intf_list);
    string weights = getNextHopWt(route_obj);

    vector<string> alsv = tokenize(intf_list, NHG_DELIMITER);
    for (auto alias : alsv)
    {
        /*
         * An FRR behavior change from 7.2 to 7.5 makes FRR update default route to eth0 in interface
         * up/down events. Skipping routes to eth0 or docker0 to avoid such behavior
         */
        if (alias == "eth0" || alias == "docker0")
        {
            SWSS_LOG_DEBUG("Skip routes to eth0 or docker0: %s %s %s",
                    destipprefix, gw_list.c_str(), intf_list.c_str());
            // If intf_list has only this interface, that means all of the next hops of this route 
            // have been removed and the next hop on the eth0/docker0 has become the only next hop. 
            // In this case since we do not want the route with next hop on eth0/docker0, we return. 
            // But still we need to clear the route from the APPL_DB. Otherwise the APPL_DB and data 
            // path will be left with stale route entry
            if(alsv.size() == 1)
            {
                if (!warmRestartInProgress)
                {
                    SWSS_LOG_NOTICE("RouteTable del msg for route with only one nh on eth0/docker0: %s %s %s %s",
                            destipprefix, gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());

                    m_routeTable.del(destipprefix);
                }
                else
                {
                    SWSS_LOG_NOTICE("Warm-Restart mode: Receiving delete msg for route with only nh on eth0/docker0: %s %s %s %s",
                            destipprefix, gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());

                    vector<FieldValueTuple> fvVector;
                    const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                                       DEL_COMMAND,
                                                                       fvVector);
                    m_warmStartHelper.insertRefreshMap(kfv);
                }
            }
            return;
        }
    }

    auto proto_num = rtnl_route_get_protocol(route_obj);
    auto proto_str = getProtocolString(proto_num);

    vector<FieldValueTuple> fvVector;
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

    if (route_eligible_for_fallback_to_default_route)
    {
        FieldValueTuple tag("fallback_to_default_route", "true");
        fvVector.push_back(tag);
    }

    if (!warmRestartInProgress)
    {
        m_routeTable.set(destipprefix, fvVector);
        SWSS_LOG_DEBUG("RouteTable set msg: %s %s %s %s", destipprefix,
                       gw_list.c_str(), intf_list.c_str(), mpls_list.c_str());
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
        m_warmStartHelper.insertRefreshMap(kfv);
    }
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
        m_label_routeTable.del(destaddr);
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
            m_label_routeTable.set(destaddr, fvVector);
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

    m_label_routeTable.set(destaddr, fvVector);
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

    nl_msg* msg{};
    rtnl_route_build_add_request(route_obj, NLM_F_CREATE, &msg);

    auto nlMsg = makeUniqueWithDestructor(msg, nlmsg_free);

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

    if (m_warmStartHelper.inProgress())
    {
        m_warmStartHelper.reconcile();
        SWSS_LOG_NOTICE("Warm-Restart reconciliation processed.");
    }
}
