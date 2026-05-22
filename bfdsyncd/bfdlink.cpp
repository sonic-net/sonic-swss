#include <string.h>
#include <errno.h>
#include <system_error>
#include "logger.h"
#include "netmsg.h"
#include "netdispatcher.h"
#include "bfdsyncd/bfdlink.h"
#include <unistd.h>

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
    m_debug(debug),
    m_bufSize(BFD_MAX_MSG_LEN * 10),
    m_messageBuffer(NULL),
    m_pos(0),
    m_connected(false),
    m_server_up(false),
    m_bfdTable(db, APP_BFD_SESSION_TABLE_NAME),
    m_bfdStateTable(stateDb, STATE_BFD_SESSION_TABLE_NAME)
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
}

BfdLink::~BfdLink()
{
    delete[] m_messageBuffer;
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

std::string BfdLink::exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
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
            break;
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
    bool is_linklocal = false;
    char dst_addr[INET6_ADDRSTRLEN];
    char src_addr[INET6_ADDRSTRLEN];
    char ifname[IFNAME_LEN];
    string dst_mac;
    string src_mac;
    string cmd, dst_str;

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

    /* HW offload does not support counters, return 0 for counters here */
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
        msg.data.session_counters.control_input_packets = htobe64(0);
        msg.data.session_counters.control_output_packets = htobe64(0);
        msg.data.session_counters.echo_input_packets = htobe64(0);
        msg.data.session_counters.echo_output_packets = htobe64(0);

        memcpy(m_sendBuffer, &msg, msglen);

        SWSS_LOG_INFO("BFD_SESSION_COUNTERS send counters to bfdd, id %d, lid %u",  ntohs(msg.header.id), ntohl(msg.data.session_counters.lid));

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
        struct in_addr v4;
        sprintf(src_addr, "%s", inet_ntoa(*(struct in_addr *)&bmp->data.session.src));
        sprintf(dst_addr, "%s", inet_ntoa(*(struct in_addr *)&bmp->data.session.dst));
        /* check link local ip address 169.254.0.0/16 0xa9fe0000 */
        if ((inet_pton(AF_INET, dst_addr, &v4) == 1) && ((v4.s_addr & 0x0000ffff) == 0x0000fea9)) {
            is_linklocal = true;
            SWSS_LOG_INFO("dst_addr %s is a link local ip address", dst_addr);
        }
    }

    bfdkey = string("default:default:")+string(dst_addr);
    bfdkey_map = string("default|default|")+string(dst_addr);

    ifindex = ntohl(bm.data.session.ifindex);
    memcpy(&ifname, bm.data.session.ifname, IFNAME_LEN);

    /* for link-local address only */
    if (ifindex != 0) {
        bfdkey = string("default:")+string(ifname)+string(":")+string(dst_addr);
        bfdkey_map = string("default|")+string(ifname)+string("|")+string(dst_addr);
    }

    /* mac address is not needed for deletion, neighbor entry might be deleted already */
    if ((ifindex != 0) && (bm.header.type == DP_ADD_SESSION)) {
        /* get src mac address */
        src_mac = get_intf_mac(ifname);

        if (flags & SESSION_IPV6)
        {
            /* update ndp table */
            cmd = string("ping6 -c 3 ") + string(dst_addr) + string(" -I ") + string(ifname);
            SWSS_LOG_INFO("CMD: %s", cmd.c_str());
            exec(cmd.c_str());

            /* get dst mac address */
            cmd = string("ip -6 neighbor get ") + string(dst_addr) + string(" dev ") + string(ifname) + string(" | grep -o -E ..:..:..:..:..:..");
            SWSS_LOG_INFO("CMD: %s", cmd.c_str());
            dst_str = exec(cmd.c_str());
            if (dst_str.length() < 17) {
                SWSS_LOG_ERROR("mac address length is not correct: dst_mac %s ", dst_str.c_str());
                return;
            }
            dst_mac = dst_str.substr(0,17);
        }
        else
        {
            /* update arp table */
            if (is_linklocal) {
                SWSS_LOG_ERROR("IPv4 link-local is not supported!");
                return;
            } else {
                cmd = string("ping -c 3 ") + string(dst_addr) + string(" -I ") + string(ifname);
            }
            SWSS_LOG_INFO("CMD: %s", cmd.c_str());
            exec(cmd.c_str());

            /* get dst mac address */
            cmd = string("arp ") + string(dst_addr) + string(" | grep -o -E ..:..:..:..:..:..");
            SWSS_LOG_INFO("CMD: %s", cmd.c_str());
            dst_str = exec(cmd.c_str());
            if (dst_str.length() < 17) {
                SWSS_LOG_ERROR("mac address length is not correct: ip_address %s, dst_mac %s", dst_addr, dst_str.c_str());
                return;
            }
            dst_mac = dst_str.substr(0,17);
        }
        SWSS_LOG_INFO("dst_mac %s ,  src_mac %s", dst_mac.c_str(), src_mac.c_str());
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
                SWSS_LOG_WARN("bfd session key %s is already created, parameter changed, delete and recreate it.", bfdkey_map.c_str());
                m_bfdTable.del(bfdkey);
                m_key2bfd.erase(bfdkey_map);
                /* the symptom observed that redis eliminates consecutive del and add transaction sometime, get wrong result. need to wait to make sure deletion done */
                usleep(100000);
            }
            else
            {
                SWSS_LOG_WARN("bfd session key %s is already created, ignore duplicated creation.", bfdkey_map.c_str());
                /* in the case of duplicated creation, update lid here and update bfd state from redis state db to bfdd */
                it->second.data.session.lid = bm.data.session.lid;
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

    /* Specify both dst_mac and src_mac for inject-down */
    if (ifindex != 0) {
        FieldValueTuple d_mac("dst_mac", dst_mac.c_str());
        fvVector.push_back(d_mac);
        FieldValueTuple s_mac("src_mac", src_mac.c_str());
        fvVector.push_back(s_mac);
    }

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
    msg.header.length = ntohs(msglen);
    msg.header.type = ntohs(BFD_STATE_CHANGE);
    
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
            uint32_t rid = (uint32_t)strtoll(string(value).c_str(), NULL, 10);
            msg.data.state.rid = htonl(rid);
            SWSS_LOG_INFO("remote_discriminator %u ", rid);
        }
        /* Remote minimum receive interval. */
        if (field == "remote_min_rx")
        {
            uint32_t rx = (uint32_t)strtoll(string(value).c_str(), NULL, 10);
            msg.data.state.required_rx = htonl(rx);
            SWSS_LOG_INFO("remote_min_rx %u ", rx);
        }
        /* Remote minimum desired transmission interval. */
        if (field == "remote_min_tx")
        {
            uint32_t tx = (uint32_t)strtoll(string(value).c_str(), NULL, 10);
            msg.data.state.desired_tx = htonl(tx);
            SWSS_LOG_INFO("remote_min_tx %u ", tx);
        }
        /* Remote detection multiplier. */
        if (field == "remote_multiplier")
        {
            msg.data.state.detection_multiplier = (uint8_t)strtoll(string(value).c_str(), NULL, 10);
            SWSS_LOG_INFO("remote_multiplier %u", msg.data.state.detection_multiplier );
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
