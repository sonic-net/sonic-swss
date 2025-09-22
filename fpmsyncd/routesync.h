#ifndef __ROUTESYNC__
#define __ROUTESYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "zmqclient.h"
#include "zmqproducerstatetable.h"
#include "netmsg.h"
#include "linkcache.h"
#include "fpminterface.h"
#include "warmRestartHelper.h"
#include <string.h>
#include <bits/stdc++.h>
#include <linux/version.h>

#include <netlink/route/route.h>

// Add RTM_F_OFFLOAD define if it is not there.
// Debian buster does not provide one but it is neccessary for compilation.
#ifndef RTM_F_OFFLOAD
#define RTM_F_OFFLOAD 0x4000 /* route is offloaded */
#endif

using namespace std;

/* Parse the Raw netlink msg */
extern void netlink_parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta,
                                                int len);

namespace swss {

struct NextHopGroup {
    uint32_t id;
    vector<pair<uint32_t,uint8_t>> group;
    string nexthop;
    string intf;
    bool installed;
    NextHopGroup(uint32_t id, const string& nexthop, const string& interface) : installed(false), id(id), nexthop(nexthop), intf(interface) {};
    NextHopGroup(uint32_t id, const vector<pair<uint32_t,uint8_t>>& group) : installed(false), id(id), group(group) {};
};

/* Path to protocol name database provided by iproute2 */
constexpr auto DefaultRtProtoPath = "/etc/iproute2/rt_protos";

class FieldValueTupleWrapperBase {
    public:
    FieldValueTupleWrapperBase(const string & _key) : key(_key) {}
    FieldValueTupleWrapperBase(const string && _key) : key(std::move(_key)) {}
    virtual ~FieldValueTupleWrapperBase() = default;

    virtual vector<FieldValueTuple> fieldValueTupleVector() = 0;

    vector<KeyOpFieldsValuesTuple> KeyOpFieldsValuesTupleVector() {
        vector<KeyOpFieldsValuesTuple> kfvVector;
        kfvVector.push_back(KeyOpFieldsValuesTuple {key.c_str(), "DEL", {}});
        auto fvVector = fieldValueTupleVector();
        kfvVector.push_back(KeyOpFieldsValuesTuple {key.c_str(), "SET", fvVector});
        return kfvVector;
    }

    // For DEL-only operations with warm restart support
    vector<KeyOpFieldsValuesTuple> KeyOpFieldsValuesTupleVectorForDel() {
        vector<KeyOpFieldsValuesTuple> kfvVector;
        kfvVector.push_back(KeyOpFieldsValuesTuple {key.c_str(), "DEL", {}});
        return kfvVector;
    }

    string key = string();
};

class RouteTableFieldValueTupleWrapper : public FieldValueTupleWrapperBase {
    public:
    RouteTableFieldValueTupleWrapper(const string & _key, string && _protocol) :
          FieldValueTupleWrapperBase(_key), protocol(std::move(_protocol)) {}
    RouteTableFieldValueTupleWrapper(const string && _key, string && _protocol) :
          FieldValueTupleWrapperBase(std::move(_key)), protocol(std::move(_protocol)) {}

    vector<FieldValueTuple> fieldValueTupleVector() override;

    string protocol = string();
    string blackhole = string();
    string nexthop = string();
    string ifname = string();
    string nexthop_group = string();
    string mpls_nh = string();
    string weight = string();
    string vni_label = string();
    string router_mac = string();
    string segment = string();
    string seg_src = string();
};

class LabelRouteTableFieldValueTupleWrapper : public FieldValueTupleWrapperBase {
    public:
    LabelRouteTableFieldValueTupleWrapper(const string & _key, string && _protocol) :
        FieldValueTupleWrapperBase(_key),
        protocol(std::move(_protocol)) {}
    LabelRouteTableFieldValueTupleWrapper(const string && _key, string && _protocol) :
        FieldValueTupleWrapperBase(std::move(_key)),
        protocol(std::move(_protocol)) {}

    vector<FieldValueTuple> fieldValueTupleVector() override;

    string protocol = string();
    string blackhole = string();
    string nexthop = string();
    string ifname = string();
    string mpls_nh = string();
    string mpls_pop = string();
};

class VnetRouteTableFieldValueTupleWrapper : public FieldValueTupleWrapperBase {
    public:
    VnetRouteTableFieldValueTupleWrapper(const string & _key) : FieldValueTupleWrapperBase(_key) {}
    VnetRouteTableFieldValueTupleWrapper(const string && _key)
        : FieldValueTupleWrapperBase(std::move(_key)) {}

    vector<FieldValueTuple> fieldValueTupleVector() override;

