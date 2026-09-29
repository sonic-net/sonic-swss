#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>

#include <algorithm>

#include "logger.h"
#include "macaddress.h"
#include "schema.h"
#include "fpmsyncd/macsync.h"

using namespace std;
using namespace swss;

#ifndef NDA_VNI
#define NDA_VNI 7
#endif

#ifndef NDA_SRC_VNI
#define NDA_SRC_VNI 11
#endif

#ifndef NDA_NH_ID
#define NDA_NH_ID 13
#endif

#define MAC_SYNC_MODE_FIELD "mac_sync_mode"
#define MAC_SYNC_MODE_FPM   "fpm"
#define FDB_SYNC_GLOBAL_KEY "global"
#define L3_EVPN_MH_SUBTYPE  "L3EvpnMH"

/* Same buffer shape zebra uses for AF_BRIDGE FDB messages. */
struct MacNlRequest
{
    struct nlmsghdr n;
    struct ndmsg ndm;
    char buf[256];
};

static bool addAttr(struct nlmsghdr *n, size_t maxLen, int type,
                    const void *data, size_t alen)
{
    size_t len = RTA_LENGTH(alen);

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxLen)
    {
        SWSS_LOG_ERROR("MacSync: netlink attribute %d does not fit in the message", type);
        return false;
    }

    struct rtattr *rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    if (alen)
    {
        memcpy(RTA_DATA(rta), data, alen);
    }
    n->nlmsg_len = (uint32_t)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len));
    return true;
}

static void parseRtAttrs(struct rtattr **tb, int max, struct rtattr *rta, int len)
{
    memset(tb, 0, sizeof(struct rtattr *) * (unsigned int)(max + 1));
    while (RTA_OK(rta, len))
    {
        if (rta->rta_type <= max)
        {
            tb[rta->rta_type] = rta;
        }
        rta = RTA_NEXT(rta, len);
    }
}

/* Reads a fixed-size attribute; false when the payload is too short to hold one. */
template <typename T>
static bool getAttr(const struct rtattr *rta, T& value)
{
    if (RTA_PAYLOAD(rta) < sizeof(T))
    {
        return false;
    }
    memcpy(&value, RTA_DATA(rta), sizeof(T));
    return true;
}

MacSync::MacSync(RedisPipeline *pipeline, DBConnector *stateDb, DBConnector *cfgDb) :
    m_vxlanFdbTable(pipeline, APP_VXLAN_FDB_TABLE_NAME, true),
    m_stateFdbTable(stateDb, STATE_FDB_TABLE_NAME),
    m_cfgFdbSyncTable(cfgDb, CFG_FDB_SYNC_TABLE_NAME),
    m_cfgFdbSyncTableRead(cfgDb, CFG_FDB_SYNC_TABLE_NAME),
    m_cfgEvpnEsTable(cfgDb, "EVPN_ETHERNET_SEGMENT"),
    m_ifNameToIndex([](const string& name) { return if_nametoindex(name.c_str()); })
{
    string subtype;
    Table deviceMetadata(cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);

    m_l3EvpnMh = deviceMetadata.hget("localhost", "subtype", subtype) && subtype == L3_EVPN_MH_SUBTYPE;
    readCfgFdbSyncMode();
}

void MacSync::readCfgFdbSyncMode()
{
    string mode;

    m_cfgFdbSyncTableRead.hget(FDB_SYNC_GLOBAL_KEY, MAC_SYNC_MODE_FIELD, mode);
    setMacSyncMode(mode);
}

/* The Ethernet Segment table is keyed by the interface the ESI is bound to, so
 * an entry is the authority on whether this port carries one. */
bool MacSync::isEthernetSegmentInterface(const string& ifname)
{
    std::vector<FieldValueTuple> values;

    return m_cfgEvpnEsTable.get(ifname, values);
}

void MacSync::setMacSyncMode(const string& mode)
{
    bool fpmMode = m_l3EvpnMh || (mode == MAC_SYNC_MODE_FPM);

    if (fpmMode == m_fpmMode)
    {
        return;
    }

    m_fpmMode = fpmMode;
    SWSS_LOG_NOTICE("MacSync: mac_sync_mode is now %s%s", m_fpmMode ? "fpm" : "kernel",
                    m_l3EvpnMh ? ", fixed by the L3EvpnMH subtype" : "");

    if (!m_fpmMode)
    {
        /* processStateFdb() discards updates while fdbsyncd owns the kernel
         * path, so the local cache would only go stale. */
        m_localMacs.clear();
    }
}

