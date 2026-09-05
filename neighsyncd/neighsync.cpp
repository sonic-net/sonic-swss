#include <string>
#include <cerrno>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <netlink/attr.h>
#include <netlink/handlers.h>
#include <netlink/msg.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include <poll.h>
#include <unistd.h>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "ipaddress.h"
#include "netmsg.h"
#include "linkcache.h"
#include "macaddress.h"

#include "neighsync.h"
#include "warm_restart.h"
#include <algorithm>
#include <linux/neighbour.h>
#include <memory>

using namespace std;
using namespace swss;

#define VRF_PREFIX              "Vrf"
#define TENMS                   10000
#define MAX_ROUTE_DEL_RETRY     100

static constexpr int VALID_NEIGH_STATES =
    NUD_PERMANENT | NUD_NOARP | NUD_REACHABLE | NUD_PROBE | NUD_STALE | NUD_DELAY;
static constexpr int LINK_LOCAL_DUMP_INACTIVITY_TIMEOUT_MS = 1000;

namespace
{

struct NlSocketDeleter
{
    void operator()(struct nl_sock *socket) const
    {
        if (socket)
        {
            nl_close(socket);
            nl_socket_free(socket);
        }
    }
};

struct LinkLocalDumpContext
{
    NeighSync *sync;
    int ifindex;
    uint32_t sequence;
    bool complete = false;
    bool interrupted = false;
    bool kernelError = false;
    int error = 0;
};

int processLinkLocalDumpMessage(struct nl_msg *message, void *arg)
{
    auto *context = static_cast<LinkLocalDumpContext *>(arg);
    auto *header = nlmsg_hdr(message);
    if (header->nlmsg_seq != context->sequence)
    {
        return NL_SKIP;
    }

    if (header->nlmsg_flags & NLM_F_DUMP_INTR)
    {
        context->interrupted = true;
    }

    if (header->nlmsg_type != RTM_NEWNEIGH)
    {
        return NL_SKIP;
    }

    struct rtnl_neigh *rawNeighbor = nullptr;
    int error = rtnl_neigh_parse(header, &rawNeighbor);
    if (error < 0)
    {
        context->error = error;
        return NL_STOP;
    }

    unique_ptr<rtnl_neigh, decltype(&rtnl_neigh_put)> neighbor(rawNeighbor, rtnl_neigh_put);
    if (rtnl_neigh_get_ifindex(neighbor.get()) != context->ifindex)
    {
        return NL_SKIP;
    }

    auto *address = rtnl_neigh_get_dst(neighbor.get());
    if (rtnl_neigh_get_family(neighbor.get()) != AF_INET6 || !address ||
        !IN6_IS_ADDR_LINKLOCAL(nl_addr_get_binary_addr(address)))
    {
        return NL_SKIP;
    }

    context->sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(neighbor.get()));
    return NL_OK;
}

int finishLinkLocalDump(struct nl_msg *message, void *arg)
{
    auto *context = static_cast<LinkLocalDumpContext *>(arg);
    auto *header = nlmsg_hdr(message);
    if (header->nlmsg_seq != context->sequence)
    {
        return NL_SKIP;
    }

    if (header->nlmsg_flags & NLM_F_DUMP_INTR)
    {
        context->interrupted = true;
    }
    context->complete = true;
    return NL_STOP;
}

int handleLinkLocalDumpError(struct sockaddr_nl *, struct nlmsgerr *error, void *arg)
{
    auto *context = static_cast<LinkLocalDumpContext *>(arg);
    context->kernelError = true;
    context->error = error->error;
    return NL_STOP;
}

}

