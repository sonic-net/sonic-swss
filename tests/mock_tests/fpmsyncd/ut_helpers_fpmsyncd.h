#include "ipaddress.h"
#include "ipprefix.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

using namespace swss;

#define NLMSG_TAIL(nmsg) \
    (reinterpret_cast<struct rtattr *>(static_cast<void *>((((uint8_t *)nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len))))

/* Values copied from fpmsyncd/fpmlink.h */
#define RTM_NEWSRV6LOCALSID		1000
#define RTM_DELSRV6LOCALSID		1001

/* Values copied from fpmsyncd/routesync.cpp */
#define NH_ENCAP_SRV6_ROUTE         101

enum {  /* Values copied from fpmsyncd/routesync.cpp */
    ROUTE_ENCAP_SRV6_UNSPEC            = 0,
    ROUTE_ENCAP_SRV6_VPN_SID           = 1,
    ROUTE_ENCAP_SRV6_ENCAP_SRC_ADDR    = 2,
    ROUTE_ENCAP_SRV6_PIC_ID            = 3,
    ROUTE_ENCAP_SRV6_NH_ID             = 4,
};

enum srv6_localsid_action {  /* Values copied from fpmsyncd/routesync.cpp */
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

enum {  /* Values copied from fpmsyncd/routesync.cpp */
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

enum {  /* Values copied from fpmsyncd/routesync.cpp */
	SRV6_LOCALSID_FORMAT_UNSPEC			= 0,
	SRV6_LOCALSID_FORMAT_BLOCK_LEN		= 1,
	SRV6_LOCALSID_FORMAT_NODE_LEN		= 2,
	SRV6_LOCALSID_FORMAT_FUNC_LEN		= 3,
	SRV6_LOCALSID_FORMAT_ARG_LEN		= 4,
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
    /* Build a Netlink object containing an SRv6 VPN Route */
    struct nlmsg *create_srv6_vpn_route_nlmsg(uint16_t cmd, IpPrefix *dst, IpAddress *encap_src_addr,
                                              IpAddress *vpn_sid, uint16_t table_id = 10, uint8_t prefixlen = 0,
											  uint8_t address_family = 0, uint8_t rtm_type = 0,
											  uint32_t nhg_id = 0, uint32_t pic_id = 0);
    /* Build a Netlink object containing an SRv6 My SID */
    struct nlmsg *create_srv6_mysid_nlmsg(uint16_t cmd, IpAddress *mysid, int8_t block_len,
                                             int8_t node_len, int8_t func_len, int8_t arg_len,
                                             uint32_t action, char *vrf = NULL, IpAddress *nh = NULL,
											 uint16_t table_id = 10, uint8_t prefixlen = 0,
											 uint8_t address_family = 0);
    /* Free the memory allocated for a Netlink object */
    inline void free_nlobj(struct nlmsg *msg)
    {
        free(msg);
    }
}