void MacSync::processCfgFdbSync()
{
    std::deque<KeyOpFieldsValuesTuple> entries;

    m_cfgFdbSyncTable.pops(entries);

    for (auto& entry : entries)
    {
        if (kfvKey(entry) != FDB_SYNC_GLOBAL_KEY)
        {
            continue;
        }

        if (kfvOp(entry) != SET_COMMAND)
        {
            setMacSyncMode("");
            continue;
        }

        for (auto& fv : kfvFieldsValues(entry))
        {
            if (fvField(fv) == MAC_SYNC_MODE_FIELD)
            {
                setMacSyncMode(fvValue(fv));
            }
        }
    }
}

/*
 * Local MACs go to zebra as STATE_DB changes. The ones already in STATE_DB when
 * fpmsyncd starts reach zebra through the subscriber's initial read, which is
 * processed once the first connection is up. A zebra that reconnects is not
 * sent them again.
 */
void MacSync::onFpmConnected(FpmInterface& fpm)
{
    m_fpmInterface = &fpm;
}

void MacSync::onFpmDisconnected()
{
    m_fpmInterface = nullptr;
}

void MacSync::processStateFdb()
{
    std::deque<KeyOpFieldsValuesTuple> entries;

    m_stateFdbTable.pops(entries);

    if (!m_fpmMode)
    {
        /* fdbsyncd owns the kernel path in this mode. */
        return;
    }

    for (auto& entry : entries)
    {
        processStateFdbEntry(entry);
    }
}

void MacSync::processStateFdbEntry(const KeyOpFieldsValuesTuple& entry)
{
    const string& key = kfvKey(entry);
    bool add = (kfvOp(entry) == SET_COMMAND);

    auto delimiter = key.find_first_of(':');
    if (delimiter == string::npos)
    {
        SWSS_LOG_ERROR("MacSync: malformed STATE_DB FDB key %s", key.c_str());
        return;
    }

    string vlanName = key.substr(0, delimiter);
    string mac = key.substr(delimiter + 1);

    if (!add)
    {
        auto it = m_localMacs.find(key);
        if (it == m_localMacs.end())
        {
            return;
        }
        LocalMac local = it->second;
        m_localMacs.erase(it);
        sendLocalMac(vlanName, mac, local, false);
        return;
    }

    LocalMac local{"", false, 0};

    for (auto& fv : kfvFieldsValues(entry))
    {
        if (fvField(fv) == "port")
        {
            local.port = fvValue(fv);
        }
        else if (fvField(fv) == "type")
        {
            local.isStatic = (fvValue(fv) == "static");
        }
    }

    if (local.port.empty())
    {
        SWSS_LOG_ERROR("MacSync: STATE_DB FDB entry %s has no port", key.c_str());
        return;
    }

    LocalMac& stored = m_localMacs[key];
    stored = local;
    sendLocalMac(vlanName, mac, stored, true);
}

