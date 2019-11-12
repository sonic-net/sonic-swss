#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "dbconnector.h"

namespace swss
{
    DBConnector::~DBConnector()
    {
        close(m_conn->fd);
        if (m_conn->connection_type == REDIS_CONN_TCP)
            free(m_conn->tcp.host);
        else
            free(m_conn->unix_sock.path);
        free(m_conn);
    }

    DBConnector::DBConnector(int dbId, const std::string &hostname, int port, unsigned int timeout) :
        m_dbId(dbId)
    {
        m_conn = (redisContext *)calloc(1, sizeof(redisContext));
        m_conn->connection_type = REDIS_CONN_TCP;
        m_conn->tcp.host = strdup(hostname.c_str());
        m_conn->tcp.port = port;
        m_conn->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    }

    DBConnector::DBConnector(int dbId, const std::string &unixPath, unsigned int timeout) :
        m_dbId(dbId)
    {
        m_conn = (redisContext *)calloc(1, sizeof(redisContext));
        m_conn->connection_type = REDIS_CONN_UNIX;
        m_conn->unix_sock.path = strdup(unixPath.c_str());
        m_conn->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    }

    int DBConnector::getDbId() const
    {
        return m_dbId;
    }
}
