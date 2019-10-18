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

    WarmStartHelper  m_warmStartHelper;
    uint32_t getIfIndex(const char *);
    std::vector<KeyOpFieldsValuesTuple> dbExistVector;
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
    void onRouteMsg(int nlmsg_type, struct nl_object *obj);

    /* Handle vnet route */
    void onVnetRouteMsg(int nlmsg_type, struct nl_object *obj, string vnet);

    /* Get interface name based on interface index */
    bool getIfName(int if_index, char *if_name, size_t name_len);

    /* Get next hop gateway IP addresses */
    string getNextHopGw(struct rtnl_route *route_obj);

    /* Get next hop interfaces */
    string getNextHopIf(struct rtnl_route *route_obj);
};

}

#endif
