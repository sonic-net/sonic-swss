/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <assert.h>
#include "errfpmroute.h"
#include "fpm/fpm.h"
#include "schema.h"

using namespace swss;
using namespace std;
ErrFpmRoute::ErrFpmRoute(DBConnector *db): 
    routeErrorListener(NULL),
    cfgTrigger(db, CFG_BGP_ERROR_TABLE_NAME)
{
	SWSS_LOG_ENTER();
}

void ErrFpmRoute::setFd(int sock_id)
{
	m_conn_socket = sock_id;	
}
void ErrFpmRoute::sendMsg(IpPrefix ip_prefix,  IpAddresses ip_addresses, string alias, bool blackhole, RouteSync &sync, string  strRc)
{
    unsigned char 		sendBuffer[2048] = {0}; 
    unsigned char  	    addrlen;
    ssize_t 			sentBytes = 0;
    fpm_msg_hdr_t 	    *hdr;
    uint32_t 			msg_len, ifIndex;
    uint16_t 			msg_len_short;
    fpm_netlink_msg_t   *nlmsg;
    fpm_custom_msg_t    *cust_msg;
    bool 			    isIpV4 = ip_prefix.isV4();
    uint8_t             succeeded = 0;
    SWSS_LOG_ENTER();

    /* The message starts with fpm_msg_hdr_t, then we have fpm_custom_msg_t. 
       Later fpm_netlink_msg_t is present. There we add attributes(struct rtattr)  
       using function addattr_l() */
    hdr = (fpm_msg_hdr_t *)sendBuffer;
    hdr->version = FPM_PROTO_VERSION;
    hdr->msg_type = FPM_MSG_TYPE_NETLINK;
    cust_msg = (fpm_custom_msg_t  *)fpm_msg_data(hdr);
    if((strRc == "SWSS_RC_SUCCESS") || (strRc == "SWSS_RC_EXISTS"))
        succeeded = 1;
    cust_msg->status = succeeded;
    nlmsg = (fpm_netlink_msg_t  *)((char *)fpm_msg_data(hdr) + FPM_CUSTOM_MSG_LEN);

    addrlen = isIpV4 ? 4 :16;

    nlmsg->n.nlmsg_len 	= NLMSG_LENGTH(sizeof(struct rtmsg));
    nlmsg->n.nlmsg_flags 	= NLM_F_CREATE | NLM_F_REQUEST;
    nlmsg->n.nlmsg_type 	= RTM_NEWROUTE;
    nlmsg->r.rtm_family 	= isIpV4 ? AF_INET : AF_INET6;
    nlmsg->r.rtm_table 	= 0; /* VRF_DEFAULT_INTERNAL from Zebra, which is 0 */
    nlmsg->r.rtm_dst_len 	= (unsigned char) ip_prefix.getMaskLength();
    nlmsg->r.rtm_protocol 	= RTPROT_UNSPEC;
    nlmsg->r.rtm_scope 	= RT_SCOPE_UNIVERSE;

    /* For blackhole nexthop, rtm_type can be RTN_BLACKHOLE/RTN_PROHIBIT/RTN_UNREACHABLE */
    nlmsg->r.rtm_type 	= (true == blackhole) ? RTN_BLACKHOLE : RTN_UNICAST; 
    addattr_l(&nlmsg->n, sizeof(sendBuffer), RTA_DST,  ip_prefix.getIp().getV6Addr(), addrlen);

    set<IpAddress> nexthop_ips= ip_addresses.getIpAddresses();

    if( ip_addresses.getSize() == 1)
    {
        auto it = nexthop_ips.begin();
        const IpAddress& ia = *it;
        addattr_l(&nlmsg->n, sizeof(sendBuffer), RTA_GATEWAY, ia.getV6Addr(), addrlen);
        ifIndex = sync.getIfIndex(alias.c_str());
        addattr_l(&nlmsg->n, sizeof(sendBuffer), RTA_OIF, &ifIndex, sizeof(uint32_t));
    }
    else
    {
        /*
         * Multipath case.
         */
        char  buf[1024] = {0};
        char  cstr[512] = {0}, *token;
        struct rtattr *rta = (struct rtattr *)buf;
        struct rtnexthop *rtnh;

        rta->rta_type = RTA_MULTIPATH;
        rta->rta_len = RTA_LENGTH(0);
        rtnh = (struct rtnexthop *)RTA_DATA(rta);

        strcpy (cstr, alias.c_str());
        token = strtok (cstr,",");

        for (auto it = nexthop_ips.begin(); ((it != nexthop_ips.end()) && (token != NULL)); ++it)
        {
            const IpAddress& ia = *it;
            rtnh->rtnh_len = sizeof(*rtnh);
            rtnh->rtnh_flags = 0;
            rtnh->rtnh_hops = 0;
            rta->rta_len =  (unsigned short)(rta->rta_len + rtnh->rtnh_len);
            rta_addattr_l(rta, sizeof(buf), RTA_GATEWAY, ia.getV6Addr(), addrlen);
            rtnh->rtnh_len = (unsigned short)(rtnh->rtnh_len + sizeof(struct rtattr) + addrlen);

            ifIndex= sync.getIfIndex(token);
            rtnh->rtnh_ifindex = (int)ifIndex;
            rtnh = RTNH_NEXT(rtnh);
            token = strtok(NULL,",");
        }

        assert(rta->rta_len > RTA_LENGTH(0));
        addattr_l(&nlmsg->n, sizeof(sendBuffer),  RTA_MULTIPATH, RTA_DATA(rta), (unsigned int)RTA_PAYLOAD(rta));		
    }


    msg_len = (uint32_t)(nlmsg->n.nlmsg_len + FPM_CUSTOM_MSG_LEN);
    assert(fpm_msg_align(msg_len) == msg_len);
    msg_len = (uint32_t)fpm_data_len_to_msg_len(msg_len);	
    msg_len_short = (uint16_t)msg_len;
    hdr->msg_len = htons(msg_len_short);
    sentBytes = ::send(m_conn_socket , sendBuffer , msg_len, 0 );
    SWSS_LOG_NOTICE("sentBytes=%ld, errno=%d", sentBytes, errno);
    return;
}