void MacSync::sendLocalMac(const string& vlanName, const string& mac, LocalMac& local,
                           bool add)
{
    if (!m_fpmMode || !m_fpmInterface)
    {
        return;
    }

    unsigned int ifindex = m_ifNameToIndex(local.port);
    if (ifindex == 0 && !add && local.ifindex)
    {
        /* The port can be torn down before its FDB deletes are processed. */
        ifindex = local.ifindex;
    }
    if (ifindex == 0)
    {
        SWSS_LOG_ERROR("MacSync: cannot resolve ifindex for port %s", local.port.c_str());
        return;
    }
    local.ifindex = ifindex;

    if (vlanName.compare(0, 4, "Vlan") != 0)
    {
        SWSS_LOG_ERROR("MacSync: unexpected VLAN name %s", vlanName.c_str());
        return;
    }

    uint16_t vlanId;
    try
    {
        vlanId = (uint16_t)stoul(vlanName.substr(4));
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_ERROR("MacSync: cannot parse VLAN id from %s", vlanName.c_str());
        return;
    }

    MacAddress macAddress;
    try
    {
        macAddress = MacAddress(mac);
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_ERROR("MacSync: cannot parse MAC %s", mac.c_str());
        return;
    }

    MacNlRequest req{};

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | (add ? (NLM_F_CREATE | NLM_F_REPLACE) : 0);
    req.n.nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
    req.ndm.ndm_family = AF_BRIDGE;
    req.ndm.ndm_ifindex = (int)ifindex;
    req.ndm.ndm_flags = NTF_MASTER | NTF_EXT_LEARNED;
    /* Follow zebra's own encoding (netlink_macfdb_update_ctx): NTF_STICKY and
     * NUD_NOARP are set together, and only for a sticky entry. NUD_NOARP on its
     * own is not an encoding zebra produces or reads, so it must not be used to
     * mean "static" on its own.
     *
     * Only administratively configured MACs are sticky. STATE_DB carries local
     * entries only, so a static entry here is a provisioned MAC that is pinned
     * to a port by configuration and must not move; RFC 7432 section 7.8 wants
     * exactly that advertised with the sticky bit so remote PEs reject a move.
     * Hardware-learnt MACs stay mobile. */
    if (local.isStatic)
    {
        req.ndm.ndm_flags |= NTF_STICKY;
        req.ndm.ndm_state = NUD_REACHABLE | NUD_NOARP;
    }
    else
    {
        req.ndm.ndm_state = NUD_REACHABLE;
    }

    if (!addAttr(&req.n, sizeof(req), NDA_LLADDR, macAddress.getMac(), ETHER_ADDR_LEN) ||
        !addAttr(&req.n, sizeof(req), NDA_VLAN, &vlanId, sizeof(vlanId)))
    {
        return;
    }

    if (!m_fpmInterface->send(&req.n))
    {
        SWSS_LOG_ERROR("MacSync: failed to send local MAC %s:%s to zebra",
                       vlanName.c_str(), mac.c_str());
        return;
    }

    SWSS_LOG_INFO("MacSync: sent local MAC %s %s:%s port %s%s",
                  add ? "add" : "del", vlanName.c_str(), mac.c_str(), local.port.c_str(),
                  local.isStatic ? " (static, sticky)" : "");
}

