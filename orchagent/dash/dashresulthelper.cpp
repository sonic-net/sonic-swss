#include "dashresulthelper.h"

#include <exception>
#include <vector>

#include "logger.h"

using namespace std;
using namespace swss;

void writeResultToDB(const std::unique_ptr<swss::Table>& table, const string& key,
                     uint32_t res, const string& version)
{
    SWSS_LOG_ENTER();

    if (!table)
    {
        SWSS_LOG_WARN("Table passed in is NULL");
        return;
    }

    std::vector<FieldValueTuple> fvVector;

    fvVector.emplace_back("result", std::to_string(res));

    if (!version.empty())
    {
        fvVector.emplace_back("version", version);
    }

    try
    {
        table->set(key, fvVector);
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Exception caught while writing to DB: %s", e.what());
        return;
    }
    SWSS_LOG_INFO("Wrote result to DB for key %s", key.c_str());
}

void removeResultFromDB(const std::unique_ptr<swss::Table>& table, const string& key)
{
    SWSS_LOG_ENTER();

    if (!table)
    {
        SWSS_LOG_WARN("Table passed in is NULL");
        return;
    }

    try
    {
        table->del(key);
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Exception caught while removing from DB: %s", e.what());
        return;
    }
    SWSS_LOG_INFO("Removed result from DB for key %s", key.c_str());
}

void flushResultsToDB(const std::unique_ptr<swss::Table>& table)
{
    SWSS_LOG_ENTER();

    if (!table)
    {
        SWSS_LOG_WARN("Table passed in is NULL");
        return;
    }

    try
    {
        table->flush();
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Exception caught while flushing results to DB: %s", e.what());
    }
}
