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

#define MAC_SYNC_MODE_FIELD  "mac_sync_mode"
#define MAC_SYNC_MODE_FPM    "fpm"
#define MAC_SYNC_MODE_KERNEL "kernel"
#define FDB_SYNC_GLOBAL_KEY  "global"
#define L3_EVPN_MH_SUBTYPE   "L3EvpnMH"

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

    struct rtattr *rta = (struct rtattr *)(void *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    if (alen)
    {
        memcpy(RTA_DATA(rta), data, alen);
    }
    n->nlmsg_len = (uint32_t)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len));
    return true;
}

/* False when the stream ends in an incomplete attribute header or payload. A
 * last attribute without its alignment padding leaves len negative, which the
 * kernel accepts as well. */
static bool parseRtAttrs(struct rtattr **tb, int max, struct rtattr *rta, int len)
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

    return len <= 0;
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

static bool hasField(const std::vector<FieldValueTuple>& row, const string& field)
{
    return std::any_of(row.begin(), row.end(), [&field](const FieldValueTuple& fv) {
        return fvField(fv) == field;
    });
}

/* SONiC names a VLAN's VxLAN netdev <tunnel>-<vid>; empty for any other name. */
static string vlanOfNetdev(const string& ifname)
{
    auto dash = ifname.rfind('-');

    if (dash == string::npos || dash + 1 == ifname.size())
    {
        return "";
    }
    return "Vlan" + ifname.substr(dash + 1);
}

MacSync::MacSync(RedisPipeline *pipeline, DBConnector *stateDb, DBConnector *cfgDb) :
    m_vxlanFdbTable(pipeline, APP_VXLAN_FDB_TABLE_NAME, true),
    m_vxlanFdbTableRead(pipeline->getDBConnector(), APP_VXLAN_FDB_TABLE_NAME),
    m_stateFdbTable(stateDb, STATE_FDB_TABLE_NAME),
    m_stateFdbSyncTable(stateDb, STATE_FDB_SYNC_TABLE_NAME),
    m_cfgFdbSyncTableRead(cfgDb, CFG_FDB_SYNC_TABLE_NAME),
    m_cfgEvpnEsTable(cfgDb, "EVPN_ETHERNET_SEGMENT"),
    m_ifNameToIndex([](const string& name) { return if_nametoindex(name.c_str()); }),
    m_ifIndexToName([](unsigned int ifindex) {
        char name[IF_NAMESIZE] = {0};
        return (ifindex && if_indextoname(ifindex, name)) ? string(name) : string();
    })
{
    string subtype;
    Table deviceMetadata(cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);

    m_l3EvpnMh = deviceMetadata.hget("localhost", "subtype", subtype) && subtype == L3_EVPN_MH_SUBTYPE;
    readCfgFdbSyncMode();
}

/*
 * Read once, at startup: zebra takes --kernel-mac-ext-learn from the same key when
 * the bgp container starts, so the two ends of the FPM channel always agree and a
 * mode change applies on the next bgp restart. fdbsyncd runs in the swss container,
 * which a bgp restart leaves running, so the mode is published for it to follow.
 */
void MacSync::readCfgFdbSyncMode()
{
    string mode;

    m_cfgFdbSyncTableRead.hget(FDB_SYNC_GLOBAL_KEY, MAC_SYNC_MODE_FIELD, mode);
    m_fpmMode = m_l3EvpnMh || (mode == MAC_SYNC_MODE_FPM);
    m_stateFdbSyncTable.hset(FDB_SYNC_GLOBAL_KEY, MAC_SYNC_MODE_FIELD,
                             m_fpmMode ? MAC_SYNC_MODE_FPM : MAC_SYNC_MODE_KERNEL);
    SWSS_LOG_NOTICE("MacSync: mac_sync_mode is %s%s", m_fpmMode ? "fpm" : "kernel",
                    m_l3EvpnMh ? ", fixed by the L3EvpnMH subtype" : "");
}

/* The Ethernet Segment table is keyed by the interface the ESI is bound to, so
 * an entry is the authority on whether this port carries one. */