void MacSync::onMacMsg(struct nlmsghdr *h, int len)
{
    if (!m_fpmMode)
    {
        return;
    }

    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(h);

    if (ndm->ndm_family != AF_BRIDGE)
    {
        return;
    }

    struct rtattr *tb[NDA_MAX + 1];
    parseRtAttrs(tb, NDA_MAX, (struct rtattr *)((char *)ndm + sizeof(struct ndmsg)), len);

    if (!tb[NDA_LLADDR] || RTA_PAYLOAD(tb[NDA_LLADDR]) != ETHER_ADDR_LEN)
    {
        SWSS_LOG_ERROR("MacSync: inbound MAC message without a valid NDA_LLADDR");
        return;
    }

    uint16_t vid = 0;
    uint32_t nhid = 0;
    uint32_t srcVni = 0;
    uint32_t vxlanVni = 0;

    if ((tb[NDA_VLAN] && !getAttr(tb[NDA_VLAN], vid)) ||
        (tb[NDA_NH_ID] && !getAttr(tb[NDA_NH_ID], nhid)) ||
        (tb[NDA_SRC_VNI] && !getAttr(tb[NDA_SRC_VNI], srcVni)) ||
        (tb[NDA_VNI] && !getAttr(tb[NDA_VNI], vxlanVni)))
    {
        SWSS_LOG_ERROR("MacSync: dropping inbound MAC message with a truncated attribute");
        return;
    }

    MacAddress mac((uint8_t *)RTA_DATA(tb[NDA_LLADDR]));

    /* zebra emits a remote MAC twice: a bridge-side copy carrying NDA_VLAN but no
     * VTEP, and a VxLAN-side copy carrying NDA_DST but no VLAN. The netdev name
     * is the only VLAN source present on both, so derive from it as fdbsyncd does
     * (SONiC names the device <tunnel>-<vlanid>). */
    string vlanName;
    char ifname[IF_NAMESIZE] = {0};

    if (if_indextoname((unsigned int)ndm->ndm_ifindex, ifname))
    {
        string intf(ifname);
        auto dash = intf.rfind('-');
        if (dash != string::npos)
        {
            vlanName = "Vlan" + intf.substr(dash + 1);
        }
    }

    if (vlanName.empty() && tb[NDA_VLAN])
    {
        vlanName = "Vlan" + to_string(vid);
    }

    if (vlanName.empty())
    {
        SWSS_LOG_ERROR("MacSync: cannot determine VLAN for inbound MAC %s on ifindex %d",
                       mac.to_string().c_str(), ndm->ndm_ifindex);
        return;
    }

    string key = vlanName + ":" + mac.to_string();
    /* Same withdrawal semantics fdbsyncd applies on the kernel path: an
     * add carrying NUD_INCOMPLETE or NUD_FAILED is a removal. */
    bool remove = (h->nlmsg_type == RTM_DELNEIGH) ||
                  (ndm->ndm_state == NUD_INCOMPLETE) ||
                  (ndm->ndm_state == NUD_FAILED);

    if (remove)
    {
        if (m_remoteMacs.erase(key))
        {
            m_vxlanFdbTable.del(key);
            SWSS_LOG_INFO("MacSync: removed remote MAC %s", key.c_str());
        }
        return;
    }

    /* A MAC behind an Ethernet Segment reaches several VTEPs, so zebra resolves
     * it to an L2 nexthop group and sends NDA_NH_ID with no NDA_DST. The group
     * itself is published to L2_NEXTHOP_GROUP_TABLE by fdbsyncd, which keeps
     * reading it from the kernel in either mac_sync_mode. */
    string nexthopGroup;

    if (nhid)
    {
        nexthopGroup = to_string(nhid);
    }

    char vtep[INET6_ADDRSTRLEN] = {0};
    string esInterface;

    if (nexthopGroup.empty())
    {
        size_t dstLen = tb[NDA_DST] ? RTA_PAYLOAD(tb[NDA_DST]) : 0;
        bool haveDst = false;

        /* zebra emits NDA_DST even when there is no VTEP, sized from the address
         * family it guessed rather than from what it holds, so an unusable or
         * all-zero destination means "no VTEP" and not 0.0.0.0. */
        if (dstLen == sizeof(struct in_addr) || dstLen == sizeof(struct in6_addr))
        {
            const uint8_t *dst = (const uint8_t *)RTA_DATA(tb[NDA_DST]);
            haveDst = std::any_of(dst, dst + dstLen, [](uint8_t b) { return b != 0; });
        }

        if (haveDst)
        {
            int family = (dstLen == sizeof(struct in6_addr)) ? AF_INET6 : AF_INET;

            if (!inet_ntop(family, RTA_DATA(tb[NDA_DST]), vtep, sizeof(vtep)))
            {
                SWSS_LOG_ERROR("MacSync: cannot parse remote VTEP for %s", key.c_str());
                return;
            }
        }
        else if (ifname[0] && isEthernetSegmentInterface(ifname))
        {
            /* A MAC on an Ethernet Segment we also hold locally is reachable
             * through our own access port, so zebra sends it against that port
             * with neither a VTEP nor a group. FdbOrch programs it against the
             * port with ageing disabled. */
            esInterface = ifname;
        }
        else
        {
            /* The bridge-side copy of a remote MAC; the VxLAN-side copy carries the VTEP. */
            SWSS_LOG_INFO("MacSync: skipping inbound MAC %s without NDA_DST", key.c_str());
            return;
        }
    }

    /* zebra encodes the VNI as NDA_SRC_VNI; NDA_VNI is accepted as a fallback. */
    uint32_t vni = tb[NDA_SRC_VNI] ? srcVni : vxlanVni;

    std::vector<FieldValueTuple> fvVector;
    if (!nexthopGroup.empty())
    {
        fvVector.emplace_back("nexthop_group", nexthopGroup);
    }
    else if (!esInterface.empty())
    {
        fvVector.emplace_back("ifname", esInterface);
    }
    else
    {
        fvVector.emplace_back("remote_vtep", vtep);
    }
    fvVector.emplace_back("type", (ndm->ndm_state & NUD_NOARP) ? "static" : "dynamic");
    fvVector.emplace_back("vni", to_string(vni));

    m_vxlanFdbTable.set(key, fvVector);
    m_remoteMacs.insert(key);

    string dest = !nexthopGroup.empty() ? ("nexthop_group " + nexthopGroup)
                : !esInterface.empty()  ? ("ifname " + esInterface)
                                        : ("vtep " + string(vtep));
    SWSS_LOG_INFO("MacSync: added remote MAC %s %s vni %u", key.c_str(), dest.c_str(), vni);
}
