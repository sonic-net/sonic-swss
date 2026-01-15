#include "consumertablebase.h"
#include "subscriberstatetable.h"

using namespace std;

namespace swss
{
SubscriberStateTable::SubscriberStateTable(DBConnector *db, const string &tableName, int popBatchSize, int pri)
    : ConsumerTableBase(db, tableName, popBatchSize, pri),
      m_table(db, tableName) {}

}
