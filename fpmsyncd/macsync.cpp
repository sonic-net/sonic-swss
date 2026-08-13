#include <arpa/inet.h>
#include <assert.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "macaddress.h"
#include "schema.h"
#include "fpm/fpm.h"
#include "fpmsyncd/macsync.h"

using namespace std;
using namespace swss;

#ifndef NDA_VNI
#define NDA_VNI 7
#endif

#ifndef NDA_SRC_VNI
#define NDA_SRC_VNI 11
#endif

#define MAC_SYNC_MODE_FIELD "mac_sync_mode"
#define MAC_SYNC_MODE_FPM   "fpm"
#define FDB_SYNC_GLOBAL_KEY "global"
#define FDB_SYNC_GENERATION_FIELD "generation"

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

MacSync::MacSync(RedisPipeline *pipeline, DBConnector *stateDb, DBConnector *cfgDb) :
    m_vxlanFdbTable(pipeline, APP_VXLAN_FDB_TABLE_NAME, true),
    m_vxlanFdbTableRead(pipeline, APP_VXLAN_FDB_TABLE_NAME, false),
    m_stateFdbTable(stateDb, STATE_FDB_TABLE_NAME),
    m_cfgFdbSyncTable(cfgDb, CFG_FDB_SYNC_TABLE_NAME),
    m_cfgFdbSyncTableRead(cfgDb, CFG_FDB_SYNC_TABLE_NAME),
    m_stateFdbTableRead(stateDb, STATE_FDB_TABLE_NAME),
    m_stateFdbSyncTable(stateDb, STATE_FDB_SYNC_TABLE_NAME)
{
    readCfgFdbSyncMode();
}

void MacSync::readCfgFdbSyncMode()
{
    string mode;

    if (m_cfgFdbSyncTableRead.hget(FDB_SYNC_GLOBAL_KEY, MAC_SYNC_MODE_FIELD, mode))
    {
        setMacSyncMode(mode);
    }
}