NeighSync::NeighSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb, DBConnector *cfgDb, DBConnector *appDb) :
    m_neighTable(pipelineAppDB, APP_NEIGH_TABLE_NAME),
    m_kernelFailedNeighTable(pipelineAppDB, APP_NEIGH_FAILED_TABLE_NAME),
    m_routeTable(pipelineAppDB, APP_ROUTE_TABLE_NAME, false),
    m_routeCheckTable(appDb, APP_ROUTE_TABLE_NAME),
    m_kernelFailedNeighCheckTable(appDb, APP_NEIGH_FAILED_TABLE_NAME),
    m_stateNeighRestoreTable(stateDb, STATE_NEIGH_RESTORE_TABLE_NAME),
    m_cfgInterfaceTable(cfgDb, CFG_INTF_TABLE_NAME),
    m_cfgLagInterfaceTable(cfgDb, CFG_LAG_INTF_TABLE_NAME),
    m_cfgVlanInterfaceTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME),
    m_cfgPeerSwitchTable(cfgDb, CFG_PEER_SWITCH_TABLE_NAME),
    m_cfgEvpnNvoTable(cfgDb, CFG_VXLAN_EVPN_NVO_TABLE_NAME),
    m_nl_sock(NULL), m_link_cache(NULL)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "neighsyncd", "swss", DEFAULT_NEIGHSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_NEIGH_TABLE_NAME, &m_neighTable);
    }

    m_nl_sock = nl_socket_alloc();
    if (!m_nl_sock)
    {
        SWSS_LOG_THROW("Failed to allocate netlink socket");
    }

    if (nl_connect(m_nl_sock, NETLINK_ROUTE) < 0)
    {
        nl_socket_free(m_nl_sock);
        m_nl_sock = NULL;
        SWSS_LOG_THROW("Failed to connect to netlink socket");
    }

    if (rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache) < 0 || !m_link_cache)
    {
        nl_close(m_nl_sock);
        nl_socket_free(m_nl_sock);
        m_nl_sock = NULL;
        SWSS_LOG_THROW("Failed to allocate link cache");
    }
}

NeighSync::~NeighSync()
{
    if (m_AppRestartAssist)
    {
        delete m_AppRestartAssist;
    }

    if (m_link_cache)
    {
        nl_cache_free(m_link_cache);
    }

    if (m_nl_sock)
    {
        nl_close(m_nl_sock);
        nl_socket_free(m_nl_sock);
    }
}

// Use a separate dump socket so live notifications remain queued on the main
// socket and are processed after the snapshot through the same onMsg path.
bool NeighSync::resyncLinkLocalNeighbors(const string &interface)
{
    const unsigned int ifindex = if_nametoindex(interface.c_str());
    if (ifindex == 0)
    {
        SWSS_LOG_ERROR("Unable to resolve interface '%s' for link-local neighbor replay: %s",
                       interface.c_str(), strerror(errno));
        return false;
    }

    unique_ptr<nl_sock, NlSocketDeleter> socket(nl_socket_alloc());
    if (!socket)
    {
        SWSS_LOG_ERROR("Unable to allocate link-local neighbor dump socket for '%s'",
                       interface.c_str());
        return false;
    }
    int error = nl_connect(socket.get(), NETLINK_ROUTE);
    if (error < 0)
    {
        SWSS_LOG_ERROR("Unable to connect link-local neighbor dump socket for '%s': %s",
                       interface.c_str(), nl_geterror(error));
        return false;
    }

    unique_ptr<nl_msg, decltype(&nlmsg_free)> request(nlmsg_alloc(), nlmsg_free);
    if (!request)
    {
        SWSS_LOG_ERROR("Unable to allocate link-local neighbor dump request for '%s'",
                       interface.c_str());
        return false;
    }

    auto *header = nlmsg_put(request.get(), NL_AUTO_PORT, NL_AUTO_SEQ, RTM_GETNEIGH,
                             sizeof(struct ndmsg), NLM_F_REQUEST | NLM_F_DUMP);
    if (!header)
    {
        SWSS_LOG_ERROR("Unable to initialize link-local neighbor dump request for '%s'",
                       interface.c_str());
        return false;
    }

    auto *neighborMessage = static_cast<struct ndmsg *>(NLMSG_DATA(header));
    memset(neighborMessage, 0, sizeof(*neighborMessage));
    neighborMessage->ndm_family = AF_INET6;
    if (nla_put_u32(request.get(), NDA_IFINDEX, ifindex) < 0)
    {
        SWSS_LOG_ERROR("Unable to add ifindex %u to link-local neighbor dump request for '%s'",
                       ifindex, interface.c_str());
        return false;
    }

    error = nl_send_auto(socket.get(), request.get());
    if (error < 0)
    {
        SWSS_LOG_ERROR("Unable to send link-local neighbor dump request for '%s' (ifindex %u): %s",
                       interface.c_str(), ifindex, nl_geterror(error));
        return false;
    }

    LinkLocalDumpContext context{this, static_cast<int>(ifindex), header->nlmsg_seq};
    unique_ptr<nl_cb, decltype(&nl_cb_put)> callbacks(nl_cb_alloc(NL_CB_DEFAULT), nl_cb_put);
    if (!callbacks)
    {
        SWSS_LOG_ERROR("Unable to allocate link-local neighbor dump callbacks for '%s' (ifindex %u)",
                       interface.c_str(), ifindex);
        return false;
    }

    error = nl_cb_set(callbacks.get(), NL_CB_VALID, NL_CB_CUSTOM,
                      processLinkLocalDumpMessage, &context);
    if (error >= 0)
    {
        error = nl_cb_set(callbacks.get(), NL_CB_FINISH, NL_CB_CUSTOM,
                          finishLinkLocalDump, &context);
    }
    if (error >= 0)
    {
        error = nl_cb_err(callbacks.get(), NL_CB_CUSTOM, handleLinkLocalDumpError, &context);
    }
    if (error < 0)
    {
        SWSS_LOG_ERROR("Unable to configure link-local neighbor dump callbacks for '%s' "
                       "(ifindex %u): %s", interface.c_str(), ifindex, nl_geterror(error));
        return false;
    }

    while (!context.complete && context.error == 0)
    {
        struct pollfd descriptor = {nl_socket_get_fd(socket.get()), POLLIN, 0};
        int ready = poll(&descriptor, 1, LINK_LOCAL_DUMP_INACTIVITY_TIMEOUT_MS);
        if (ready == 0)
        {
            SWSS_LOG_ERROR("Timed out receiving link-local neighbor dump for '%s' (ifindex %u)",
                           interface.c_str(), ifindex);
            return false;
        }
        if (ready < 0)
        {
            SWSS_LOG_ERROR("Unable to poll link-local neighbor dump socket for '%s' "
                           "(ifindex %u): %s", interface.c_str(), ifindex, strerror(errno));
            return false;
        }

        error = nl_recvmsgs(socket.get(), callbacks.get());
        if (error < 0)
        {
            SWSS_LOG_ERROR("Unable to receive link-local neighbor dump for '%s' "
                           "(ifindex %u): %s", interface.c_str(), ifindex, nl_geterror(error));
            return false;
        }
    }

    if (context.error < 0)
    {
        const char *description = context.kernelError ? strerror(-context.error) : nl_geterror(context.error);
        SWSS_LOG_ERROR("Link-local neighbor dump failed for '%s' (ifindex %u): %s",
                       interface.c_str(), ifindex, description);
        return false;
    }
    if (!context.complete || context.interrupted)
    {
        SWSS_LOG_ERROR("Link-local neighbor dump was interrupted for '%s' (ifindex %u)",
                       interface.c_str(), ifindex);
        return false;
    }

    return true;
}

