#include "subscriberstatetable.h"

namespace swss
{
    SubscriberStateTable::SubscriberStateTable(DBConnector *db, const std::string &tableName, int popBatchSize, int pri) :
        ConsumerTableBase(db, tableName, popBatchSize, pri),
	m_table(db, tableName)
    {
        m_keyspace = "__keyspace@";

        m_keyspace += std::to_string(db->getDbId()) + "__:" + tableName + m_table.getTableNameSeparator() + "*";

        psubscribe(m_db, m_keyspace);
    }

    void SubscriberStateTable::pops(std::deque<KeyOpFieldsValuesTuple> &vkco, const std::string& /*prefix*/)
    {
        std::vector<std::string> keys;
        m_table.getKeys(keys);
        for (const auto &key: keys)
        {
            KeyOpFieldsValuesTuple kco;

            kfvKey(kco) = key;
            kfvOp(kco) = SET_COMMAND;

            if (!m_table.get(key, kfvFieldsValues(kco)))
            {
                continue;
            }
            m_table.del(key);
            vkco.push_back(kco);
        }
    }
}
