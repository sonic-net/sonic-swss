#include "counternameupdater.h"
#include "hftelorch.h"

#include <swss/logger.h>
#include <sai_serialize.h>

extern HFTelOrch *gSTelOrch;

CounterNameMapUpdater::CounterNameMapUpdater(const std::string &db_name, const std::string &table_name)
    : m_db_name(db_name),
      m_table_name(table_name),
      m_connector(m_db_name, 0),
      m_counters_table(&m_connector, m_table_name)
{
    SWSS_LOG_ENTER();
}

void CounterNameMapUpdater::setCounterNameMap(const std::string &counter_name, sai_object_id_t oid)
{
    SWSS_LOG_ENTER();

    // auto itr = m_local_cache.find(counter_name);
    // if (itr != m_local_cache.end())
    // {
    //     SWSS_LOG_WARN("Counter name %s already exists in the local cache", counter_name.c_str());
    //     if (itr->second == oid)
    //     {
    //         return;
    //     }
    // }

    assert(gSTelOrch != nullptr);
    Message msg{
        .m_table_name = m_table_name.c_str(),
        .m_operation = OPERATION::SET,
        .m_set{
            .m_counter_name = counter_name.c_str(),
            .m_oid = oid,
        },
    };
    gSTelOrch->locallyNotify(msg);

    // m_local_cache[counter_name] = oid;
    m_counters_table.hset("", counter_name, sai_serialize_object_id(oid));
}

void CounterNameMapUpdater::delCounterNameMap(const std::string &counter_name)
{
    SWSS_LOG_ENTER();

    // auto itr = m_local_cache.find(counter_name);
    // if (itr == m_local_cache.end())
    // {
    //     SWSS_LOG_WARN("Counter name %s does not exist in the local cache", counter_name.c_str());
    //     return;
    // }

    assert(gSTelOrch != nullptr);
    Message msg{
        .m_table_name = m_table_name.c_str(),
        .m_operation = OPERATION::DEL,
        .m_del{
            .m_counter_name = counter_name.c_str(),
        },
    };
    gSTelOrch->locallyNotify(msg);

    // m_local_cache.erase(itr);
    m_counters_table.hdel("", counter_name);
}
