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

#include <string>
#include <netinet/in.h>
#include <netlink/netfilter/ct.h>
#include <netlink/utils.h>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "ipaddress.h"
#include "netmsg.h"
#include "linkcache.h"

#include "natsync.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

#define NL_IP_ADDR(addrp)        (((struct nl_ip_addr *)addrp)->a_addr)
#define IS_LOOPBACK_ADDR(ipaddr) ((ipaddr & 0xFF000000) == 0x7F000000)

#define CT_UDP_EXPIRY_TIMEOUT   600 /* Max conntrack timeout in the user configurable range */

NatSync::NatSync(RedisPipeline *pipelineAppDB, DBConnector *appDb, DBConnector *stateDb, NfNetlink *nfnl) :
    m_natTable(appDb, APP_NAT_TABLE_NAME),
    m_naptTable(appDb, APP_NAPT_TABLE_NAME),
    m_natTwiceTable(appDb, APP_NAT_TWICE_TABLE_NAME),
    m_naptTwiceTable(appDb, APP_NAPT_TWICE_TABLE_NAME),
    m_natCheckTable(appDb, APP_NAT_TABLE_NAME),
    m_naptCheckTable(appDb, APP_NAPT_TABLE_NAME),
    m_twiceNatCheckTable(appDb, APP_NAT_TWICE_TABLE_NAME),
    m_twiceNaptCheckTable(appDb, APP_NAPT_TWICE_TABLE_NAME),
    m_naptPoolCheckTable(appDb, APP_NAPT_POOL_IP_TABLE_NAME),
    m_stateNatRestoreTable(stateDb, STATE_NAT_RESTORE_TABLE_NAME)
{
    nfsock = nfnl;

    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "natsyncd", "nat", DEFAULT_NATSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_NAT_TABLE_NAME, &m_natTable);
        m_AppRestartAssist->registerAppTable(APP_NAPT_TABLE_NAME, &m_naptTable);
        m_AppRestartAssist->registerAppTable(APP_NAT_TWICE_TABLE_NAME, &m_natTwiceTable);
        m_AppRestartAssist->registerAppTable(APP_NAPT_TWICE_TABLE_NAME, &m_naptTwiceTable);
    }
}

/* To check the port init is done or not */
bool NatSync::isPortInitDone(DBConnector *app_db)
{
    bool portInit = 0;
    long cnt = 0;

    while(!portInit) {
        Table portTable(app_db, APP_PORT_TABLE_NAME);
        std::vector<FieldValueTuple> tuples;
        portInit = portTable.get("PortInitDone", tuples);

        if(portInit)
            break;
        sleep(1);
        cnt++;
    }
    sleep(5);
    SWSS_LOG_NOTICE("PORT_INIT_DONE : %d %ld", portInit, cnt);
    return portInit;
}

// Check if nat conntrack entries are restored in kernel
bool NatSync::isNatRestoreDone()
{
    SWSS_LOG_ENTER();

    string value;

    m_stateNatRestoreTable.hget("Flags", "restored", value);
    if (value == "true")
    {
        SWSS_LOG_NOTICE("Conntrack table restore for NAT entries to kernel is complete");
        return true;
    }
    return false;
}

std::string ctStatusStr(uint32_t ct_status)
{
    string ct_status_str = "";

    if (ct_status & IPS_EXPECTED)
        ct_status_str += "EXPECTED,";

    if (!(ct_status & IPS_SEEN_REPLY))
        ct_status_str += "NOREPLY,";

    if (ct_status & IPS_ASSURED)
	ct_status_str += "ASSURED,";

    if (!(ct_status & IPS_CONFIRMED))
	ct_status_str += "NOTSENT,";

    if (ct_status & IPS_SRC_NAT)
	ct_status_str += "SNAT,";

    if (ct_status & IPS_DST_NAT)
	ct_status_str += "DNAT,";

    if (ct_status & IPS_SEQ_ADJUST)
	ct_status_str += "SEQADJUST,";

    if (!(ct_status & IPS_SRC_NAT_DONE))
	ct_status_str += "SNAT_INIT,";

    if (!(ct_status & IPS_DST_NAT_DONE))
	ct_status_str += "DNAT_INIT,";

    if (ct_status & IPS_DYING)
	ct_status_str += "DYING,";

    if (ct_status & IPS_FIXED_TIMEOUT)
	ct_status_str += "FIXED_TIMEOUT";

    return ct_status_str;
}

