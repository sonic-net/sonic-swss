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

#include "selectable.h"
#include "fpm/fpm.h"

namespace swss {

class FpmLink : public Selectable {
public:
    const int MSG_BATCH_SIZE;
    FpmLink(int port = FPM_DEFAULT_PORT);
    virtual ~FpmLink();

    /* Wait for connection (blocking) */
    void accept();

    int getFd() override;
    void readData() override;
    /* readMe throws FpmConnectionClosedException when connection is lost */
    class FpmConnectionClosedException : public std::exception
    {
    };

private:
    unsigned int m_bufSize;
    char *m_messageBuffer;
    unsigned int m_pos;

    bool m_connected;
    bool m_server_up;
    int m_server_socket;
    int m_connection_socket;
};

}

#endif
