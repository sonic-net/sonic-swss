#include "orchagent/lagid.h"

LagIdAllocator::LagIdAllocator(
        _In_ DBConnector* chassis_app_db)
{
    SWSS_LOG_ENTER();

    m_dbConnector = chassis_app_db;

    // Load lua script to allocate system lag id. This lua script ensures allocation
    // of unique system lag id from global chassis app db in atomic fashion when allocation
    // is requested by different asic instances simultaneously

    string luaScript = loadLuaScript("lagids.lua");
    m_shaLagId = loadRedisScript(m_dbConnector, luaScript);
}

int32_t LagIdAllocator::lagIdAdd(
        _In_ const string &pcname,
        _In_ int32_t lag_id)
{
    SWSS_LOG_ENTER();

    // No keys
    vector<string> keys;

    vector<string> args;
    args.push_back("add");
    args.push_back(pcname);
    args.push_back(to_string(lag_id));

    set<string> ret = runRedisScript(*m_dbConnector, m_shaLagId, keys, args);

    if (!ret.empty())
    {
        // We expect only one value in the set returned

        auto rv_lag_id = ret.begin();

        return (stoi(*rv_lag_id));
    }

    return LAG_ID_ALLOCATOR_ERROR_DB_ERROR;
}

int32_t LagIdAllocator::lagIdDel(
        _In_ const string &pcname)
{
    SWSS_LOG_ENTER();

    // No keys
    vector<string> keys;

    vector<string> args;
    args.push_back("del");
    args.push_back(pcname);

    set<string> ret = runRedisScript(*m_dbConnector, m_shaLagId, keys, args);

    if (!ret.empty())
    {
        // We expect only one value in the set returned

        auto rv_lag_id = ret.begin();

        return (stoi(*rv_lag_id));
    }

    return LAG_ID_ALLOCATOR_ERROR_DB_ERROR;
}

int32_t LagIdAllocator::lagIdGet(
        _In_ const string &pcname)
{
    SWSS_LOG_ENTER();

    // No keys
    vector<string> keys;

    vector<string> args;
    args.push_back("get");
    args.push_back(pcname);

    set<string> ret = runRedisScript(*m_dbConnector, m_shaLagId, keys, args);

    if (!ret.empty())
    {
        // We expect only one value in the set returned

        auto rv_lag_id = ret.begin();

        return (stoi(*rv_lag_id));
    }

    return LAG_ID_ALLOCATOR_ERROR_DB_ERROR;
}
