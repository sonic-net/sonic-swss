#include "subscriberstatetable.h"

namespace swss
{

SubscriberStateTable::SubscriberStateTable(DBConnector *db, const std::string &tableName, int popBatchSize, int pri)
    : ConsumerTableBase(db, tableName, popBatchSize, pri), m_table(db, tableName)
{
}

} // namespace swss