    string nexthop = string();
    string ifname = string();
};

class VnetTunnelTableFieldValueTupleWrapper : public FieldValueTupleWrapperBase {
    public:
    VnetTunnelTableFieldValueTupleWrapper(const string & _key) : FieldValueTupleWrapperBase(_key) {}
    VnetTunnelTableFieldValueTupleWrapper(const string && _key)
        : FieldValueTupleWrapperBase(std::move(_key)) {}

    vector<FieldValueTuple> fieldValueTupleVector() override;

    string endpoint = string();
};

class NextHopGroupTableFieldValueTupleWrapper : public FieldValueTupleWrapperBase {
    public:
    NextHopGroupTableFieldValueTupleWrapper(const string & _key) : FieldValueTupleWrapperBase(_key) {}
    NextHopGroupTableFieldValueTupleWrapper(const string && _key)
        : FieldValueTupleWrapperBase(std::move(_key)) {}

    vector<FieldValueTuple> fieldValueTupleVector() override;

    string nexthop = string();
    string ifname = string();
    string weight = string();
};

class Srv6MySidTableFieldValueTupleWrapper : public FieldValueTupleWrapperBase {
    public:
    Srv6MySidTableFieldValueTupleWrapper(const string & _key) : FieldValueTupleWrapperBase(_key) {}
    Srv6MySidTableFieldValueTupleWrapper(const string && _key)
       : FieldValueTupleWrapperBase(std::move(_key)) {}

    vector<FieldValueTuple> fieldValueTupleVector() override;

    string action = string();
    string vrf = string();
    string adj = string();
};

class Srv6SidListTableFieldValueTupleWrapper : public FieldValueTupleWrapperBase {
    public:
    Srv6SidListTableFieldValueTupleWrapper(const string & _key) : FieldValueTupleWrapperBase(_key) {}
    Srv6SidListTableFieldValueTupleWrapper(const string && _key)
       : FieldValueTupleWrapperBase(std::move(_key)) {}

    vector<FieldValueTuple> fieldValueTupleVector() override;

    string path = string();
};

class RouteSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    RouteSync(RedisPipeline *pipeline);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    virtual void onMsgRaw(struct nlmsghdr *obj);

    void setSuppressionEnabled(bool enabled);

    bool isSuppressionEnabled() const
    {
        return m_isSuppressionEnabled;
    }

    /* Helper method to set route table with warm restart support */
    void setRouteWithWarmRestart(
        const std::string& key,
        const std::vector<FieldValueTuple>& fvVector,
        ProducerStateTable & table,
        const std::string& cmd = SET_COMMAND);

    void setRouteWithWarmRestart(
        RouteTableFieldValueTupleWrapper & fvw,
        ProducerStateTable & table);

    void setRouteWithWarmRestart(
        LabelRouteTableFieldValueTupleWrapper & fvw,
        ProducerStateTable & table);

    void setTable(
        VnetRouteTableFieldValueTupleWrapper & fvw,
        ProducerStateTable & table);

    void setTable(
        VnetTunnelTableFieldValueTupleWrapper & fvw,
        ProducerStateTable & table);

    void setTable(
        NextHopGroupTableFieldValueTupleWrapper & fvw,
        ProducerStateTable & table);

    void setTable(
        Srv6MySidTableFieldValueTupleWrapper & fvw,
        ProducerStateTable & table);

    void setTable(
        Srv6SidListTableFieldValueTupleWrapper & fvw,
        ProducerStateTable & table);

    // Generic method for DEL operations with warm restart support
    template<typename WrapperType>
    void delWithWarmRestart(WrapperType && fvw, ProducerStateTable & table) {
        bool warmRestartInProgress = m_warmStartHelper.inProgress();
        if (!warmRestartInProgress) {
            table.set(fvw.KeyOpFieldsValuesTupleVectorForDel());
        } else {
            m_warmStartHelper.insertRefreshMap(fvw.KeyOpFieldsValuesTupleVectorForDel()[0]);
        }
    }

    // Generic method for DEL operations (non-shared_ptr tables, no warm restart)
    template<typename WrapperType>
    void delKey(WrapperType && fvw, ProducerStateTable & table) {
        table.set(fvw.KeyOpFieldsValuesTupleVectorForDel());
    }

    void onRouteResponse(const std::string& key, const std::vector<FieldValueTuple>& fieldValues);

    void onWarmStartEnd(swss::DBConnector& applStateDb);

    /* Mark all routes from DB with offloaded flag */
    void markRoutesOffloaded(swss::DBConnector& db);

    void onFpmConnected(FpmInterface& fpm)
    {
        m_fpmInterface = &fpm;
    }

    void onFpmDisconnected()
    {
        m_fpmInterface = nullptr;
    }

