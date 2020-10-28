#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "dbconnector.h"

namespace swss
{
    DBConnector::DBConnector(int dbId, const std::string &hostname, int port, unsigned int timeout) :
        m_dbId(dbId)
    {
        auto conn = (redisContext *)calloc(1, sizeof(redisContext));
        conn->connection_type = REDIS_CONN_TCP;
        conn->tcp.host = strdup(hostname.c_str());
        conn->tcp.port = port;
        conn->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        setContext(conn);
    }

    DBConnector::DBConnector(int dbId, const std::string &unixPath, unsigned int timeout) :
        m_dbId(dbId)
    {
        auto conn = (redisContext *)calloc(1, sizeof(redisContext));
        conn->connection_type = REDIS_CONN_UNIX;
        conn->unix_sock.path = strdup(unixPath.c_str());
        conn->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        setContext(conn);
    }

    DBConnector::DBConnector(const std::string& dbName, unsigned int timeout, bool isTcpConn)
        : m_dbName(dbName)
    {
        if (swss::SonicDBConfig::isInit() == false)
            swss::SonicDBConfig::initialize("./database_config.json");
        m_dbId = swss::SonicDBConfig::getDbId(dbName);
        if (isTcpConn)
        {
            auto conn = (redisContext *)calloc(1, sizeof(redisContext));
            conn->connection_type = REDIS_CONN_TCP;
            conn->tcp.host = strdup(swss::SonicDBConfig::getDbHostname(dbName).c_str());
            conn->tcp.port = swss::SonicDBConfig::getDbPort(dbName);
            conn->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
            setContext(conn);
        }
        else
        {
            auto conn = (redisContext *)calloc(1, sizeof(redisContext));
            conn->connection_type = REDIS_CONN_UNIX;
            conn->unix_sock.path = strdup(swss::SonicDBConfig::getDbSock(dbName).c_str());
            conn->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
            setContext(conn);
        }
    }

    int DBConnector::getDbId() const
    {
        return m_dbId;
    }
}
