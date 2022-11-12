#include "ipaddress.h"
#include "ipprefix.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

using namespace swss;

#define NLMSG_TAIL(nmsg) \
    (reinterpret_cast<struct rtattr *>(static_cast<void *>((((uint8_t *)nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len))))

/* Values copied from fpmsyncd/routesync.cpp */
#define NH_ENCAP_SRV6_ROUTE         101
#define NH_ENCAP_SRV6_LOCAL_SID     102

enum {  /* Values copied from fpmsyncd/routesync.cpp */
    SRV6_ROUTE_UNSPEC            = 0,
    SRV6_ROUTE_VPN_SID           = 1,
    SRV6_ROUTE_ENCAP_SRC_ADDR    = 2,
};

enum srv6_localsid_action {  /* Values copied from fpmsyncd/routesync.cpp */
    SRV6_LOCALSID_ACTION_UNSPEC       = 0,
    SRV6_LOCALSID_ACTION_END          = 1,
    SRV6_LOCALSID_ACTION_END_X        = 2,
    SRV6_LOCALSID_ACTION_END_T        = 3,
    SRV6_LOCALSID_ACTION_END_DX2      = 4,
    SRV6_LOCALSID_ACTION_END_DX6      = 5,
    SRV6_LOCALSID_ACTION_END_DX4      = 6,
    SRV6_LOCALSID_ACTION_END_DT6      = 7,
    SRV6_LOCALSID_ACTION_END_DT4      = 8,
    SRV6_LOCALSID_ACTION_END_B6       = 9,
    SRV6_LOCALSID_ACTION_END_B6_ENCAP = 10,
    SRV6_LOCALSID_ACTION_END_BM       = 11,
    SRV6_LOCALSID_ACTION_END_S        = 12,
    SRV6_LOCALSID_ACTION_END_AS       = 13,
    SRV6_LOCALSID_ACTION_END_AM       = 14,
    SRV6_LOCALSID_ACTION_END_BPF      = 15,
    SRV6_LOCALSID_ACTION_END_DT46     = 16,
    SRV6_LOCALSID_ACTION_UDT4         = 100,
    SRV6_LOCALSID_ACTION_UDT6         = 101,
    SRV6_LOCALSID_ACTION_UDT46        = 102,
};

enum {  /* Values copied from fpmsyncd/routesync.cpp */
    SRV6_LOCALSID_UNSPEC         = 0,
    SRV6_LOCALSID_ACTION         = 1,
    SRV6_LOCALSID_SRH            = 2,
    SRV6_LOCALSID_TABLE          = 3,
    SRV6_LOCALSID_NH4            = 4,
    SRV6_LOCALSID_NH6            = 5,
    SRV6_LOCALSID_IIF            = 6,
    SRV6_LOCALSID_OIF            = 7,
    SRV6_LOCALSID_BPF            = 8,
    SRV6_LOCALSID_VRFTABLE       = 9,
    SRV6_LOCALSID_COUNTERS       = 10,
    SRV6_LOCALSID_VRFNAME        = 100,
    SRV6_LOCALSID_FORMAT         = 101,
};

enum {  /* Values copied from fpmsyncd/routesync.cpp */
    SRV6_LOCALSID_FORMAT_UNSPEC         = 0,
    SRV6_LOCALSID_FORMAT_BLOCK_LEN      = 1,
    SRV6_LOCALSID_FORMAT_NODE_LEN       = 2,
    SRV6_LOCALSID_FORMAT_FUNC_LEN       = 3,
    SRV6_LOCALSID_FORMAT_ARG_LEN        = 4,
};

namespace ut_fpmsyncd
{
    struct nlmsg
    {
        struct nlmsghdr n;
        struct rtmsg r;
        char buf[512];
    };

    /* Add a unspecific attribute to netlink message */
    bool nl_attr_put(struct nlmsghdr *n, unsigned int maxlen, int type,
                     const void *data, unsigned int alen);
    /* Add 8 bit integer attribute to netlink message */
    bool nl_attr_put8(struct nlmsghdr *n, unsigned int maxlen, int type,
                      uint16_t data);
    /* Add 16 bit integer attribute to netlink message */
    bool nl_attr_put16(struct nlmsghdr *n, unsigned int maxlen, int type,
                       uint16_t data);
    /* Add 32 bit integer attribute to netlink message */
    bool nl_attr_put32(struct nlmsghdr *n, unsigned int maxlen, int type,
                       uint32_t data);
    /* Start a new level of nested attributes */
    struct rtattr *nl_attr_nest(struct nlmsghdr *n, unsigned int maxlen, int type);
    /* Finalize nesting of attributes */
    int nl_attr_nest_end(struct nlmsghdr *n, struct rtattr *nest);
    /* Build a Netlink object containing an SRv6 Steering Route */
    struct nlmsg *create_srv6_steer_route_nlmsg(uint16_t cmd, IpPrefix *dst, IpAddress *encap_src_addr,
                                                IpAddress *vpn_sid, uint16_t table_id = 10);
    /* Build a Netlink object containing an SRv6 Local SID */
    struct nlmsg *create_srv6_localsid_nlmsg(uint16_t cmd, IpAddress *localsid, uint8_t block_len,
                                             uint8_t node_len, uint8_t func_len, uint8_t arg_len,
                                             uint32_t action, char *vrf, uint16_t table_id = 10);
    /* Free the memory allocated for a Netlink object */
    inline void free_nlobj(struct nlmsg *msg)
    {
        free(msg);
    }
}