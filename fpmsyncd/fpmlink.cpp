#include <string.h>
#include <errno.h>
#include <system_error>
#include "logger.h"
#include "netmsg.h"
#include "netdispatcher.h"
#include "fpmsyncd/fpmlink.h"

using namespace swss;
using namespace std;

FpmLink::FpmLink(int port) :
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
    socklen_t client_len;

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

void FpmLink::readData()
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
        hdr = (fpm_msg_hdr_t *)(m_messageBuffer + start);
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
            nl_msg *msg = nlmsg_convert((nlmsghdr *)fpm_msg_data(hdr));
            if (msg == NULL)
                throw system_error(make_error_code(errc::bad_message), "Unable to convert nlmsg");

            nlmsg_set_proto(msg, NETLINK_ROUTE);
            NetDispatcher::getInstance().onNetlinkMessage(msg);
            nlmsg_free(msg);
        }
        start += msg_len;
    }

    memmove(m_messageBuffer, m_messageBuffer + start, m_pos - start);
    m_pos = m_pos - (uint32_t)start;
}
