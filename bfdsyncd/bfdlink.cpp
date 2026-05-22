#include <string.h>
#include <errno.h>
#include <system_error>
#include "logger.h"
#include "converter.h"
#include "netmsg.h"
#include "netdispatcher.h"
#include "bfdsyncd/bfdlink.h"
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>

#include <arpa/inet.h>


extern "C" {
#include "sai.h"
#include "saistatus.h"
}

using namespace std;
using namespace swss;

/* Whitelist check for interface names received over the BFD DP socket.
 * The value is later used to compose redis keys, so reject anything
 * outside the Linux ifname character set defensively. */
static bool is_valid_ifname(const char *name)
{
    static const size_t kMaxIfname = 15;  /* Linux IFNAMSIZ - 1 */
    size_t len = strnlen(name, kMaxIfname + 1);
    if (len == 0 || len > kMaxIfname) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static const char *bfd_dplane_messagetype2str(enum bfddp_message_type bmt)
{
	switch (bmt) {
	case ECHO_REQUEST:
		return "ECHO_REQUEST";
	case ECHO_REPLY:
		return "ECHO_REPLY";
	case DP_ADD_SESSION:
		return "DP_ADD_SESSION";
	case DP_DELETE_SESSION:
		return "DP_DELETE_SESSION";
	case BFD_STATE_CHANGE:
		return "BFD_STATE_CHANGE";
	case DP_REQUEST_SESSION_COUNTERS:
		return "DP_REQUEST_SESSION_COUNTERS";
	case BFD_SESSION_COUNTERS:
		return "BFD_SESSION_COUNTERS";
	default:
		return "UNKNOWN";
	}
}

BfdLink::BfdLink(DBConnector *db, DBConnector *stateDb, unsigned short port, int debug) :
    /* Init list ordered to match member declaration order in bfdlink.h
     * (C++ initializes in declaration order regardless of init-list
     * order, so a mismatch produces a -Wreorder warning and, more
     * importantly, leaves any member not in the init list with an
     * indeterminate value if the constructor body is later refactored
     * to read it before assigning. -1 sentinels for socket fds keep the
     * destructor's `if (m_connected) close(...)` paths well-defined
     * even if construction throws partway through. */
    m_messageBuffer(nullptr),
    m_bfdTable(db, APP_BFD_SESSION_TABLE_NAME),
    m_bfdStateTable(stateDb, STATE_BFD_SESSION_TABLE_NAME),
    m_bufSize(BFD_MAX_MSG_LEN * 10),
    m_sendBuffer(nullptr),
    m_pos(0),
    m_connected(false),
    m_server_up(false),
    m_debug(debug),
    m_server_socket(-1),
    m_connection_socket(-1)
{
    struct sockaddr_in addr;
    int true_val = 1;

    m_server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_server_socket < 0)
        throw system_error(errno, system_category());

    if (setsockopt(m_server_socket, SOL_SOCKET, SO_REUSEADDR, &true_val,
                   sizeof(true_val)) < 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    if (setsockopt(m_server_socket, SOL_SOCKET, SO_KEEPALIVE, &true_val,
                   sizeof(true_val)) < 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(m_server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    if (listen(m_server_socket, 2) != 0)
    {
        close(m_server_socket);
        throw system_error(errno, system_category());
    }

    m_server_up = true;
    m_messageBuffer = new char[m_bufSize];
    m_sendBuffer = new char[m_bufSize];

    /* Open COUNTERS_DB lazily so a transient failure here doesn't take
     * down the BFDDP control path. If construction throws, leave the
     * unique_ptrs null and the DP_REQUEST_SESSION_COUNTERS handler
     * falls back to returning 0 (existing behavior). The
     * BFD_SESSION_STAT FLEX_COUNTER group has to be explicitly enabled
     * by the operator anyway, so the name-map lookup will routinely
     * miss in production until that's done. */
    try
    {
        m_countersDb = std::unique_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));
        m_bfdSessionNameMap = std::unique_ptr<Table>(
            new Table(m_countersDb.get(), COUNTERS_BFD_SESSION_NAME_MAP));
        m_bfdCountersTable = std::unique_ptr<Table>(
            new Table(m_countersDb.get(), COUNTERS_TABLE));
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_WARN("BfdLink: COUNTERS_DB unavailable, falling back to "
                      "zero-valued counters in DP_REQUEST_SESSION_COUNTERS replies: %s",
                      e.what());
        m_countersDb.reset();
        m_bfdSessionNameMap.reset();
        m_bfdCountersTable.reset();
    }
}

BfdLink::~BfdLink()
{
    delete[] m_messageBuffer;
    delete[] m_sendBuffer;
    if (m_connected)
        close(m_connection_socket);
    if (m_server_up)
        close(m_server_socket);
}

std::string BfdLink::get_intf_mac(const char* intf)
{
    std::string mac;
    std::string path;
    std::ifstream netfile;
    path = "/sys/class/net/" + string(intf) + "/address";
    netfile.open(path);
    std::getline(netfile, mac);
    netfile.close();
    return mac;
}

bool BfdLink::sendmsg(uint16_t msglen) {
    size_t sent = 0;
    while (sent != msglen)
    {
        auto rc = ::send(m_connection_socket, m_sendBuffer + sent, msglen - sent, 0);
        if (rc == -1)
        {
            SWSS_LOG_ERROR("Failed to send BFD state or counter message: %s", strerror(errno));
            return false;
        }
        sent += rc;
    }
    return true;
}

void BfdLink::accept()
{
    struct sockaddr_in client_addr;

    /* Ref: man 3 accept */
    /* address_len argument, on input, specifies the length of the supplied sockaddr structure */
    socklen_t client_len = sizeof(struct sockaddr_in);

    m_connection_socket = ::accept(m_server_socket, (struct sockaddr *)&client_addr,
                                   &client_len);
    if (m_connection_socket < 0)
        throw system_error(errno, system_category());

    SWSS_LOG_WARN("New connection accepted from: %s\n", inet_ntoa(client_addr.sin_addr));
}

int BfdLink::getFd()
{
    return m_connection_socket;
}

void BfdLink::hexdump(void *ptr, int buflen)
{
    char str[100];
    m_printbuf[0]=0;
    unsigned char *buf = (unsigned char*)ptr;
    int i, j;
    for (i=0; i<buflen; i+=16) {
        snprintf(str, 100, "%06x: ", i);
        strcat(m_printbuf, str);
        for (j=0; j<16; j++)
        {
            if (i+j < buflen)
                snprintf(str, 100, "%02x ", buf[i+j]);
            else
                snprintf(str, 100, "   ");
            strcat(m_printbuf, str);
        }
        snprintf(str, 100, " ");
        strcat(m_printbuf, str);
        for (j=0; j<16; j++)
        {
            if (i+j < buflen)
            {
                snprintf(str, 100, "%c", isprint(buf[i+j]) ? buf[i+j] : '.');
                strcat(m_printbuf, str);
            }
        }
        snprintf(str, 100, "\n");
        strcat(m_printbuf, str);
        SWSS_LOG_INFO("%s", m_printbuf);
        m_printbuf[0]=0;
    }
}

uint64_t BfdLink::readData()
{
    bfd_msg_hdr_t *hdr;
    size_t msg_len;
    size_t start = 0, left;
    ssize_t read;

    SWSS_LOG_INFO("Received BFD DP message. m_bufSize %d, pos %d", m_bufSize, m_pos);
    read = ::read(m_connection_socket, m_messageBuffer + m_pos, m_bufSize - m_pos);
    if (read == 0)
    {
        SWSS_LOG_ERROR("read BFD DP message, read=0");
        throw BfdConnectionClosedException();
    }
    if (read < 0)
    {
        SWSS_LOG_ERROR("read BFD DP message, read<0");
        throw system_error(errno, system_category());
    }

    if (m_debug) 
    {
        this->hexdump(m_messageBuffer, (int)read);
    }

    m_pos+= (uint32_t)read;
    SWSS_LOG_INFO("updated pos %d", m_pos);

    /* Check for complete messages */
    while (true)
    {
        hdr = reinterpret_cast<bfd_msg_hdr_t *>(static_cast<void *>(m_messageBuffer + start));
        left = m_pos - start;
        if (left < BFD_MSG_HDR_LEN)
            break;

        msg_len = bfd_msg_len(hdr);
        if (left < msg_len)
        {
            break;
        }

        if (!bfd_msg_ok(hdr))
        {
            /* Malformed header: unknown type, or msg_len outside
             * [BFD_MSG_HDR_LEN, BFD_MAX_MSG_LEN]. We cannot trust msg_len
             * to advance past this frame — the next bytes after a corrupt
             * header could be anything, including more bad framing — so
             * flush the entire buffer rather than re-parse the same garbage
             * forever. The previous implementation `break`'d without
             * advancing `start`, which left the bad bytes at the head of
             * the buffer; on the next readData() the same header parsed
             * the same way, looping until the socket buffer filled and
             * the daemon stopped processing valid messages — a single-byte
             * DoS. Resetting m_pos to 0 trades a small window of dropped
             * (possibly valid) frames for guaranteed forward progress. */
            SWSS_LOG_ERROR("malformed BFD DP message: ver=%d type=%d msg_len=%zu, "
                           "flushing %u bytes of buffer to recover",
                           hdr->version, ntohs(hdr->type), bfd_msg_len(hdr),
                           m_pos - (uint32_t)start);
            if (m_debug)
            {
                this->hexdump(m_messageBuffer + start, (int)(m_pos - start));
            }
            m_pos = 0;
            return 0;
        }

        this->handleBfdDpMessage(start);

        start += msg_len;
    }

    memmove(m_messageBuffer, m_messageBuffer + start, m_pos - start);
    if (m_pos > start)
    {
        m_pos = m_pos - (uint32_t)start;
    }
    else
    {
        m_pos = 0;
    }
    return 0;
}

void BfdLink::handleBfdDpMessage(size_t start)
{
    bfddp_message *bmp;
    bfddp_message bm ={};
    size_t msg_len;
    uint32_t flags;
    uint32_t lid;
    uint32_t ifindex;
    uint32_t rx_int;
    uint32_t tx_int;
    string  bfdkey = "";
    string  bfdkey_map = "";
    bool add = true;
    bool multihop = true;
    char dst_addr[INET6_ADDRSTRLEN];
    char src_addr[INET6_ADDRSTRLEN];
    char ifname[IFNAME_LEN];

    bmp = reinterpret_cast<bfddp_message *>(static_cast<void *>(m_messageBuffer+start));

    auto type = ntohs(bmp->header.type);
    msg_len = bfd_msg_len(&bmp->header);

    if (!bfd_msg_ok(&bmp->header))
    {
        SWSS_LOG_ERROR("received an invalid BFD DP message, ver %d, type %s, msg_len %ld", bmp->header.version, bfd_dplane_messagetype2str((bfddp_message_type)type), msg_len);
        return;
    }

    SWSS_LOG_INFO("bfd dp message, ver %d, type %s, msg_len %ld", bmp->header.version, bfd_dplane_messagetype2str((bfddp_message_type)type), msg_len);

    if ((type != DP_ADD_SESSION) && (type != DP_DELETE_SESSION) && (type != DP_REQUEST_SESSION_COUNTERS))
    {
        SWSS_LOG_ERROR("BFD_DP supports DP_ADD_SESSION/DP_DELETE_SESSION/DP_REQUEST_SESSION_COUNTERS type only, received message type %s", bfd_dplane_messagetype2str((bfddp_message_type)type));
        return;
    }

    memcpy(&bm, bmp, sizeof(bm));

    /* DP_REQUEST_SESSION_COUNTERS: bfdd polls per-session counters for
     * `vtysh -c "show bfd peers"`. We forward real values from
     * COUNTERS_DB when the BFD_SESSION_STAT FLEX_COUNTER group is
     * enabled (BfdOrch::addBfdSessionToFlexCounter populates the
     * COUNTERS_BFD_SESSION_NAME_MAP and syncd polls
     * SAI_BFD_SESSION_STAT_{IN,OUT,DROP}_PACKETS into COUNTERS:<oid>
     * every BFD_SESSION_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS=10s).
     * If polling isn't enabled, the name-map miss leaves us with 0 —
     * the cosmetic-only fallback. Echo counters stay 0 because BFD
     * echo mode isn't part of the HW offload data path. */
    if (type == DP_REQUEST_SESSION_COUNTERS)
    {
        struct bfddp_message msg = {};
        uint16_t msglen = sizeof(msg.header) + sizeof(msg.data.session_counters);

        /* Message header. don't need to do hton for the data from bm. for the counters, need htobe64*/
        msg.header.version = BFD_DP_VERSION;
        msg.header.length = htons(msglen);
        msg.header.type = htons(BFD_SESSION_COUNTERS);
        msg.header.id = bm.header.id;
        msg.data.session_counters.lid = bm.data.counters_req.lid;

        uint64_t in_pkts = 0;
        uint64_t out_pkts = 0;
        const uint32_t requested_lid = ntohl(bm.data.counters_req.lid);
        const auto lid_it = m_lid2appkey.find(requested_lid);
        if (lid_it != m_lid2appkey.end() && m_bfdSessionNameMap && m_bfdCountersTable)
        {
            std::string sai_oid;
            if (m_bfdSessionNameMap->hget("", lid_it->second, sai_oid) && !sai_oid.empty())
            {
                std::string val;
                if (m_bfdCountersTable->hget(sai_oid, "SAI_BFD_SESSION_STAT_IN_PACKETS", val))
                {
                    try { in_pkts = std::stoull(val); }
                    catch (const std::exception &) { in_pkts = 0; }
                }
                if (m_bfdCountersTable->hget(sai_oid, "SAI_BFD_SESSION_STAT_OUT_PACKETS", val))
                {
                    try { out_pkts = std::stoull(val); }
                    catch (const std::exception &) { out_pkts = 0; }
                }
            }
        }
        msg.data.session_counters.control_input_packets = htobe64(in_pkts);
        msg.data.session_counters.control_output_packets = htobe64(out_pkts);
        msg.data.session_counters.echo_input_packets = htobe64(0);
        msg.data.session_counters.echo_output_packets = htobe64(0);

        memcpy(m_sendBuffer, &msg, msglen);

        SWSS_LOG_INFO("BFD_SESSION_COUNTERS send counters to bfdd, id %d, lid %u, in_pkts %llu, out_pkts %llu",
                      ntohs(msg.header.id), requested_lid,
                      (unsigned long long)in_pkts, (unsigned long long)out_pkts);

        sendmsg(msglen);
        return;
    }

    bm.header.type = type;
    bm.header.length = (uint16_t)msg_len;

    flags=ntohl(bmp->data.session.flags);
    bm.data.session.flags = flags;

    multihop = flags & SESSION_MULTIHOP;
    if (flags & SESSION_IPV6) 
    {
        struct in6_addr v6;
        v6 = bm.data.session.src;
        if (inet_ntop(AF_INET6, &v6, src_addr, sizeof(src_addr)) == NULL) {
            SWSS_LOG_ERROR("Invalid src ip6 address");
            return;
        }
        v6 = bm.data.session.dst;
        if (inet_ntop(AF_INET6, &v6, dst_addr, sizeof(dst_addr)) == NULL) {
            SWSS_LOG_ERROR("Invalid dst ip6 address");
            return;
        }
    }
    else
    {
        if (inet_ntop(AF_INET, &bmp->data.session.src, src_addr, sizeof(src_addr)) == NULL) {
            SWSS_LOG_ERROR("Invalid src ip4 address");
            return;
        }
        if (inet_ntop(AF_INET, &bmp->data.session.dst, dst_addr, sizeof(dst_addr)) == NULL) {
            SWSS_LOG_ERROR("Invalid dst ip4 address");
            return;
        }
        /* IPv4 link-local detection (169.254.0.0/16) is not needed any more
         * — we refuse all interface-bound (ifindex != 0) DP_ADD_SESSION
         * below regardless of address family. */
    }

    bfdkey = string("default:default:")+string(dst_addr);
    bfdkey_map = string("default|default|")+string(dst_addr);

    ifindex = ntohl(bm.data.session.ifindex);
    /* Force NUL-termination — bm.data.session.ifname is a fixed 64-byte
     * field with no terminator guarantee, and string(ifname) below would
     * otherwise walk off the end of the stack buffer. */
    memcpy(ifname, bm.data.session.ifname, IFNAME_LEN - 1);
    ifname[IFNAME_LEN - 1] = '\0';

    /* Genuinely link-local destinations (IPv6 fe80::/10, IPv4
     * 169.254.0.0/16) cannot appear as routable next hops in any FIB
     * by RFC, so SAI's FIB-based BFD path has nothing to resolve and
     * the session would silently produce no wire traffic. Refuse those
     * here.
     *
     * Numbered interface-bound peers (e.g. "neighbor 10.0.0.169
     * update-source Ethernet208") are routable — broadcom-sai-sdk
     * commit 2d23b640 installs /32 host routes for each neighbor into
     * the SAI Route TRIE, so FIB lookup succeeds. Forward those with
     * a default-alias key; SAI will handle next-hop resolution via
     * FIB and ignore the port attribute (the DNX BFD PD driver does
     * not implement inject-down — it relies on FIB lookup of dst_ip
     * regardless of HW_LOOKUP_VALID, see brcm_sai_bfd.c).
     *
     * DP_DELETE_SESSION on a stale link-local row keeps ifname in
     * the key so the right state-db row gets cleaned up. */
    bool is_link_local = false;
    if (flags & SESSION_IPV6) {
        struct in6_addr v6_dst = bm.data.session.dst;
        is_link_local = IN6_IS_ADDR_LINKLOCAL(&v6_dst);
    } else {
        uint32_t v4 = ntohl(*(const uint32_t *)&bmp->data.session.dst);
        is_link_local = (v4 & 0xFFFF0000U) == 0xA9FE0000U;  /* 169.254/16 */
    }

    if (ifindex != 0) {
        if (!is_valid_ifname(ifname)) {
            SWSS_LOG_ERROR("rejecting BFD DP message: invalid ifname (ifindex=%u, type=%s)",
                           ifindex, bfd_dplane_messagetype2str((bfddp_message_type)type));
            return;
        }
        if (is_link_local && bm.header.type == DP_ADD_SESSION) {
            SWSS_LOG_ERROR("rejecting BFD DP_ADD_SESSION on link-local peer "
                           "(ifindex=%u, ifname=%s, dst=%s): SAI BFD path "
                           "requires a FIB route which link-local addresses "
                           "cannot have. Use a numbered peer or set "
                           "SYSTEM_DEFAULTS|software_bfd|status=enabled.",
                           ifindex, ifname, dst_addr);
            return;
        }
        if (is_link_local) {
            /* DELETE for a previously-installed link-local row — keep
             * ifname in key so the matching state-db row is removed. */
            bfdkey = string("default:")+string(ifname)+string(":")+string(dst_addr);
            bfdkey_map = string("default|")+string(ifname)+string("|")+string(dst_addr);
        } else {
            /* Numbered interface-bound peer — route via default alias.
             * SAI uses FIB lookup of dst_ip; the port attr is ignored. */
            SWSS_LOG_NOTICE("BFD session for %s reported interface-bound "
                            "(ifname=%s, ifindex=%u); routing as default-alias "
                            "session via FIB lookup.",
                            dst_addr, ifname, ifindex);
        }
    }

    if (bm.header.type == DP_ADD_SESSION) {
        std::map<std::string, bfddp_message>::iterator it;
        SWSS_LOG_INFO("bfd session lookup key %s ", bfdkey_map.c_str());
        it = m_key2bfd.find(bfdkey_map);
        if (it != m_key2bfd.end())
        {
            /* check if there is any timing parameter change. return if not */
            bool changed = false;
            if (it->second.data.session.min_rx != ntohl(bmp->data.session.min_rx))
            {
                changed = true;
            }
            if (it->second.data.session.min_tx != ntohl(bmp->data.session.min_tx))
            {
                changed = true;
            };
            if (it->second.data.session.detect_mult != bmp->data.session.detect_mult)
            {
                changed = true;
            };
            if (changed) {
                /* Timer parameters changed (BFD P-bit poll sequence resolved
                 * to new MIN_TX/MIN_RX/multiplier). Fall through and re-emit
                 * the row via m_bfdTable.set(); BfdOrch detects SET on an
                 * existing key and routes to update_bfd_session(), which
                 * pushes the new values through SAI set_bfd_session_attribute
                 * without flapping the offloaded session.
                 *
                 * The previous implementation issued del+set with a 100ms
                 * sleep in between to defeat ProducerStateTable's per-key
                 * coalescing — racy, blocking, and unnecessary now that
                 * BfdOrch supports in-place timer updates. */
                SWSS_LOG_NOTICE("bfd session key %s timer params changed, updating in place", bfdkey_map.c_str());
            }
            else
            {
                SWSS_LOG_WARN("bfd session key %s is already created, ignore duplicated creation.", bfdkey_map.c_str());
                /* in the case of duplicated creation, update lid here and update bfd state from redis state db to bfdd */
                /* Keep m_lid2appkey in sync — the cached lid may change here
                 * (e.g. bgp/bfdd restart re-allocates lid for the same
                 * session); without this, counter requests for the new
                 * lid would miss the name-map lookup. */
                m_lid2appkey.erase(ntohl(it->second.data.session.lid));
                it->second.data.session.lid = bm.data.session.lid;
                m_lid2appkey[ntohl(bm.data.session.lid)] = bfdkey;
                bfdStateUpdate(bfdkey_map);
                return;
            }
        }
    }

    lid = bm.data.session.lid;
    SWSS_LOG_INFO("add key %s local discriminator 0x%08x to lookup table", bfdkey.c_str(), lid);

    bm.data.session.min_rx = ntohl(bmp->data.session.min_rx);
    bm.data.session.min_tx = ntohl(bmp->data.session.min_tx);

    rx_int = ntohl(bmp->data.session.min_rx)/1000;
    tx_int = ntohl(bmp->data.session.min_tx)/1000;

    vector<FieldValueTuple> fvVector;
    FieldValueTuple mh("multihop", multihop?"true":"false");
    fvVector.push_back(mh);

    FieldValueTuple la("local_addr", src_addr);
    fvVector.push_back(la);

    /* dst_mac / src_mac (inject-down attrs) are no longer emitted. The
     * only path that ever populated them was DP_ADD_SESSION with
     * ifindex != 0, which is now refused above with a clear error log
     * because Broadcom DNX BFD HW offload doesn't implement the
     * inject-down (HW_LOOKUP_VALID=false) silicon path. */

    /* let bfdorch use default value if the following parameters are not provided */
    if (rx_int != 0) 
    {
        FieldValueTuple rx("rx_interval", to_string(rx_int));
        fvVector.push_back(rx);
    }
    if (tx_int != 0) 
    {
        FieldValueTuple tx("tx_interval", to_string(tx_int));
        fvVector.push_back(tx);
    }
    if (bm.data.session.detect_mult != 0)
    {
        FieldValueTuple detect_mult("multiplier", to_string(bm.data.session.detect_mult));
        fvVector.push_back(detect_mult);
    }


    if (bm.header.type == DP_ADD_SESSION)
    {
        m_key2bfd[bfdkey_map] = bm;

        /* Track lid -> APP_DB-key for the counters request path. The
         * stored lid is in host byte order so the comparison in the
         * DP_REQUEST_SESSION_COUNTERS handler doesn't have to repeat
         * the ntohl() on every poll. */
        m_lid2appkey[ntohl(bm.data.session.lid)] = bfdkey;

        /* in the case of bgp container restart,
         * bgp creates bfd session again but bfdorch does not create bfd session again,
         * update bfd state from redis state db to bfdd here
         */
        bfdStateUpdate(bfdkey_map);

        m_bfdTable.set(bfdkey, fvVector);
        SWSS_LOG_INFO("add key %s  to appl DB", bfdkey.c_str());
    }
    else if (bm.header.type == DP_DELETE_SESSION)
    {
        m_bfdTable.del(bfdkey);
        m_key2bfd.erase(bfdkey_map);
        m_lid2appkey.erase(ntohl(bm.data.session.lid));
        add = false;
        SWSS_LOG_INFO("delete key %s from appl DB", bfdkey.c_str());
    }

    SWSS_LOG_NOTICE("BfdTable op %s key: %s local_addr:%s multihop:%s rx_interval:%d tx_interval:%d ifindex:%d ifname:%s ",
                   add?"add":"del", bfdkey.c_str(), src_addr, multihop?"true":"false", rx_int, tx_int, ifindex, ifname);
    if (m_debug) 
    {
        this->bfdDebugMessage(&bm);
    }

}

void BfdLink::bfdDebugMessage(struct bfddp_message *bm)
{
    uint32_t flags;

    SWSS_LOG_INFO("ver %d", bm->header.version);
    SWSS_LOG_INFO("type %s", bfd_dplane_messagetype2str((bfddp_message_type)bm->header.type));

    flags=bm->data.session.flags;
    SWSS_LOG_INFO("flag 0x%08x", flags);
    SWSS_LOG_INFO("local discriminator 0x%08x", bm->data.session.lid);
    SWSS_LOG_INFO("ttl %d", bm->data.session.ttl);
    SWSS_LOG_INFO("detect_mult %d", bm->data.session.detect_mult);
    SWSS_LOG_INFO("rx_interval %d", bm->data.session.min_rx);
    SWSS_LOG_INFO("tx_interval %d", bm->data.session.min_tx);
    SWSS_LOG_INFO("multihop %d", flags & SESSION_MULTIHOP);

    if (flags & SESSION_IPV6) 
    {
        struct in6_addr v6;
        char str[INET6_ADDRSTRLEN];

        v6 = bm->data.session.src;
        if (inet_ntop(AF_INET6, &v6, str, sizeof(str)) != NULL) {
            SWSS_LOG_INFO("src %s", str);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid src ip6 address");
        }
        v6 = bm->data.session.dst;
        if (inet_ntop(AF_INET6, &v6, str, sizeof(str)) != NULL) {
            SWSS_LOG_INFO("dst %s", str);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid dst ip6 address");
        }
    }
    else
    {
        SWSS_LOG_INFO("src %s", inet_ntoa(*(struct in_addr *)&bm->data.session.src));
        SWSS_LOG_INFO("dst %s", inet_ntoa(*(struct in_addr *)&bm->data.session.dst));
    }

}

void BfdLink::bfdStateUpdate(std::string key)
{
    std::vector<swss::FieldValueTuple> fvs;
    m_bfdStateTable.get(key, fvs);
    for (auto fv: fvs)
    {
        if (fvField(fv) == "state")
        {
            SWSS_LOG_INFO("key %s found in state db, update state %s to bfdd", key.c_str(), string(fvValue(fv)).c_str());
            handleBfdStateUpdate(key, fvs);
            break;
        }
    }
}

bool BfdLink::handleBfdStateUpdate(std::string k, const std::vector<swss::FieldValueTuple> &fvs)
{
    struct in_addr inaddr;
    struct in6_addr in6addr;
    char buf6[INET6_ADDRSTRLEN];

    std::string key;
    std::string s = k;
    size_t pos = s.find("|");
    std::string vrf = s.substr(0, pos);
    s.erase(0, pos+1);
    pos = s.find("|");
    std::string intf = s.substr(0, pos);
    s.erase(0, pos+1);
    std::string ip = s;

    if (inet_pton(AF_INET6, ip.c_str(), &in6addr) == 1) /* success! */
    {
        if (inet_ntop(AF_INET6, &in6addr, buf6, sizeof(buf6)) != NULL)
        {
            key = vrf + string("|") + intf+string("|") + string(buf6);
        }
        else
        {
            SWSS_LOG_ERROR("inet_ntop error, ipv6 address %s ", ip.c_str());
            return false;
        }
    }
    else if (inet_pton(AF_INET, ip.c_str(), &inaddr) == 1) /* success! */
    {
        key = k;
    }
    else
    {
        SWSS_LOG_ERROR("invalid ip address:  %s ", ip.c_str());
        return false;
    };
    std::map<std::string, bfddp_message>::iterator it;
    SWSS_LOG_INFO("lookup key %s ", key.c_str());
    it = m_key2bfd.find(key);
    if (it == m_key2bfd.end())
    {
        SWSS_LOG_INFO("key %s not found", key.c_str());
        return false;
    }
    auto session = it->second.data.session;

    struct bfddp_message msg = {};
    uint16_t msglen = sizeof(msg.header) + sizeof(msg.data.state);
    uint8_t state = STATE_DOWN;

    /* Message header. */
    msg.header.version = BFD_DP_VERSION;
    msg.header.length = htons(msglen);
    msg.header.type = htons(BFD_STATE_CHANGE);
    
    /* Message payload. */

    /* HW local_discriminator is different from frr/bfd local discriminator, do not use local discriminator from state db */
    msg.data.state.lid = session.lid;

    for (const auto& fv: fvs)
    {
        const auto& field = fvField(fv);
        const auto& value = fvValue(fv);
        if (field == "state")
        {
            if (value == "Up")
            {
                state = STATE_UP;
            }
            else if (value == "Down")
            {
                state = STATE_DOWN;
            }
        }
        /* Remote discriminator. */
        if (field == "remote_discriminator")
        {
            try
            {
                uint32_t rid = swss::to_uint<uint32_t>(value);
                msg.data.state.rid = htonl(rid);
                SWSS_LOG_INFO("remote_discriminator %u ", rid);
            }
            catch (const std::exception &e)
            {
                SWSS_LOG_ERROR("BFD STATE_DB %s: malformed remote_discriminator '%s': %s",
                               k.c_str(), value.c_str(), e.what());
                return false;
            }
        }
        /* Remote minimum receive interval. */
        if (field == "remote_min_rx")
        {
            try
            {
                uint32_t rx = swss::to_uint<uint32_t>(value);
                msg.data.state.required_rx = htonl(rx);
                SWSS_LOG_INFO("remote_min_rx %u ", rx);
            }
            catch (const std::exception &e)
            {
                SWSS_LOG_ERROR("BFD STATE_DB %s: malformed remote_min_rx '%s': %s",
                               k.c_str(), value.c_str(), e.what());
                return false;
            }
        }
        /* Remote minimum desired transmission interval. */
        if (field == "remote_min_tx")
        {
            try
            {
                uint32_t tx = swss::to_uint<uint32_t>(value);
                msg.data.state.desired_tx = htonl(tx);
                SWSS_LOG_INFO("remote_min_tx %u ", tx);
            }
            catch (const std::exception &e)
            {
                SWSS_LOG_ERROR("BFD STATE_DB %s: malformed remote_min_tx '%s': %s",
                               k.c_str(), value.c_str(), e.what());
                return false;
            }
        }
        /* Remote detection multiplier. */
        if (field == "remote_multiplier")
        {
            try
            {
                msg.data.state.detection_multiplier = swss::to_uint<uint8_t>(value);
                SWSS_LOG_INFO("remote_multiplier %u", msg.data.state.detection_multiplier);
            }
            catch (const std::exception &e)
            {
                SWSS_LOG_ERROR("BFD STATE_DB %s: malformed remote_multiplier '%s': %s",
                               k.c_str(), value.c_str(), e.what());
                return false;
            }
        }

    }
    msg.data.state.state = state;
    SWSS_LOG_INFO("lookup key %s, state %d ", key.c_str(), state);

    if (msglen > m_bufSize)
    {
        /* should no be reached here */
        SWSS_LOG_THROW("Message length %d is greater than the send buffer size %d", msglen, m_bufSize);
    }

    memcpy(m_sendBuffer, &msg, msglen);

    return sendmsg(msglen);

}
