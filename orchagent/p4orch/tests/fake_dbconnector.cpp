#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "dbconnector.h"

namespace swss
{

static std::map<std::string, int> dbNameIdMap = {
    {"APPL_DB", 0}, {"ASIC_DB", 1}, {"COUNTERS_DB", 2}, {"CONFIG_DB", 4}, {"FLEX_COUNTER_DB", 5}, {"STATE_DB", 6}, {"CHASSIS_APP_DB", 12},
};

using DbDataT = std::map<int, std::map<std::string, std::map<std::string, std::string>>>;

namespace fake_db_connector
{

DbDataT gDB;

} // namespace fake_db_connector

using namespace fake_db_connector;

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

void DBConnector::hset(const std::string &key, const std::string &field, const std::string &value)
{
    gDB[m_dbId][key][field] = value;
}

std::vector<std::string> DBConnector::keys(const std::string &key)
{
    std::vector<std::string> list;
    for (auto const &x : gDB[m_dbId])
    {
        list.push_back(x.first);
    }
    return list;
}

} // namespace swss
