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

#ifndef __NATSYNC_H__
#define __NATSYNC_H__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartAssist.h"
#include "ipaddress.h"
#include "nfnetlink.h"
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <unistd.h>

// The timeout value (in seconds) for natsyncd reconcilation logic
#define DEFAULT_NATSYNC_WARMSTART_TIMER 30

/* This is the timer value (in seconds) that the natsyncd waits for 
 * restore_nat_entries service to finish. */

#define RESTORE_NAT_WAIT_TIME_OUT 120

namespace swss {

struct naptEntry;

class NatSync : public NetMsg
{
public:
    NatSync(RedisPipeline *pipelineAppDB, DBConnector *appDb, DBConnector *stateDb, NfNetlink *nfnl);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    bool isNatRestoreDone();
    bool isPortInitDone(DBConnector *app_db);

    AppRestartAssist *getRestartAssist()
    {
        return m_AppRestartAssist;
    }

private:
    static int  parseConnTrackMsg(const struct nfnl_ct *ct, struct naptEntry &entry);
    void        updateConnTrackEntry(struct nfnl_ct *ct);
    void        deleteConnTrackEntry(struct nfnl_ct *ct);

    bool        matchingSnaptPoolExists(const IpAddress &natIp);
    bool        matchingSnaptEntryExists(const naptEntry &entry);
    bool        matchingDnaptEntryExists(const naptEntry &entry);
    int         addNatEntry(struct nfnl_ct *ct, struct naptEntry &entry, bool addFlag);

    ProducerStateTable m_natTable;
    ProducerStateTable m_naptTable;
    ProducerStateTable m_natTwiceTable;
    ProducerStateTable m_naptTwiceTable;

    Table              m_natCheckTable;
    Table              m_naptCheckTable;
    Table              m_naptPoolCheckTable;
    Table              m_twiceNatCheckTable;
    Table              m_twiceNaptCheckTable;

    Table              m_stateNatRestoreTable;
    AppRestartAssist  *m_AppRestartAssist;

    NfNetlink          *nfsock;
};

struct naptEntry
{
    uint32_t conntrack_id;
    uint8_t  protocol;
    IpAddress orig_src_ip;
    uint16_t orig_src_l4_port;
    IpAddress orig_dest_ip;
    uint16_t orig_dst_l4_port;
    IpAddress nat_src_ip;
    uint16_t nat_src_l4_port;
    IpAddress nat_dest_ip;
    uint16_t nat_dst_l4_port;
    uint32_t ct_status;
};

/* Copy of nl_addr from netlink-private/types.h */
struct nl_ip_addr
{
    int              a_family;
    unsigned int     a_maxsize;
    unsigned int     a_len;
    int              a_prefixlen;
    int              a_refcnt;
    int              a_addr;
};

}

#endif /* __NATSYNC_H__ */
