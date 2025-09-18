#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <swss/rediscommand.h>
#include <swss/table.h>
#include <saitypes.h>

class CounterNameMapUpdater
{
public:

    enum OPERATION
    {
        SET,
        DEL,
    };

    struct SetPayload
    {
        const char* m_counter_name;
        sai_object_id_t m_oid;
    };

    struct DelPayload
    {
        const char* m_counter_name;
    };

    struct Message
    {
        const char* m_table_name;
        OPERATION m_operation;
        union
        {
            SetPayload m_set;
            DelPayload m_del;
        };
    };

    CounterNameMapUpdater(const std::string &db_name, const std::string &table_name);
    ~CounterNameMapUpdater() = default;

    void setCounterNameMap(const std::string &counter_name, sai_object_id_t oid);
    void setCounterNameMap(const std::vector<swss::FieldValueTuple> &counter_name_maps);
    void delCounterNameMap(const std::string &counter_name);

private:
    std::string m_db_name;
    std::string m_table_name;
    swss::DBConnector m_connector;
    swss::Table m_counters_table;

    std::string unify_counter_name(const std::string &counter_name);
};