/* Parse the valid conntrack notifications that can be added to hardware NAT table */
int NatSync::parseConnTrackMsg(const struct nfnl_ct *ct, struct naptEntry &entry)
{
    SWSS_LOG_ENTER();

    string  ct_status_str;
    char    proto_str[32] = {0};

    /* Only IPv4 family connections are handled */
    if (nfnl_ct_get_family(ct) != AF_INET)
    {
        SWSS_LOG_DEBUG("Conntrack entry protocol is not AF_INET (%d)", entry.protocol);
        return -1;
    }

    /* If the connection is not subjected to either SNAT or DNAT,
     * we are not interested in those connection entries.
     */
    entry.ct_status         = nfnl_ct_get_status(ct); 

    if (! ((entry.ct_status & IPS_SRC_NAT_DONE) || (entry.ct_status & IPS_DST_NAT_DONE)))
    {
        SWSS_LOG_DEBUG("Conntrack entry is not SNAT or DNAT");
        return -1;
    }

    entry.orig_src_ip       = NL_IP_ADDR(nfnl_ct_get_src(ct, 0));
    entry.orig_dest_ip      = NL_IP_ADDR(nfnl_ct_get_dst(ct, 0));
    entry.nat_src_ip        = NL_IP_ADDR(nfnl_ct_get_dst(ct, 1));
    entry.nat_dest_ip       = NL_IP_ADDR(nfnl_ct_get_src(ct, 1));

    /* Ignore those conntrack entries that correspond to internal loopback socket
     * connections in the system. ie., if source ip or destination ip are in 127.0.0.X.
     * Ideally, such connections would not have been subjected to SNAT/DNAT and should
     * have been ignored in the above check already.
     */ 
    if (((IS_LOOPBACK_ADDR(ntohl(entry.orig_src_ip.getV4Addr())))  &&
         (IS_LOOPBACK_ADDR(ntohl(entry.orig_dest_ip.getV4Addr())))) ||
        ((IS_LOOPBACK_ADDR(ntohl(entry.nat_src_ip.getV4Addr())))   &&
         (IS_LOOPBACK_ADDR(ntohl(entry.nat_dest_ip.getV4Addr())))))
    {
        SWSS_LOG_DEBUG("Conntrack entry is a loopback entry, ignoring it.");
        return -1;
    }

    entry.orig_src_l4_port  = nfnl_ct_get_src_port(ct, 0);
    entry.orig_dst_l4_port  = nfnl_ct_get_dst_port(ct, 0);
    entry.nat_src_l4_port   = nfnl_ct_get_dst_port(ct, 1);
    entry.nat_dst_l4_port   = nfnl_ct_get_src_port(ct, 1);

    entry.protocol          = nfnl_ct_get_proto(ct);
    entry.conntrack_id      = nfnl_ct_get_id(ct); 
    ct_status_str           = ctStatusStr(nfnl_ct_get_status(ct));

    nl_ip_proto2str(entry.protocol, proto_str, sizeof(proto_str));

    if ((entry.protocol == IPPROTO_TCP) || (entry.protocol == IPPROTO_UDP))
    {
        SWSS_LOG_INFO("Conntrack entry : protocol %s, src %s:%d, dst %s:%d, natted src %s:%d, dst %s:%d, CT status %s",
                      proto_str, entry.orig_src_ip.to_string().c_str(), entry.orig_src_l4_port,
                      entry.orig_dest_ip.to_string().c_str(), entry.orig_dst_l4_port,
                      entry.nat_src_ip.to_string().c_str(), entry.nat_src_l4_port,
                      entry.nat_dest_ip.to_string().c_str(), entry.nat_dst_l4_port, ct_status_str.c_str());
    }
    else if (entry.protocol == IPPROTO_ICMP)
    {
        /* Don't add ICMP NAT entries to hardware */
        SWSS_LOG_INFO("Conntrack entry : protocol icmp, src %s, dst %s, icmp_type %d, code %d, icmp_id %d, \
                      natted src %s, dst %s, icmp_type %d, code %d, icmp_id %d, CT status %s",
                      entry.orig_src_ip.to_string().c_str(), entry.orig_dest_ip.to_string().c_str(),
                      nfnl_ct_get_icmp_type(ct, 0), nfnl_ct_get_icmp_code(ct, 0), nfnl_ct_get_icmp_id(ct, 0),
                      entry.nat_src_ip.to_string().c_str(), entry.nat_dest_ip.to_string().c_str(),
                      nfnl_ct_get_icmp_type(ct, 1), nfnl_ct_get_icmp_code(ct, 1), nfnl_ct_get_icmp_id(ct, 1),
                      ct_status_str.c_str());

        return -1;
    }
    if (! (entry.ct_status & IPS_CONFIRMED))
    {
        SWSS_LOG_INFO("Conntrack entry is not CONFIRMED (not went out of the box, don't process them)");
        return -1;
    }
    return 0;
}