    WarmStartHelper& getWarmStartHelper()
    {
        return m_warmStartHelper;
    }

private:
    /* ZMQ client */
    shared_ptr<ZmqClient> m_zmqClient;
    /* regular route table */
    shared_ptr<ProducerStateTable> m_routeTable;
    /* label route table */
    shared_ptr<ProducerStateTable> m_label_routeTable;
    /* vnet route table */
    ProducerStateTable  m_vnet_routeTable;
    /* vnet vxlan tunnel table */  
    ProducerStateTable  m_vnet_tunnelTable;
    /* Warm start helper */
    WarmStartHelper m_warmStartHelper;
    /* srv6 mySid table */
    ProducerStateTable m_srv6MySidTable; 
    /* srv6 sid list table */
    ProducerStateTable m_srv6SidListTable; 
    struct nl_cache    *m_link_cache;
    struct nl_sock     *m_nl_sock;
    /* nexthop group table */
    ProducerStateTable  m_nexthop_groupTable;
    map<uint32_t,NextHopGroup> m_nh_groups;

    bool                m_isSuppressionEnabled{false};
    FpmInterface*       m_fpmInterface {nullptr};

    /* Handle regular route (include VRF route) */
    void onRouteMsg(int nlmsg_type, struct nl_object *obj, char *vrf);

    /* Handle label route */
    void onLabelRouteMsg(int nlmsg_type, struct nl_object *obj);

    void parseEncap(struct rtattr *tb, uint32_t &encap_value, string &rmac);

    void parseEncapSrv6SteerRoute(struct rtattr *tb, string &vpn_sid, string &src_addr);

    bool parseSrv6MySid(struct rtattr *tb[], string &block_len,
                           string &node_len, string &func_len,
                           string &arg_len, string &action, string &vrf,
                           string &adj);

    bool parseSrv6MySidFormat(struct rtattr *tb, string &block_len,
                                 string &node_len, string &func_len,
                                 string &arg_len);

    void parseRtAttrNested(struct rtattr **tb, int max,
                 struct rtattr *rta);

    char *prefixMac2Str(char *mac, char *buf, int size);


    /* Handle prefix route */
    void onEvpnRouteMsg(struct nlmsghdr *h, int len);

    /* Handle routes containing an SRv6 nexthop */
    void onSrv6SteerRouteMsg(struct nlmsghdr *h, int len);

    /* Handle SRv6 MySID */
    void onSrv6MySidMsg(struct nlmsghdr *h, int len);

    /* Handle vnet route */
    void onVnetRouteMsg(int nlmsg_type, struct nl_object *obj, string vnet);

    /* Get interface name based on interface index */
    virtual bool getIfName(int if_index, char *if_name, size_t name_len);

    /* Get interface if_index based on interface name */
    rtnl_link* getLinkByName(const char *name);

    void getEvpnNextHopSep(string& nexthops, string& vni_list,  
                       string& mac_list, string& intf_list);

    void getEvpnNextHopGwIf(char *gwaddr, int vni_value,
                          string& nexthops, string& vni_list,
                          string& mac_list, string& intf_list,
                          string rmac, string vlan_id);

    virtual bool getEvpnNextHop(struct nlmsghdr *h, int received_bytes, struct rtattr *tb[],
                        string& nexthops, string& vni_list, string& mac_list,
                        string& intf_list);

    bool getSrv6SteerRouteNextHop(struct nlmsghdr *h, int received_bytes,
                        struct rtattr *tb[], string &vpn_sid, string &src_addr);

    /* Get next hop list */
    void getNextHopList(struct rtnl_route *route_obj, string& gw_list,
                        string& mpls_list, string& intf_list);

    /* Get next hop gateway IP addresses */
    string getNextHopGw(struct rtnl_route *route_obj);

    /* Get next hop interfaces */
    string getNextHopIf(struct rtnl_route *route_obj);

    /* Get next hop weights*/
    string getNextHopWt(struct rtnl_route *route_obj);

    /* Sends FPM message with RTM_F_OFFLOAD flag set to zebra */
    bool sendOffloadReply(struct nlmsghdr* hdr);

    /* Sends FPM message with RTM_F_OFFLOAD flag set to zebra */
    bool sendOffloadReply(struct rtnl_route* route_obj);

    /* Sends FPM message with RTM_F_OFFLOAD flag set for all routes in the table */
    void sendOffloadReply(swss::DBConnector& db, const std::string& table);

    /* Get encap type */
    uint16_t getEncapType(struct nlmsghdr *h);

    const char *mySidAction2Str(uint32_t action);

    /* Handle Nexthop message */
    void onNextHopMsg(struct nlmsghdr *h, int len);
    /* Get next hop group key */
    const string getNextHopGroupKeyAsString(uint32_t id) const;
    void installNextHopGroup(uint32_t nh_id);
    void deleteNextHopGroup(uint32_t nh_id);
    void updateNextHopGroupDb(const NextHopGroup& nhg);
    void getNextHopGroupFields(const NextHopGroup& nhg, string& nexthops, string& ifnames, string& weights, uint8_t af = AF_INET);
};

}

#endif
