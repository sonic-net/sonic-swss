#include <string>
#include <vector>

#include "producertable.h"

namespace swss
{

ProducerTable::ProducerTable(DBConnector *db, const std::string &tableName)
    : TableBase(tableName, ":"), TableName_KeyValueOpQueues(tableName)
{
}

ProducerTable::~ProducerTable()
{
}

void ProducerTable::set(const std::string &key, const std::vector<FieldValueTuple> &values, const std::string &op,
                        const std::string &prefix)
{
}

void ProducerTable::del(const std::string &key, const std::string &op, const std::string &prefix)
{
}

} // namespace swss
