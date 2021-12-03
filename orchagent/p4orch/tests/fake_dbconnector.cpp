#include <map>
#include <string>

#include "dbconnector.h"

namespace swss
{

static std::map<std::string, int> dbNameIdMap = {
    {"APPL_DB", 0}, {"ASIC_DB", 1}, {"COUNTERS_DB", 2}, {"CONFIG_DB", 4}, {"FLEX_COUNTER_DB", 5}, {"STATE_DB", 6},
};

RedisContext::RedisContext()
{
}

RedisContext::~RedisContext()
{
}

DBConnector::DBConnector(int dbId, const std::string &hostname, int port, unsigned int timeout) : m_dbId(dbId)
{
}

DBConnector::DBConnector(const std::string &dbName, unsigned int timeout, bool isTcpConn)
{
    if (dbNameIdMap.find(dbName) != dbNameIdMap.end())
    {
        m_dbId = dbNameIdMap[dbName];
    }
    else
    {
        m_dbId = -1;
    }
}

DBConnector::DBConnector(int dbId, const std::string &unixPath, unsigned int timeout) : m_dbId(dbId)
{
}

int DBConnector::getDbId() const
{
    return m_dbId;
}

} // namespace swss