/* Process the netfiliter conntrack notifications from the kernel via netlink */
void NatSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    SWSS_LOG_ENTER();

    struct nfnl_ct *ct = (struct nfnl_ct *)obj;
    struct naptEntry      napt;
  
    nlmsg_type = NFNL_MSG_TYPE(nlmsg_type);

    SWSS_LOG_DEBUG("Conntrack entry notification, msg type :%s (%d)",
        (((nlmsg_type == IPCTNL_MSG_CT_NEW) ? "CT_NEW" : ((nlmsg_type == IPCTNL_MSG_CT_DELETE) ? "CT_DELETE" : "OTHER"))),
        nlmsg_type);

    if ((nlmsg_type != IPCTNL_MSG_CT_NEW) && (nlmsg_type != IPCTNL_MSG_CT_DELETE))
    {
        SWSS_LOG_DEBUG("Conntrack entry notification, msg type not NEW or DELETE, ignoring");
  
    }

    /* Parse the conntrack notification from the kernel */
    if (-1 == parseConnTrackMsg(ct, napt))
    {
        return;
    }

    if (nlmsg_type == IPCTNL_MSG_CT_NEW)
    {
        if ((napt.protocol == IPPROTO_TCP) && (napt.ct_status & IPS_ASSURED))
        {
            addNatEntry(ct, napt, 1);
        }
        else if (napt.protocol == IPPROTO_UDP)
        {
            if (0 == addNatEntry(ct, napt, 1))
            {
                if (! (napt.ct_status & IPS_ASSURED))
                {
                    /* Update the connection tracking entry status to ASSURED for UDP connection.
                     * Since application takes care of timing it out, and we don't want the kernel
                     * to age the UDP entries prematurely.
                     */
                    napt.ct_status |= (IPS_SEEN_REPLY | IPS_ASSURED);
    
                    nfnl_ct_set_status(ct, napt.ct_status);
                    nfnl_ct_set_timeout(ct, CT_UDP_EXPIRY_TIMEOUT);
    
                    updateConnTrackEntry(ct);
                }
            }
        }
    }
    else if ((nlmsg_type == IPCTNL_MSG_CT_DELETE) && (napt.ct_status & IPS_ASSURED))
    {
        /* Delete only ASSURED NAT entries from APP_DB */
        addNatEntry(ct, napt, 0);
    }
}

/* Conntrack notifications from the kernel don't have a flag to indicate if the
 * NAT is NAPT or basic NAT. The original L4 port and the translated L4 port may 
 * be the same and still can be the NAPT (can happen if the original L4 port is
 * already in the L4 port pool range). To find out if it is a case of NAPT, we
 * check if the Nat'ted IP is one of the NAPT pool range IP addresses, or if
 * there is no static or dynamic NAT for that IP address. If both checks are not
 * met, then it is the case of NAT basic translation where only IP address is NAT'ted. */
bool NatSync::matchingSnaptPoolExists(const IpAddress &natIp)
{
    string key             = natIp.to_string();
    std::vector<FieldValueTuple> values;

    if (m_naptPoolCheckTable.get(key, values))
    {
        SWSS_LOG_INFO("Matching pool IP exists for NAT IP %s", key.c_str());
        return true;
    }

    return false;
}

bool NatSync::matchingSnaptEntryExists(const naptEntry &entry)
{
    string key             = entry.orig_src_ip.to_string() + ":" + to_string(entry.orig_src_l4_port);
    string reverseEntryKey = entry.nat_src_ip.to_string() + ":" + to_string(entry.nat_src_l4_port);
    std::vector<FieldValueTuple> values;

    if (m_naptCheckTable.get(key, values) || m_naptCheckTable.get(reverseEntryKey, values))
    {
        SWSS_LOG_INFO("Matching SNAPT entry exists for key %s or reverse key %s",
                       key.c_str(), reverseEntryKey.c_str());
        return true;
    }
    return false;
}

bool NatSync::matchingDnaptEntryExists(const naptEntry &entry)
{
    string key             = entry.orig_dest_ip.to_string() + ":" + to_string(entry.orig_dst_l4_port);
    string reverseEntryKey = entry.nat_dest_ip.to_string() + ":" + to_string(entry.nat_dst_l4_port);
    std::vector<FieldValueTuple> values;

    if (m_naptCheckTable.get(key, values) || m_naptCheckTable.get(reverseEntryKey, values))
    {
        SWSS_LOG_INFO("Matching DNAPT entry exists for key %s or reverse key %s",
                       key.c_str(), reverseEntryKey.c_str());
        return true;
    }
    return false;
}

/* Add the NAT entries to APP_DB based on the criteria:
 * ----------------------------------------------------
 *   - If only source ip changed, add it as SNAT entry.
 *   - If only destination ip changed, add it as DNAT entry.
 *   - If SNAT happened and the l4 port changes or part of any dynamic pool range 
 *       or if there is matching static or dynamic NAPT entry, add it as SNAPT entry.
 *   - If DNAT happened and the l4 port changes or if there is matching static 
 *       or dynamic NAPT entry, add it as DNAPT entry.
 *   - If SNAT and DNAT happened, add it as Twice NAT entry.
 *   - If SNAPT and DNAPT conditions are met or if there is no static
 *       or dynamic Twice NAT entry, then it is a Twice NAPT entry.
 */
