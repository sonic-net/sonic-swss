#include <string>
#include <vector>

#include "producerstatetable.h"

namespace swss
{

ProducerStateTable::ProducerStateTable(DBConnector *db, const std::string &tableName)
    : TableBase(tableName, ":"), TableName_KeySet(tableName)
{
}

ProducerStateTable::~ProducerStateTable()
{
}

void ProducerStateTable::set(const std::string &key, const std::vector<FieldValueTuple> &values, const std::string &op,
                        const std::string &prefix)
{
}

void ProducerStateTable::del(const std::string &key, const std::string &op, const std::string &prefix)
{
}

} // namespace swss