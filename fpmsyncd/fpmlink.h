#ifndef __FPMLINK__
#define __FPMLINK__

#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <exception>

#include "fpm/fpm.h"
#include "fpmsyncd/fpminterface.h"
#include "fpmsyncd/routesync.h"

#define RTM_NEWSRV6LOCALSID		1000
#define RTM_DELSRV6LOCALSID		1001
#define RTM_NEWPICCONTEXT		2000
#define RTM_DELPICCONTEXT		2001
#define RTM_NEWSRV6VPNROUTE		3000
#define RTM_DELSRV6VPNROUTE		3001

namespace swss {

class FpmLink : public FpmInterface {
public:
    const int MSG_BATCH_SIZE;
    FpmLink(RouteSync *rsync, unsigned short port = FPM_DEFAULT_PORT);
    virtual ~FpmLink();

    /* Wait for connection (blocking) */
    void accept();

    int getFd() override;
    uint64_t readData() override;
    /* readMe throws FpmConnectionClosedException when connection is lost */
    class FpmConnectionClosedException : public std::exception
    {
    };

    bool isRawProcessing(struct nlmsghdr *h);
    void processRawMsg(struct nlmsghdr *h)
    {
        m_routesync->onMsgRaw(h);
    };

    void processFpmMessage(fpm_msg_hdr_t* hdr);

    bool send(nlmsghdr* nl_hdr) override;

private:
    RouteSync *m_routesync;
    unsigned int m_bufSize;
    char *m_messageBuffer;
    char *m_sendBuffer;
    unsigned int m_pos;

    bool m_connected;
    bool m_server_up;
    int m_server_socket;
    int m_connection_socket;
};

}

#endif