/*
 * Get interface/VRF name based on interface/VRF index
 * @arg if_index          Interface/VRF index
 * @arg if_name           String to store interface name
 * @arg name_len          Length of destination string, including terminating zero byte
 *
 * Return true if we successfully gets the interface/VRF name.
 */
bool NeighSync::getIfName(int if_index, char *if_name, size_t name_len)
{
    if (!if_name || name_len == 0)
    {
        return false;
    }

    memset(if_name, 0, name_len);

    /* Cannot get interface name. Possibly the interface gets re-created. */
    if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
    {
        /* Trying to refill cache */
        nl_cache_refill(m_nl_sock, m_link_cache);
        if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
        {
            return false;
        }
    }

    return true;
}

void NeighSync::processCfgEvpnNvo()
{
    std::deque<KeyOpFieldsValuesTuple> entries;
    m_cfgEvpnNvoTable.pops(entries);

    for (const auto &entry : entries)
    {
        const std::string &op = kfvOp(entry);
        if (op == SET_COMMAND)
        {
            m_isEvpnNvoExist = true;
        }
        else if (op == DEL_COMMAND)
        {
            m_isEvpnNvoExist = false;
        }
    }
}


// Check if neighbor table is restored in kernel
bool NeighSync::isNeighRestoreDone()
{
    string value;

    m_stateNeighRestoreTable.hget("Flags", "restored", value);
    if (value == "true")
    {
        SWSS_LOG_NOTICE("neighbor table restore to kernel is done");
        return true;
    }
    return false;
}

void NeighSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char ipStr[MAX_ADDR_SIZE + 1] = {0};
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_neigh *neigh = (struct rtnl_neigh *)obj;
    string key;
    string family;
    string intfName;
    std::vector<std::string> peerSwitchKeys;
    m_cfgPeerSwitchTable.getKeys(peerSwitchKeys);
    bool is_dualtor = peerSwitchKeys.size() > 0;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
        return;

    if (rtnl_neigh_get_family(neigh) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_neigh_get_family(neigh) == AF_INET6)
        family = IPV6_NAME;
    else
        return;

    key+= LinkCache::getInstance().ifindexToName(rtnl_neigh_get_ifindex(neigh));
    intfName = key;
    key+= ":";

    /* Get the vrf name (only needed for the EVPN host-route cleanup path) */
    char master_name[IFNAMSIZ] = {0};
    if (m_isEvpnNvoExist)
    {
        int ifindex = rtnl_neigh_get_ifindex(neigh);
        if (ifindex > 0)
        {
            struct rtnl_link *link = rtnl_link_get(m_link_cache, ifindex);
            if (!link)
            {
                /* Trying to refill cache */
                nl_cache_refill(m_nl_sock, m_link_cache);
                link = rtnl_link_get(m_link_cache, ifindex);
            }

            if (link)
            {
                int master_index = rtnl_link_get_master(link);
                if (master_index)
                {
                    /* Get the name of the master device */
                    getIfName(master_index, master_name, IFNAMSIZ);
                }
                rtnl_link_put(link);
            }
        }
    }

    nl_addr2str(rtnl_neigh_get_dst(neigh), ipStr, MAX_ADDR_SIZE);

    /* Ignore IPv4 link-local addresses as neighbors if subtype is dualtor */
    IpAddress ipAddress(ipStr);
    if (family == IPV4_NAME && ipAddress.getAddrScope() == IpAddress::AddrScope::LINK_SCOPE && is_dualtor)
    {
        SWSS_LOG_INFO("Link Local address received on dualtor, ignoring for %s", ipStr);
        return;
    }

    /* Ignore IPv6 link-local addresses as neighbors, if ipv6 link local mode is disabled */
    if (family == IPV6_NAME && IN6_IS_ADDR_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
    {
        if ((isLinkLocalEnabled(intfName) == false) && (nlmsg_type != RTM_DELNEIGH))
        {
            SWSS_LOG_INFO("LinkLocal address received, ignoring for %s", ipStr);
            return;
        }
    }
    /* Ignore IPv6 multicast link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_MC_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
    {
        SWSS_LOG_INFO("Multicast LinkLocal address received, ignoring for %s", ipStr);
        return;
    }
    key+= ipStr;

    int state = rtnl_neigh_get_state(neigh);
    if (is_dualtor && family == IPV6_NAME)
    {
        if (nlmsg_type == RTM_NEWNEIGH && state == NUD_FAILED)
        {
            std::vector<FieldValueTuple> failedNeighFields = {
                FieldValueTuple("NULL", "NULL"),
            };
            m_kernelFailedNeighTable.set(key, failedNeighFields);
            SWSS_LOG_INFO("Published failed kernel neighbor '%s' for nbrmgrd processing", key.c_str());
        }
        else if (nlmsg_type == RTM_DELNEIGH ||
                 ((nlmsg_type == RTM_NEWNEIGH || nlmsg_type == RTM_GETNEIGH) &&
                  (state & VALID_NEIGH_STATES)))
        {
            std::vector<FieldValueTuple> failedNeighFields;
            if (m_kernelFailedNeighCheckTable.get(key, failedNeighFields))
            {
                m_kernelFailedNeighTable.del(key);
                SWSS_LOG_INFO("Removed resolved or deleted kernel neighbor '%s' from failed neighbor table",
                              key.c_str());
            }
        }
    }

    /* Ignore probe msg (EVPN only) */
    if (m_isEvpnNvoExist && (nlmsg_type == RTM_NEWNEIGH) && (state == NUD_PROBE))
    {
        return;
    }

    /* When EVPN NVO is not configured, preserve the original NUD_NOARP
     * handling: ignore NOARP neighbors unless they are externally learned. */
    if (!m_isEvpnNvoExist && (state == NUD_NOARP))
    {
        if (!(rtnl_neigh_get_flags(neigh) & NTF_EXT_LEARNED))
        {
            SWSS_LOG_INFO("NOARP address received, ignoring for %s", ipStr);
            return;
        }
    }

    SWSS_LOG_INFO("Get neighbor msg %s, state %d, type %d", ipStr, state, nlmsg_type);

    bool delete_key = false;
    bool use_zero_mac = false;
    if (is_dualtor && (state == NUD_INCOMPLETE || state == NUD_FAILED))
    {
        SWSS_LOG_INFO("Unable to resolve %s, setting zero MAC", key.c_str());
        use_zero_mac = true;

        // Unresolved neighbor deletion on dual ToR devices must be handled
        // separately, otherwise delete_key is never set to true
        // and neighorch is never able to remove the neighbor
        if (nlmsg_type == RTM_DELNEIGH)
        {
            delete_key = true;
        }
    }
    else if ((nlmsg_type == RTM_DELNEIGH) ||
             (state == NUD_INCOMPLETE) || (state == NUD_FAILED))
    {
        delete_key = true;
    }
    else if (m_isEvpnNvoExist && (state == NUD_NOARP))
    {
        /* NUD_NOARP with NTF_EXT_LEARNED means this is an EVPN-synced neighbor
         * (e.g., from RT-2 MAC/IP via FRR zebra). Keep it — don't delete.
         * NUD_NOARP without NTF_EXT_LEARNED means moved to remote — delete. */
        if (!(rtnl_neigh_get_flags(neigh) & NTF_EXT_LEARNED))
        {
            SWSS_LOG_INFO("NUD_NOARP without NTF_EXT_LEARNED, neighbor moved to remote for %s", ipStr);
            delete_key = true;
        }
        else
        {
            SWSS_LOG_INFO("NUD_NOARP with NTF_EXT_LEARNED (EVPN-synced), keeping neighbor for %s", ipStr);
        }
    }

    if (use_zero_mac)
    {
        std::string zero_mac = "00:00:00:00:00:00";
        strncpy(macStr, zero_mac.c_str(), zero_mac.length());
        macStr[zero_mac.length()] = '\0';
    }
    else
    {
        nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);
    }

    if (!delete_key && !strncmp(macStr, "none", MAX_ADDR_SIZE))
    {
        SWSS_LOG_NOTICE("Mac address is 'none' for ADD op, ignoring for %s", ipStr);
        return;
    }

    /* Ignore neighbor entries with Broadcast Mac - Trigger for directed broadcast */
    if (!delete_key && (MacAddress(macStr) == MacAddress("ff:ff:ff:ff:ff:ff")))
    {
        SWSS_LOG_INFO("Broadcast Mac received, ignoring for %s", ipStr);
        return;
    }

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("family", family);
    FieldValueTuple nh("neigh", macStr);
    fvVector.push_back(nh);
    fvVector.push_back(f);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_NEIGH_TABLE_NAME, key, fvVector, delete_key);
    }
    else
    {
        if (delete_key == true)
        {
            m_neighTable.del(key);
            return;
        }

        string hostRoute;
        /* EVPN only: always try to del the host route before add neighbor */
        if (m_isEvpnNvoExist && string(master_name).compare(0, 3, VRF_PREFIX) == 0)
        {
            hostRoute += master_name;
            hostRoute += ":";
            hostRoute += ipStr;

            SWSS_LOG_INFO("Remove host route before adding neighbor %s", hostRoute.c_str());
            m_routeTable.del(hostRoute);
        }

        m_neighTable.set(key, fvVector);
    }
}

/* To check the ipv6 link local is enabled on a given port */
bool NeighSync::isLinkLocalEnabled(const string &port)
{
    vector<FieldValueTuple> values;

    if (!port.compare(0, strlen("Vlan"), "Vlan"))
    {
        if (!m_cfgVlanInterfaceTable.get(port, values))
        {
            SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
            return false;
        }
    }
    else if (!port.compare(0, strlen("PortChannel"), "PortChannel"))
    {
        if (!m_cfgLagInterfaceTable.get(port, values))
        {
            SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
            return false;
        }
    }
    else if (!port.compare(0, strlen("Ethernet"), "Ethernet"))
    {
        if (!m_cfgInterfaceTable.get(port, values))
        {
            SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
            return false;
        }
    }
    else
    {
        SWSS_LOG_INFO("IPv6 Link local is not supported for %s ", port.c_str());
        return false;
    }

    auto it = std::find_if(values.begin(), values.end(), [](const FieldValueTuple& t){ return t.first == "ipv6_use_link_local_only";});
    if (it != values.end())
    {
        if (it->second == "enable")
        {
            SWSS_LOG_INFO("IPv6 Link local is enabled on %s", port.c_str());
            return true;
        }
    }

    SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
    return false;
}