/* The below function sends implicit ACK during warm-reboot. 
   During warm-reboot, if there are any emtries in APP_DB which match, the entries 
   sent by BGP, fpmsyncd will ignore these entries due to mark and sweep approach.
   But if error-handling is enabled, BGP is waiting for ACK for these entries.
   Hence, we are sending implicit positive ACK for these entries. */
void ErrFpmRoute::sendImplicitAck(RouteSync &sync)
{
	//No need to send ACK when there is no subscription
	if(!routeErrorListener)
	{
	    sync.dbExistVector.clear();
		SWSS_LOG_NOTICE("error-handling feature is disabled, returning");
		return;
	}
	for (auto &entry: sync.dbExistVector)
	{
		 std::string key = kfvKey(entry);
		 std::string op = kfvOp(entry);
		 IpPrefix ip_prefix = IpPrefix(key);
		 IpAddresses ip_addresses;
                 string alias;
		 bool blackhole = false;
		 SWSS_LOG_NOTICE("key=%s, operation=%s\n", key.c_str(), op.c_str());
		 for (auto iter : kfvFieldsValues(entry))
		 {
		 	if (fvField(iter) == "blackhole")
				blackhole = true;
			if (fvField(iter) == "nexthop")
				 ip_addresses = IpAddresses(fvValue(iter));
			if (fvField(iter) == "ifname")
				 alias = fvValue(iter);
			
		 }
		 /* Currently support only BGP routes */
		 if(false == blackhole)
		 	sendMsg(ip_prefix,  ip_addresses, alias, false, sync, "SWSS_RC_SUCCESS");
	}
	sync.dbExistVector.clear();
	return;
}

int ErrFpmRoute::addattr_l(struct nlmsghdr *n, unsigned int maxlen, int type,
          const void *data, unsigned int alen)
{
    int len;
    struct rtattr *rta;

    len = (int)RTA_LENGTH(alen);

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
        return -1;

    rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;

    if (data)
        memcpy(RTA_DATA(rta), data, alen);
    else
        assert(alen == 0);

    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);

    return 0;
}
int ErrFpmRoute::rta_addattr_l(struct rtattr *rta, unsigned int maxlen, int type,
          const void *data, unsigned int alen)
{
    unsigned int len;
    struct rtattr *subrta;

    len = (unsigned int)RTA_LENGTH(alen);

    if (RTA_ALIGN(rta->rta_len) + RTA_ALIGN(len) > maxlen)
        return -1;

    subrta = (struct rtattr *)(((char *)rta) + RTA_ALIGN(rta->rta_len));
    subrta->rta_type = (unsigned short)type;
    subrta->rta_len = (unsigned short)len;

    if (data)
        memcpy(RTA_DATA(subrta), data, alen);
    else
        assert(alen == 0);

    rta->rta_len = (unsigned short)(NLMSG_ALIGN(rta->rta_len) + RTA_ALIGN(len));

    return 0;
}