bool MacSync::isEthernetSegmentInterface(const string& ifname)
{
    std::vector<FieldValueTuple> values;

    return m_cfgEvpnEsTable.get(ifname, values);
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

/*
 * Deleting a VxLAN netdev flushes its FDB in the kernel, which is how fdbsyncd
 * withdraws the rows in kernel mode. zebra sends no per-MAC delete for it: its
 * remote MAC uninstall needs the netdev still enslaved to the bridge. onMacMsg()
 * keys a remote MAC by the VLAN in the <tunnel>-<vid> name of the netdev it came
 * on, so that VLAN's rows are withdrawn here, whoever wrote them. Ethernet
 * Segment port rows stay, as the kernel keeps those entries on the port.
 */
void MacSync::onVxlanLinkDel(const string& ifname)
{
    if (!m_fpmMode)
    {
        return;
    }

    string vlanName = vlanOfNetdev(ifname);
    if (vlanName.empty())
    {
        SWSS_LOG_INFO("MacSync: %s deleted, but its name gives no VLAN", ifname.c_str());
        return;
    }

    string prefix = vlanName + ":";
    std::vector<string> gone;

    /* orchagent may not have read the rows MacSync queued last. A row MacSync
     * withdrew can still be in APPL_DB, and withdrawing it again does no harm. */
    for (auto it = m_remoteRows.lower_bound(prefix);
         it != m_remoteRows.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
    {
        if (!hasField(it->second.row, "ifname"))
        {
            gone.push_back(it->first);
        }
    }

    std::vector<string> keys;
    m_vxlanFdbTableRead.getKeys(keys);
    for (const auto& key : keys)
    {
        std::vector<FieldValueTuple> row;

        if (key.compare(0, prefix.size(), prefix) == 0 && !m_remoteRows.count(key) &&
            m_vxlanFdbTableRead.get(key, row) && !hasField(row, "ifname"))
        {
            gone.push_back(key);
        }
    }

    for (const auto& key : gone)
    {
        removeRemoteMac(key);
        SWSS_LOG_INFO("MacSync: removed remote MAC %s, %s is gone", key.c_str(), ifname.c_str());
    }

    size_t withdrawn = gone.size();
    if (withdrawn)
    {
        SWSS_LOG_NOTICE("MacSync: %s deleted, withdrew its %zu remote MACs", ifname.c_str(), withdrawn);
    }
}

/* set() merges fields, so a MAC whose destination changes would keep the old
 * one. The row is deleted first, whoever wrote it; when both reach orchagent
 * in one pop, it sees only the new row. */
void MacSync::writeRemoteMac(const string& key, const std::vector<FieldValueTuple>& row)
{
    m_vxlanFdbTable.del(key);
    m_vxlanFdbTable.set(key, row);
    m_remoteRows[key] = RemoteRow{row};
}

/* By key, whoever wrote the row: one an earlier fpmsyncd or fdbsyncd in kernel
 * mode left behind is still zebra's to withdraw. */
void MacSync::removeRemoteMac(const string& key)
{
    m_vxlanFdbTable.del(key);
    m_remoteRows.erase(key);
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
    /* A refresh whose port no longer resolves is not sent, so zebra still holds
     * the MAC on the ifindex it last heard, and the delete must go there. */
    local.ifindex = stored.ifindex;
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

    /* A delete withdraws the MAC where zebra last heard it, whatever port the
     * entry names by then: the port can be torn down before its FDB deletes are
     * processed, and a refresh naming a port that did not resolve was not sent. */
    unsigned int ifindex = add ? m_ifNameToIndex(local.port) : local.ifindex;
    if (ifindex == 0)
    {
        if (add)
        {
            SWSS_LOG_ERROR("MacSync: cannot resolve ifindex for port %s", local.port.c_str());
        }
        else
        {
            SWSS_LOG_INFO("MacSync: local MAC %s:%s was never sent to zebra, nothing to withdraw",
                          vlanName.c_str(), mac.c_str());
        }
        return;
    }

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
     * to a port by configuration and must not move; RFC 7432 section 15.2 wants
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
    local.ifindex = ifindex;

    SWSS_LOG_INFO("MacSync: sent local MAC %s %s:%s port %s ifindex %u%s",
                  add ? "add" : "del", vlanName.c_str(), mac.c_str(), local.port.c_str(),
                  ifindex, local.isStatic ? " (static, sticky)" : "");
}

void MacSync::onMacMsg(struct nlmsghdr *h, int len)
{
    if (!m_fpmMode)
    {
        return;
    }

    if (m_readLinks)
    {
        m_readLinks();
    }

    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(h);

    if (ndm->ndm_family != AF_BRIDGE)
    {
        return;
    }

    struct rtattr *tb[NDA_MAX + 1];
    if (!parseRtAttrs(tb, NDA_MAX, (struct rtattr *)(void *)((char *)ndm + sizeof(struct ndmsg)), len))
    {
        SWSS_LOG_ERROR("MacSync: dropping inbound MAC message with a malformed attribute");
        return;
    }

    if (!tb[NDA_LLADDR] || RTA_PAYLOAD(tb[NDA_LLADDR]) != ETHER_ADDR_LEN)
    {
        SWSS_LOG_ERROR("MacSync: inbound MAC message without a valid NDA_LLADDR");
        return;
    }

    uint16_t vid = 0;
    uint32_t nhid = 0;
    uint32_t srcVni = 0;
    uint32_t vxlanVni = 0;
    size_t dstLen = tb[NDA_DST] ? RTA_PAYLOAD(tb[NDA_DST]) : 0;

    /* A VTEP is 4 or 16 bytes. Any other NDA_DST is not "no VTEP": on an
     * Ethernet Segment port it would replace the MAC's row with that port. */
    if ((tb[NDA_VLAN] && !getAttr(tb[NDA_VLAN], vid)) ||
        (tb[NDA_NH_ID] && !getAttr(tb[NDA_NH_ID], nhid)) ||
        (tb[NDA_SRC_VNI] && !getAttr(tb[NDA_SRC_VNI], srcVni)) ||
        (tb[NDA_VNI] && !getAttr(tb[NDA_VNI], vxlanVni)) ||
        (tb[NDA_DST] && dstLen != sizeof(struct in_addr) && dstLen != sizeof(struct in6_addr)))
    {
        SWSS_LOG_ERROR("MacSync: dropping inbound MAC message with an attribute of the wrong size");
        return;
    }

    MacAddress mac((uint8_t *)RTA_DATA(tb[NDA_LLADDR]));

    /* zebra emits a remote MAC twice: a bridge-side copy carrying NDA_VLAN but no
     * VTEP, and a VxLAN-side copy carrying NDA_DST but no VLAN. The netdev name
     * is the only VLAN source present on both, so derive from it as fdbsyncd does
     * (SONiC names the device <tunnel>-<vlanid>). */
    string ifname = m_ifIndexToName((unsigned int)ndm->ndm_ifindex);
    string vlanName = vlanOfNetdev(ifname);

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

    if (!remove && ndm->ndm_ifindex && ifname.empty())
    {
        /* The netdev is gone: a VxLAN netdev's rows went with its delete. */
        SWSS_LOG_INFO("MacSync: skipping MAC %s on deleted ifindex %d", key.c_str(), ndm->ndm_ifindex);
        return;
    }

    if (remove)
    {
        removeRemoteMac(key);
        SWSS_LOG_INFO("MacSync: removed remote MAC %s", key.c_str());
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
        bool haveDst = false;

        /* zebra emits NDA_DST even when there is no VTEP, sized from the address
         * family it guessed rather than from what it holds, so an all-zero
         * destination means "no VTEP" and not 0.0.0.0. */
        if (dstLen)
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
        else if (!ifname.empty() && isEthernetSegmentInterface(ifname))
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

    writeRemoteMac(key, fvVector);

    string dest = !nexthopGroup.empty() ? ("nexthop_group " + nexthopGroup)
                : !esInterface.empty()  ? ("ifname " + esInterface)
                                        : ("vtep " + string(vtep));
    SWSS_LOG_INFO("MacSync: added remote MAC %s %s vni %u", key.c_str(), dest.c_str(), vni);
}
