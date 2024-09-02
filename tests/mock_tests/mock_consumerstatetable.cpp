#include "consumerstatetable.h"

namespace swss
{
    ConsumerStateTable::ConsumerStateTable(DBConnector *db, const std::string &tableName, int popBatchSize, int pri) :
        ConsumerTableBase(db, tableName, popBatchSize, pri),
        TableName_KeySet(tableName)
    {
    }

    void ConsumerStateTable::pops(std::deque<KeyOpFieldsValuesTuple> &vkco, const std::string& /*prefix*/)
    {
        int count = 0;
        swss::Table table(getDbConnector(), getTableName());
        std::vector<std::string> keys;
        table.getKeys(keys);
        for (const auto &key: keys)
        {
            // pop with batch size
            if (count < POP_BATCH_SIZE)
            {
                count++;
            }
            else
            {
                break;
            }

            KeyOpFieldsValuesTuple kco;
            kfvKey(kco) = key;
            kfvOp(kco) = SET_COMMAND;
            if (!table.get(key, kfvFieldsValues(kco)))
            {
                continue;
            }
            table.del(key);
            vkco.push_back(kco);
        }
    }
}