void MacSync::setMacSyncMode(const string& mode)
{
    bool fpmMode = (mode == MAC_SYNC_MODE_FPM);

    if (fpmMode == m_fpmMode)
    {
        return;
    }

    m_fpmMode = fpmMode;
    SWSS_LOG_NOTICE("MacSync: mac_sync_mode is now %s", m_fpmMode ? "fpm" : "kernel");

    if (m_fpmMode)
    {
        loadRemoteMacs();
        replayLocalMacs();
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

void MacSync::onFpmConnected(FpmInterface& fpm)
{
    m_fpmInterface = &fpm;
    m_generation = nextGeneration();

    loadLocalMacs();
    replayLocalMacs();
    sendReplayEnd();
}

void MacSync::onFpmDisconnected()
{
    m_fpmInterface = nullptr;
}

/*
 * Rebuild the local MAC set straight from STATE_DB. The subscriber delivers the
 * existing entries through the select loop, which has not necessarily run by the
 * time zebra connects; replaying from an unpopulated map would tell zebra that
 * no MAC is local and have it withdraw every one of them.
 */
/*
 * Zebra keeps the MACs a previous fpmsyncd gave it, so reusing that process's
 * generation would make them look current and survive the sweep. The counter is
 * stored before it is sent, so a crash mid-replay cannot hand out the same value
 * twice.
 */
uint32_t MacSync::nextGeneration()
{
    std::string value;
    uint32_t generation = 0;

    if (m_stateFdbSyncTable.hget(FDB_SYNC_GLOBAL_KEY, FDB_SYNC_GENERATION_FIELD, value))
    {
        generation = (uint32_t)strtoul(value.c_str(), nullptr, 10);
    }

    ++generation;
    m_stateFdbSyncTable.hset(FDB_SYNC_GLOBAL_KEY, FDB_SYNC_GENERATION_FIELD,
                             std::to_string(generation));

    return generation;
}

/*
 * zebra replays router MACs on reconnect but never the remote EVPN MACs, so an
 * entry inherited from fdbsyncd or from a previous fpmsyncd would otherwise
 * find nothing to erase on withdrawal and stay programmed forever.
 */
void MacSync::loadRemoteMacs()
{
    std::vector<std::string> keys;

    m_vxlanFdbTableRead.getKeys(keys);
    m_remoteMacs.clear();
    m_remoteMacs.insert(keys.begin(), keys.end());

    SWSS_LOG_NOTICE("MacSync: adopted %zu remote MACs already in APPL_DB",
                    m_remoteMacs.size());
}

void MacSync::loadLocalMacs()
{
    if (!m_fpmMode)
    {
        return;
    }

    std::vector<std::string> keys;
    m_stateFdbTableRead.getKeys(keys);

    m_localMacs.clear();

    for (const auto& key : keys)
    {
        std::vector<FieldValueTuple> values;
        if (!m_stateFdbTableRead.get(key, values))
        {
            continue;
        }

        string port;
        bool isStatic = false;

        for (const auto& fv : values)
        {
            if (fvField(fv) == "port")
            {
                port = fvValue(fv);
            }
            else if (fvField(fv) == "type")
            {
                isStatic = (fvValue(fv) == "static");
            }
        }

        if (port.empty())
        {
            continue;
        }

        m_localMacs[key] = {port, isStatic};
    }
}

void MacSync::replayLocalMacs()
{
    if (!m_fpmMode || !m_fpmInterface)
    {
        return;
    }

    for (const auto& it : m_localMacs)
    {
        auto delimiter = it.first.find_first_of(':');
        if (delimiter == string::npos)
        {
            continue;
        }
        sendLocalMac(it.first.substr(0, delimiter), it.first.substr(delimiter + 1),
                     it.second.port, it.second.isStatic, true);
    }
}

void MacSync::sendReplayEnd()
{
    if (!m_fpmMode || !m_fpmInterface)
    {
        return;
    }

    struct nlmsghdr n{};

    n.nlmsg_len = NLMSG_LENGTH(0);
    n.nlmsg_flags = NLM_F_REQUEST;
    n.nlmsg_type = RTM_FPM_MAC_REPLAY_END;
    n.nlmsg_seq = m_generation;

    if (!m_fpmInterface->send(&n))
    {
        SWSS_LOG_ERROR("MacSync: failed to send end-of-replay for generation %u",
                       m_generation);
        return;
    }

    SWSS_LOG_NOTICE("MacSync: replayed %zu local MACs, generation %u",
                    m_localMacs.size(), m_generation);
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
        const string& key = kfvKey(entry);
        bool add = (kfvOp(entry) == SET_COMMAND);

        auto delimiter = key.find_first_of(':');
        if (delimiter == string::npos)
        {
            SWSS_LOG_ERROR("MacSync: malformed STATE_DB FDB key %s", key.c_str());
            continue;
        }

        string vlanName = key.substr(0, delimiter);
        string mac = key.substr(delimiter + 1);
        string port;
        bool isStatic = false;

        for (auto& fv : kfvFieldsValues(entry))
        {
            if (fvField(fv) == "port")
            {
                port = fvValue(fv);
            }
            else if (fvField(fv) == "type")
            {
                isStatic = (fvValue(fv) == "static");
            }
        }

        if (add)
        {
            if (port.empty())
            {
                SWSS_LOG_ERROR("MacSync: STATE_DB FDB entry %s has no port", key.c_str());
                continue;
            }
            m_localMacs[key] = {port, isStatic};
        }
        else
        {
            auto it = m_localMacs.find(key);
            if (it == m_localMacs.end())
            {
                continue;
            }
            port = it->second.port;
            isStatic = it->second.isStatic;
            m_localMacs.erase(it);
        }

        sendLocalMac(vlanName, mac, port, isStatic, add);
    }
}

void MacSync::sendLocalMac(const string& vlanName, const string& mac, const string& port,
                           bool isStatic, bool add)
{
    if (!m_fpmMode || !m_fpmInterface)
    {
        return;
    }

    unsigned int ifindex = if_nametoindex(port.c_str());
    if (ifindex == 0)
    {
        SWSS_LOG_ERROR("MacSync: cannot resolve ifindex for port %s", port.c_str());
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
    req.n.nlmsg_seq = m_generation;
    req.ndm.ndm_family = AF_BRIDGE;
    req.ndm.ndm_ifindex = (int)ifindex;
    req.ndm.ndm_state = isStatic ? NUD_NOARP : NUD_REACHABLE;
    req.ndm.ndm_flags = NTF_MASTER | NTF_EXT_LEARNED;

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

    SWSS_LOG_INFO("MacSync: sent local MAC %s %s:%s port %s",
                  add ? "add" : "del", vlanName.c_str(), mac.c_str(), port.c_str());
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

    if (!tb[NDA_LLADDR])
    {
        SWSS_LOG_ERROR("MacSync: inbound MAC message without NDA_LLADDR");
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
        vlanName = "Vlan" + to_string(*(uint16_t *)RTA_DATA(tb[NDA_VLAN]));
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

    if (!tb[NDA_DST])
    {
        /* The bridge-side copy of a remote MAC; the VxLAN-side copy carries the VTEP. */
        SWSS_LOG_INFO("MacSync: skipping inbound MAC %s without NDA_DST", key.c_str());
        return;
    }

    char vtep[INET6_ADDRSTRLEN] = {0};
    size_t dstLen = RTA_PAYLOAD(tb[NDA_DST]);
    int family = (dstLen == sizeof(struct in6_addr)) ? AF_INET6 : AF_INET;

    if (!inet_ntop(family, RTA_DATA(tb[NDA_DST]), vtep, sizeof(vtep)))
    {
        SWSS_LOG_ERROR("MacSync: cannot parse remote VTEP for %s", key.c_str());
        return;
    }

    /* zebra encodes the VNI as NDA_SRC_VNI; NDA_VNI is accepted as a fallback. */
    uint32_t vni = 0;
    if (tb[NDA_SRC_VNI])
    {
        vni = *(uint32_t *)RTA_DATA(tb[NDA_SRC_VNI]);
    }
    else if (tb[NDA_VNI])
    {
        vni = *(uint32_t *)RTA_DATA(tb[NDA_VNI]);
    }

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("remote_vtep", vtep);
    fvVector.emplace_back("type", (ndm->ndm_state & NUD_NOARP) ? "static" : "dynamic");
    fvVector.emplace_back("vni", to_string(vni));

    m_vxlanFdbTable.set(key, fvVector);
    m_remoteMacs.insert(key);

    SWSS_LOG_INFO("MacSync: added remote MAC %s vtep %s vni %u", key.c_str(), vtep, vni);
}