int NatSync::addNatEntry(struct nfnl_ct *ct, struct naptEntry &entry, bool addFlag)
{
    SWSS_LOG_ENTER();

    bool src_ip_natted   = (entry.orig_src_ip      != entry.nat_src_ip);
    bool dst_ip_natted   = (entry.orig_dest_ip     != entry.nat_dest_ip);
    bool src_port_natted = (src_ip_natted && ((entry.orig_src_l4_port != entry.nat_src_l4_port) || 
                                              (matchingSnaptPoolExists(entry.nat_src_ip)) || 
                                              (matchingSnaptEntryExists(entry))));
    bool dst_port_natted = (dst_ip_natted && ((entry.orig_dst_l4_port != entry.nat_dst_l4_port) ||
                                              (matchingDnaptEntryExists(entry))));
    bool entryExists = 0, reverseEntryExists = 0;

    SWSS_LOG_INFO("Flags: src natted %d, dst natted %d, src port natted %d, dst port natted %d", src_ip_natted, dst_ip_natted, src_port_natted, dst_port_natted);

    std::vector<FieldValueTuple> fvVector, reverseFvVector;
    string protostr = ((entry.protocol == IPPROTO_TCP) ? "TCP:" : "UDP:");
    string opStr    = ((addFlag) ? "CREATE" : "DELETE");
    string key = "", reverseEntryKey = "";

    FieldValueTuple snat_type("nat_type", "snat");
    FieldValueTuple dnat_type("nat_type", "dnat");
    FieldValueTuple dynamic_entry("entry_type", "dynamic");

    if (src_ip_natted && dst_ip_natted)
    {
        if (addFlag)
        {
            FieldValueTuple translated_src_ip("translated_src_ip", entry.nat_src_ip.to_string());
            FieldValueTuple translated_dst_ip("translated_dst_ip", entry.nat_dest_ip.to_string());
            FieldValueTuple reverse_translated_src_ip("translated_src_ip", entry.orig_dest_ip.to_string());
            FieldValueTuple reverse_translated_dst_ip("translated_dst_ip", entry.orig_src_ip.to_string());

            fvVector.push_back(dynamic_entry);
            fvVector.push_back(translated_src_ip);
            fvVector.push_back(translated_dst_ip);

            reverseFvVector.push_back(dynamic_entry);
            reverseFvVector.push_back(reverse_translated_src_ip);
            reverseFvVector.push_back(reverse_translated_dst_ip);
        }

        string tmpKey             = key + entry.orig_src_ip.to_string() + ":" + entry.orig_dest_ip.to_string();
        string tmpReverseEntryKey = reverseEntryKey + entry.nat_dest_ip.to_string() + ":" + entry.nat_src_ip.to_string();

        std::vector<FieldValueTuple> values;
        if (m_twiceNatCheckTable.get(tmpKey, values))
        {
            src_port_natted = dst_port_natted = false;

            for (auto iter : values)
            {
                /* If a matching Static Twice NAT entry exists in the APP_DB,
                 * it has higher priority than the dynamic twice nat entry. */
                if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                {
                    SWSS_LOG_INFO("Static Twice NAT %s: entry exists, not processing twice NAT entry notification", opStr.c_str());
                    if (m_AppRestartAssist->isWarmStartInProgress())
                    {
                       m_AppRestartAssist->insertToMap(APP_NAT_TWICE_TABLE_NAME, tmpKey, fvVector, (!addFlag));
                       m_AppRestartAssist->insertToMap(APP_NAT_TWICE_TABLE_NAME, tmpReverseEntryKey, reverseFvVector, (!addFlag));
                    }
                    return 1;
                }
            }
            if (addFlag)
            {
                SWSS_LOG_INFO("Twice SNAT CREATE: ignoring the duplicated Twice SNAT notification");
                return 1;
            }
        }

        if (src_port_natted || dst_port_natted)
        {
            /* Case of Twice NAPT entry, where both the SIP, DIP and
             * the L4 port(s) are NAT'ted. */
            SWSS_LOG_INFO("Twice NAPT %s conntrack notification", opStr.c_str());

            key        += protostr + entry.orig_src_ip.to_string();
            reverseEntryKey += protostr + entry.nat_dest_ip.to_string();

            string src_l4_port      = std::to_string(entry.orig_src_l4_port);
            string dst_l4_port      = std::to_string(entry.orig_dst_l4_port);
            string nat_src_l4_port  = std::to_string(entry.nat_src_l4_port);
            string nat_dst_l4_port  = std::to_string(entry.nat_dst_l4_port);

            key += ":" + src_l4_port + ":" + entry.orig_dest_ip.to_string()
                 + ":" + dst_l4_port;
            reverseEntryKey += ":" + nat_dst_l4_port + ":" + entry.nat_src_ip.to_string()
                          + ":" + nat_src_l4_port;

            std::vector<FieldValueTuple> values;
            /* If a matching Static Twice NAPT entry exists in the APP_DB,
             * it has higher priority than the dynamic twice napt entry. */
            if (m_twiceNaptCheckTable.get(key, values))
            {
                for (auto iter : values)
                {
                    if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                    {
                        SWSS_LOG_INFO("Static Twice NAPT %s: entry exists, not processing dynamic twice NAPT entry", opStr.c_str());
                        if (m_AppRestartAssist->isWarmStartInProgress())
                        {
                            m_AppRestartAssist->insertToMap(APP_NAPT_TWICE_TABLE_NAME, key, fvVector, (!addFlag));
                            m_AppRestartAssist->insertToMap(APP_NAPT_TWICE_TABLE_NAME, reverseEntryKey, reverseFvVector, (!addFlag));
                        }
                        return 1;
                    }
                }
                if (addFlag)
                {
                    SWSS_LOG_INFO("Twice SNAPT CREATE: ignoring the duplicated Twice SNAPT notification");
                    return 1;
                }
            }
            if (addFlag)
            {
                FieldValueTuple translated_src_port("translated_src_l4_port", nat_src_l4_port);
                FieldValueTuple translated_dst_port("translated_dst_l4_port", nat_dst_l4_port);
                FieldValueTuple reverse_translated_src_port("translated_src_l4_port", dst_l4_port);
                FieldValueTuple reverse_translated_dst_port("translated_dst_l4_port", src_l4_port);

                fvVector.push_back(translated_src_port);
                fvVector.push_back(translated_dst_port);
                reverseFvVector.push_back(reverse_translated_src_port);
                reverseFvVector.push_back(reverse_translated_dst_port);


                if (m_AppRestartAssist->isWarmStartInProgress())
                {
                    m_AppRestartAssist->insertToMap(APP_NAPT_TWICE_TABLE_NAME, key, fvVector, false);
                    m_AppRestartAssist->insertToMap(APP_NAPT_TWICE_TABLE_NAME, reverseEntryKey, reverseFvVector, false);
                }
                else
                {
                    m_naptTwiceTable.set(key, fvVector);
                    SWSS_LOG_NOTICE("Twice NAPT entry with key %s added to APP_DB", key.c_str());
                    m_naptTwiceTable.set(reverseEntryKey, reverseFvVector);
                    SWSS_LOG_NOTICE("Twice NAPT entry with reverse key %s added to APP_DB", reverseEntryKey.c_str());
                }
            }
            else
            {
                if (m_AppRestartAssist->isWarmStartInProgress())
                {
                    m_AppRestartAssist->insertToMap(APP_NAPT_TWICE_TABLE_NAME, key, fvVector, true);
                    m_AppRestartAssist->insertToMap(APP_NAPT_TWICE_TABLE_NAME, reverseEntryKey, reverseFvVector, true);
                }
                else
                {
                    m_naptTwiceTable.del(key);
                    SWSS_LOG_NOTICE("Twice NAPT entry with key %s deleted from APP_DB", key.c_str());
                    m_naptTwiceTable.del(reverseEntryKey);
                    SWSS_LOG_NOTICE("Twice NAPT entry with reverse key %s deleted from APP_DB", reverseEntryKey.c_str());
                }
            }
        }
        else
        { 
            /* Case of Twice NAT entry, where only the SIP, DIP are
             * NAT'ted but the port translation is not done. */
            SWSS_LOG_INFO("Twice NAT %s conntrack notification", opStr.c_str());

            key             = tmpKey;
            reverseEntryKey = tmpReverseEntryKey;

            if (addFlag)
            {
                if (m_AppRestartAssist->isWarmStartInProgress())
                {
                    m_AppRestartAssist->insertToMap(APP_NAT_TWICE_TABLE_NAME, key, fvVector, false);
                    m_AppRestartAssist->insertToMap(APP_NAT_TWICE_TABLE_NAME, reverseEntryKey, reverseFvVector, false);
                }
                else
                {
                    m_natTwiceTable.set(key, fvVector);
                    SWSS_LOG_NOTICE("Twice NAT entry with key %s added to APP_DB", key.c_str());
                    m_natTwiceTable.set(reverseEntryKey, reverseFvVector);
                    SWSS_LOG_NOTICE("Twice NAT entry with reverse key %s added to APP_DB", reverseEntryKey.c_str());
                }
            }
            else
            {
                if (m_AppRestartAssist->isWarmStartInProgress())
                {
                    m_AppRestartAssist->insertToMap(APP_NAT_TWICE_TABLE_NAME, key, fvVector, true);
                    m_AppRestartAssist->insertToMap(APP_NAT_TWICE_TABLE_NAME, reverseEntryKey, reverseFvVector, true);
                }
                else
                {
                    m_natTwiceTable.del(key);
                    SWSS_LOG_NOTICE("Twice NAT entry with key %s deleted from APP_DB", key.c_str());
                    m_natTwiceTable.del(reverseEntryKey);
                    SWSS_LOG_NOTICE("Twice NAT entry with reverse key %s deleted from APP_DB", reverseEntryKey.c_str());
                }
            }
        }
    }
    else if (src_ip_natted || dst_ip_natted)
    {
        if (src_ip_natted)
        {
            if (addFlag)
            {
                FieldValueTuple snat_translated_ip("translated_ip", entry.nat_src_ip.to_string());
                FieldValueTuple dnat_translated_ip("translated_ip", entry.orig_src_ip.to_string());

                fvVector.push_back(snat_type);
                fvVector.push_back(dynamic_entry);
                fvVector.push_back(snat_translated_ip);

                reverseFvVector.push_back(dnat_type);
                reverseFvVector.push_back(dynamic_entry);
                reverseFvVector.push_back(dnat_translated_ip);
            }

            if (src_port_natted)
            {
                key             += protostr + entry.orig_src_ip.to_string();
                reverseEntryKey += protostr + entry.nat_src_ip.to_string();

                /* Case of NAPT entry, where SIP is NAT'ted and the L4 port is NAT'ted. */
                SWSS_LOG_INFO("SNAPT %s conntrack notification", opStr.c_str());

                string src_l4_port      = std::to_string(entry.orig_src_l4_port);
                string nat_src_l4_port  = std::to_string(entry.nat_src_l4_port);

                key             += ":" + src_l4_port;
                reverseEntryKey += ":" + nat_src_l4_port;

                std::vector<FieldValueTuple> values;
                /* We check for existence of reverse nat entry in the app-db because the same dnat static entry
                 * would be reported as snat entry from the kernel if a packet that is forwarded in the kernel
                 * is matched by the iptables rules corresponding to the dnat static entry */
                if (! m_AppRestartAssist->isWarmStartInProgress())
                {
                    if ((entryExists = m_naptCheckTable.get(key, values)))
                    {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAPT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("SNAPT %s: static entry exists, not processing the NAPT notification", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("SNAPT CREATE: ignoring the duplicated SNAPT notification");
                            return 1;
                        }
                        else
                        {
                            /* Skip entry, if entry contains loopback destination address */
                            if ((IS_LOOPBACK_ADDR(ntohl(entry.orig_dest_ip.getV4Addr()))) ||
                                (IS_LOOPBACK_ADDR(ntohl(entry.nat_dest_ip.getV4Addr()))))
                            {
                                SWSS_LOG_INFO("SNAPT %s: static entry contains loopback address, ignoring the notification", opStr.c_str());
                                return 1;
                            }
                            else
                            {
                                m_naptTable.del(key);
                                SWSS_LOG_NOTICE("SNAPT entry with key %s deleted from APP_DB", key.c_str());
                            }
                        }
                    }
                    if ((reverseEntryExists = m_naptCheckTable.get(reverseEntryKey, values)))
                    {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAPT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("SNAPT %s: static reverse entry exists, not processing dynamic NAPT entry", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("SNAPT CREATE: ignoring the duplicated SNAPT notification");
                            return 1;
                        }
                        else
                        {
                            /* Skip entry, if entry contains loopback destination address */
                            if ((IS_LOOPBACK_ADDR(ntohl(entry.orig_dest_ip.getV4Addr()))) ||
                                (IS_LOOPBACK_ADDR(ntohl(entry.nat_dest_ip.getV4Addr()))))
                            {
                                SWSS_LOG_INFO("SNAPT %s: static entry contains loopback address, ignoring the notification", opStr.c_str());
                                return 1;
                            }
                            else
                            {
                                m_naptTable.del(reverseEntryKey);
                                SWSS_LOG_NOTICE("Implicit DNAPT entry with key %s deleted from APP_DB", reverseEntryKey.c_str());
                            }
                        }
                    }
                }
                if (addFlag)
                {
                    FieldValueTuple snat_translated_port("translated_l4_port", nat_src_l4_port);
                    FieldValueTuple dnat_translated_port("translated_l4_port", src_l4_port);
   
                    fvVector.push_back(snat_translated_port);
                    reverseFvVector.push_back(dnat_translated_port);
  
                    if (m_AppRestartAssist->isWarmStartInProgress())
                    {
                        m_AppRestartAssist->insertToMap(APP_NAPT_TABLE_NAME, key, fvVector, false);
                        m_AppRestartAssist->insertToMap(APP_NAPT_TABLE_NAME, reverseEntryKey, reverseFvVector, false);
                    }
                    else
                    {
                        /* Skip entry, if entry contains loopback destination address */
                        if ((IS_LOOPBACK_ADDR(ntohl(entry.orig_dest_ip.getV4Addr()))) ||
                            (IS_LOOPBACK_ADDR(ntohl(entry.nat_dest_ip.getV4Addr()))))
                        {
                            SWSS_LOG_INFO("SNAPT %s: static entry contains loopback address, ignoring the notification", opStr.c_str());
                            return 1;
                        }
                        else
                        {
                            m_naptTable.set(key, fvVector);
                            SWSS_LOG_NOTICE("SNAPT entry with key %s added to APP_DB", key.c_str());
                            m_naptTable.set(reverseEntryKey, reverseFvVector);
                            SWSS_LOG_NOTICE("Implicit DNAPT entry with key %s added to APP_DB", reverseEntryKey.c_str());
                        }
                    }
                }
            }
            else
            {
                /* Case of Basic SNAT entry, where SIP is NAT'ted but the port translation is not done. */
                SWSS_LOG_INFO("SNAT %s conntrack notification", opStr.c_str());

                key             += entry.orig_src_ip.to_string();
                reverseEntryKey += entry.nat_src_ip.to_string();

                std::vector<FieldValueTuple> values;
                if (! m_AppRestartAssist->isWarmStartInProgress())
                {
                    if ((entryExists = m_natCheckTable.get(key, values)))
                    {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("SNAT %s: static entry exists, not processing the NAT notification", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("SNAT CREATE: ignoring the duplicated notification");
                            return 1;
                        }
                        else
                        {
                            /* Skip entry, if entry contains loopback destination address */
                            if ((IS_LOOPBACK_ADDR(ntohl(entry.orig_dest_ip.getV4Addr()))) ||
                                (IS_LOOPBACK_ADDR(ntohl(entry.nat_dest_ip.getV4Addr()))))
                            {
                                SWSS_LOG_INFO("SNAT %s: static entry contains loopback address, ignoring the notification", opStr.c_str());
                                return 1;
                            }
                            else
                            {
                                m_natTable.del(key);
                                SWSS_LOG_NOTICE("SNAT entry with key %s deleted from APP_DB", key.c_str());
                            }
                        }
                    }
                    if ((reverseEntryExists = m_natCheckTable.get(reverseEntryKey, values)))
                    {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("SNAT %s: static reverse entry exists, not adding dynamic NAT entry", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("SNAT CREATE: ignoring the duplicated notification");
                            return 1;
                        }
                        else
                        {
                            /* Skip entry, if entry contains loopback destination address */
                            if ((IS_LOOPBACK_ADDR(ntohl(entry.orig_dest_ip.getV4Addr()))) ||
                                (IS_LOOPBACK_ADDR(ntohl(entry.nat_dest_ip.getV4Addr()))))
                            {
                                SWSS_LOG_INFO("SNAT %s: static entry contains loopback address, ignoring the notification", opStr.c_str());
                                return 1;
                            }
                            else
                            {
                                m_natTable.del(reverseEntryKey);
                                SWSS_LOG_NOTICE("Implicit DNAT entry with key %s deleted from APP_DB", reverseEntryKey.c_str());
                            }
                        }
                    }
                }
                if (addFlag)
                {
                    if (m_AppRestartAssist->isWarmStartInProgress())
                    {
                        m_AppRestartAssist->insertToMap(APP_NAT_TABLE_NAME, key, fvVector, false);
                        m_AppRestartAssist->insertToMap(APP_NAT_TABLE_NAME, reverseEntryKey, reverseFvVector, false);
                    }
                    else
                    {
                        /* Skip entry, if entry is src natted and dest is loopback destination address */
                        if ((IS_LOOPBACK_ADDR(ntohl(entry.orig_dest_ip.getV4Addr()))) ||
                            (IS_LOOPBACK_ADDR(ntohl(entry.nat_dest_ip.getV4Addr()))))
                        {
                            SWSS_LOG_INFO("SNAT %s: static entry contains loopback address, ignoring the notification", opStr.c_str());
                            return 1;
                        }
                        else
                        {
                            m_natTable.set(key, fvVector);
                            SWSS_LOG_NOTICE("SNAT entry with key %s added to APP_DB", key.c_str());
                            m_natTable.set(reverseEntryKey, reverseFvVector);
                            SWSS_LOG_NOTICE("Implicit DNAT entry with key %s added to APP_DB", reverseEntryKey.c_str());
                        }
                    }
                }
            }
        }
        else
        {
            if (addFlag)
            {
                FieldValueTuple dnat_translated_ip("translated_ip", entry.nat_dest_ip.to_string());
                FieldValueTuple snat_translated_ip("translated_ip", entry.orig_dest_ip.to_string());

                fvVector.push_back(dnat_type);
                fvVector.push_back(dynamic_entry);
                fvVector.push_back(dnat_translated_ip);

                reverseFvVector.push_back(snat_type);
                reverseFvVector.push_back(dynamic_entry);
                reverseFvVector.push_back(snat_translated_ip);
            }

            if (dst_port_natted)
            {
                key             += protostr + entry.orig_dest_ip.to_string();
                reverseEntryKey += protostr + entry.nat_dest_ip.to_string();

                /* Case of DNAPT entry, where DIP is NAT'ted and the L4 port is NAT'ted. */
                SWSS_LOG_INFO("DNAPT %s conntrack notification", opStr.c_str());

                string dst_l4_port      = std::to_string(entry.orig_dst_l4_port);
                string nat_dst_l4_port  = std::to_string(entry.nat_dst_l4_port);

                key             += ":" + dst_l4_port;
                reverseEntryKey += ":" + nat_dst_l4_port;

                std::vector<FieldValueTuple> values;
                if (! m_AppRestartAssist->isWarmStartInProgress())
                {
                    if ((entryExists = m_naptCheckTable.get(key, values)))
                    {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAPT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("DNAPT %s: static entry exists, not processing the NAPT notification", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("DNAPT CREATE: ignoring the duplicated notification");
                            return 1;
                        }
                        else
                        {
                            m_naptTable.del(key);
                            SWSS_LOG_NOTICE("DNAPT entry with key %s deleted from APP_DB", key.c_str());
                        }
                     }
                     if ((reverseEntryExists = m_naptCheckTable.get(reverseEntryKey, values)))
                     {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAPT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("DNAPT %s: static reverse entry exists, not adding dynamic NAPT entry", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("DNAPT CREATE: ignoring the duplicated notification");
                            return 1;
                        }
                        else
                        {
                            m_naptTable.del(reverseEntryKey);
                            SWSS_LOG_NOTICE("Implicit SNAPT entry with key %s deleted from APP_DB", reverseEntryKey.c_str());
                        }
                    }
                }
                if (addFlag)
                {
                    FieldValueTuple dnat_translated_port("translated_l4_port", nat_dst_l4_port);
                    FieldValueTuple snat_translated_port("translated_l4_port", dst_l4_port);

                    fvVector.push_back(dnat_translated_port);
                    reverseFvVector.push_back(snat_translated_port);

                    if (m_AppRestartAssist->isWarmStartInProgress())
                    {
                        m_AppRestartAssist->insertToMap(APP_NAPT_TABLE_NAME, key, fvVector, false);
                        m_AppRestartAssist->insertToMap(APP_NAPT_TABLE_NAME, reverseEntryKey, reverseFvVector, false);
                    }
                    else
                    {
                        m_naptTable.set(key, fvVector);
                        SWSS_LOG_NOTICE("DNAPT entry with key %s added to APP_DB", key.c_str());
                        m_naptTable.set(reverseEntryKey, reverseFvVector);
                        SWSS_LOG_NOTICE("Implicit SNAPT entry with key %s added to APP_DB", reverseEntryKey.c_str());
                    }
                }
            }
            else
            {
                /* Case of Basic DNAT entry, where DIP is NAT'ted but the port translation is not done. */
                SWSS_LOG_INFO("DNAT %s conntrack notification", opStr.c_str());
                key             += entry.orig_dest_ip.to_string();
                reverseEntryKey += entry.nat_dest_ip.to_string();

                std::vector<FieldValueTuple> values;
                if (! m_AppRestartAssist->isWarmStartInProgress())
                {
                    if ((entryExists = m_natCheckTable.get(key, values)))
                    {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("DNAT %s: static entry exists, not processing the NAT notification", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("DNAT CREATE: ignoring the duplicated notification");
                            return 1;
                        }
                        else
                        { 
                            m_natTable.del(key);
                            SWSS_LOG_NOTICE("DNAT entry with key %s deleted from APP_DB", key.c_str());
                        }
                    }
                    if ((reverseEntryExists = m_natCheckTable.get(reverseEntryKey, values)))
                    {
                        for (auto iter : values)
                        {
                            if ((fvField(iter) == "entry_type") && (fvValue(iter) == "static"))
                            {
                                /* If a matching Static NAT entry exists in the APP_DB,
                                 * it has higher priority than the dynamic napt entry. */
                                SWSS_LOG_INFO("DNAT %s: static reverse entry exists, not adding dynamic NAT entry", opStr.c_str());
                                return 1;
                            }
                        }
                        if (addFlag)
                        {
                            SWSS_LOG_INFO("DNAT CREATE: ignoring the duplicated notification");
                            return 1;
                        }
                        else
                        { 
                            m_natTable.del(reverseEntryKey);
                            SWSS_LOG_NOTICE("Implicit SNAT entry with key %s deleted from APP_DB", reverseEntryKey.c_str());
                        }
                    }
                }
                if (addFlag)
                {
                    if (m_AppRestartAssist->isWarmStartInProgress())
                    {
                        m_AppRestartAssist->insertToMap(APP_NAT_TABLE_NAME, key, fvVector, false);
                        m_AppRestartAssist->insertToMap(APP_NAT_TABLE_NAME, reverseEntryKey, reverseFvVector, false);
                    }
                    else
                    {
                        m_natTable.set(key, fvVector);
                        SWSS_LOG_NOTICE("DNAT entry with key %s added to APP_DB", key.c_str());
                        m_natTable.set(reverseEntryKey, reverseFvVector);
                        SWSS_LOG_NOTICE("Implicit SNAT entry with key %s added to APP_DB", reverseEntryKey.c_str());
                    }
                }
            }
        }
    }
    return 0;
}

/* This function is called only for updating the UDP connection entries
 * so as not to timeout early in the kernel. */
void NatSync::updateConnTrackEntry(struct nfnl_ct *ct)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Updating conntrack entry in the kernel");

    if (nfsock)
    {
        nfsock->updateConnTrackEntry(ct);
    }
}

/* This function is called to delete conflicting NAT entries
 * to ensure full cone NAT functionality.
 * Also this function is invoked to remove low priority
 * dynamic NAT entry when there is a matching static NAT entry */
void NatSync::deleteConnTrackEntry(struct nfnl_ct *ct)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Deleting conntrack entry in the kernel");

    if (nfsock)
    {
        nfsock->deleteConnTrackEntry(ct);
    }
}
