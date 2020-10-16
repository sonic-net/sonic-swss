#include <string.h>
#include <errno.h>
#include <system_error>
#include "logger.h"
#include "netmsg.h"
#include "netdispatcher.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"

using namespace swss;
using namespace std;

extern RouteSync *g_routesync;

void netlink_parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta,
        int len)
{
    while (RTA_OK(rta, len)) 
    {
        if (rta->rta_type <= max)
        {
            tb[rta->rta_type] = rta;
        }
        rta = RTA_NEXT(rta, len);
    }
}

bool FpmLink::isRawProcessing(struct nlmsghdr *h)
{
    int len;
    short encap_type = 0;
    struct rtmsg *rtm;
    struct rtattr *tb[RTA_MAX + 1];

    rtm = (struct rtmsg *)NLMSG_DATA(h);

    if (h->nlmsg_type != RTM_NEWROUTE && h->nlmsg_type != RTM_DELROUTE)
    {
        return false;
    }

    len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg)));
    if (len < 0) 
    {
        return false;
    }

    memset(tb, 0, sizeof(tb));
    netlink_parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);

    if (!tb[RTA_MULTIPATH])
    {
        if (tb[RTA_ENCAP_TYPE])
        {
            encap_type = *(short *)RTA_DATA(tb[RTA_ENCAP_TYPE]);
        }
    }
    else
    {
        /* This is a multipath route */
        int len;            
        struct rtnexthop *rtnh = (struct rtnexthop *)RTA_DATA(tb[RTA_MULTIPATH]);
        len = (int)RTA_PAYLOAD(tb[RTA_MULTIPATH]);
        struct rtattr *subtb[RTA_MAX + 1];
        
        for (;;) 
        {
            if (len < (int)sizeof(*rtnh) || rtnh->rtnh_len > len)
            {
                break;
            }

            if (rtnh->rtnh_len > sizeof(*rtnh)) 
            {
                memset(subtb, 0, sizeof(subtb));
                netlink_parse_rtattr(subtb, RTA_MAX, RTNH_DATA(rtnh),
                                      (int)(rtnh->rtnh_len - sizeof(*rtnh)));
                if (subtb[RTA_ENCAP_TYPE])
                {
                    encap_type = *(uint16_t *)RTA_DATA(subtb[RTA_ENCAP_TYPE]);
                    break;
                }
            }

            if (rtnh->rtnh_len == 0)
            {
                break;
            }

            len -= NLMSG_ALIGN(rtnh->rtnh_len);
            rtnh = RTNH_NEXT(rtnh);                
        }
    }

    SWSS_LOG_INFO("Rx MsgType:%d Encap:%d", h->nlmsg_type, encap_type);

    if (encap_type > 0)
    {
        return true;
    }

    return false;
}

FpmLink::FpmLink(unsigned short port) :
    MSG_BATCH_SIZE(256),
    m_bufSize(FPM_MAX_MSG_LEN * MSG_BATCH_SIZE),
    m_messageBuffer(NULL),
    m_pos(0),
    m_connected(false),
    m_server_up(false)
{
    struct sockaddr_in addr;
    int true_val = 1;

    m_server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
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
}

FpmLink::~FpmLink()
{
    delete m_messageBuffer;
    if (m_connected)
        close(m_connection_socket);
    if (m_server_up)
        close(m_server_socket);
}

void FpmLink::accept()
{
    struct sockaddr_in client_addr;

    // Ref: man 3 accept
    // address_len argument, on input, specifies the length of the supplied sockaddr structure
    socklen_t client_len = sizeof(struct sockaddr_in);

    m_connection_socket = ::accept(m_server_socket, (struct sockaddr *)&client_addr,
                                   &client_len);
    if (m_connection_socket < 0)
        throw system_error(errno, system_category());

    SWSS_LOG_INFO("New connection accepted from: %s\n", inet_ntoa(client_addr.sin_addr));
}

int FpmLink::getFd()
{
    return m_connection_socket;
}

uint64_t FpmLink::readData()
{
    fpm_msg_hdr_t *hdr;
    size_t msg_len;
    size_t start = 0, left;
    ssize_t read;

    read = ::read(m_connection_socket, m_messageBuffer + m_pos, m_bufSize - m_pos);
    if (read == 0)
        throw FpmConnectionClosedException();
    if (read < 0)
        throw system_error(errno, system_category());
    m_pos+= (uint32_t)read;

    /* Check for complete messages */
    while (true)
    {
        hdr = reinterpret_cast<fpm_msg_hdr_t *>(static_cast<void *>(m_messageBuffer + start));
        left = m_pos - start;
        if (left < FPM_MSG_HDR_LEN)
            break;
        /* fpm_msg_len includes header size */
        msg_len = fpm_msg_len(hdr);
        if (left < msg_len)
            break;

        if (!fpm_msg_ok(hdr, left))
            throw system_error(make_error_code(errc::bad_message), "Malformed FPM message received");

        if (hdr->msg_type == FPM_MSG_TYPE_NETLINK)
        {
            bool isRaw = false;

            nlmsghdr *nl_hdr = (nlmsghdr *)fpm_msg_data(hdr);

            /*
             * EVPN Type5 Add Routes need to be process in Raw mode as they contain 
             * RMAC, VLAN and L3VNI information.
             * Where as all other route will be using rtnl api to extract information 
             * from the netlink msg.
           * */
            isRaw = isRawProcessing(nl_hdr);

            nl_msg *msg = nlmsg_convert(nl_hdr);
            if (msg == NULL)
            {
                throw system_error(make_error_code(errc::bad_message), "Unable to convert nlmsg");
            }

            nlmsg_set_proto(msg, NETLINK_ROUTE);

            if (isRaw)
            {
                /* EVPN Type5 Add route processing */
                g_routesync->onMsgRaw(nl_hdr);
            }
            else
            {
                NetDispatcher::getInstance().onNetlinkMessage(msg);
            }
            nlmsg_free(msg);
        }
        start += msg_len;
    }

    memmove(m_messageBuffer, m_messageBuffer + start, m_pos - start);
    m_pos = m_pos - (uint32_t)start;
    return 0;
}
