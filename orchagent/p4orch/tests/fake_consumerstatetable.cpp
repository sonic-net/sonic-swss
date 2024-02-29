#include "consumerstatetable.h"

namespace swss
{

ConsumerStateTable::ConsumerStateTable(DBConnector *db, const std::string &tableName, int popBatchSize, int pri)
    : ConsumerTableBase(db, tableName, popBatchSize, pri), TableName_KeySet(tableName)
{
}

} // namespace swss
