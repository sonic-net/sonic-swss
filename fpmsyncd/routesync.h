#ifndef __ROUTESYNC__
#define __ROUTESYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartHelper.h"
#include <string.h>
#include <bits/stdc++.h>

using namespace std;

namespace swss {

class RouteSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    RouteSync(RedisPipeline *pipeline);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    virtual void onMsgRaw(struct nlmsghdr *obj);
    WarmStartHelper  m_warmStartHelper;

private:
    /* regular route table */
    ProducerStateTable  m_routeTable;
    /* vnet route table */
    ProducerStateTable  m_vnet_routeTable;
    /* vnet vxlan tunnel table */  
    ProducerStateTable  m_vnet_tunnelTable; 
    struct nl_cache    *m_link_cache;
    struct nl_sock     *m_nl_sock;

    /* Handle regular route (include VRF route) */
    void onRouteMsg(int nlmsg_type, struct nl_object *obj, char *vrf);

    /* Parse the Raw netlink msg */
    void netlink_parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta,
            int len);

    void parse_encap(struct rtattr *tb, uint32_t &encap_value, string &rmac, uint32_t &vlan);

    void netlink_parse_rtattr_nested(struct rtattr **tb, int max,
                 struct rtattr *rta);

    char *prefix_mac2str(char *mac, char *buf, int size);


    /* Handle prefix route */
    void onEvpnRouteMsg(struct nlmsghdr *h, int len);

    /* Handle vnet route */
    void onVnetRouteMsg(int nlmsg_type, struct nl_object *obj, string vnet);

    /* Get interface name based on interface index */
    bool getIfName(int if_index, char *if_name, size_t name_len);

    void getEvpnNextHopSep(string& nexthops, string& vni_list,  
                       string& mac_list, string& intf_list);

    void getEvpnNextHopGwIf(char *gwaddr, int vni_value,
                          string& nexthops, string& vni_list,
                          string& mac_list, string& intf_list,
                          string rmac, unsigned int vid);

    bool getEvpnNextHop(struct nlmsghdr *h, int received_bytes, struct rtattr *tb[],
                        string& nexthops, string& vni_list, string& mac_list,
                        string& intf_list);

    /* Get next hop gateway IP addresses */
    string getNextHopGw(struct rtnl_route *route_obj);

    /* Get next hop interfaces */
    string getNextHopIf(struct rtnl_route *route_obj);
};

}

#endif
