#include <map>

#include "table.h"

namespace swss
{

using TableDataT = std::map<std::string, std::map<std::string, std::string>>;
using TablesT = std::map<std::string, TableDataT>;

namespace fake_db
{

TablesT gTables;

} // namespace fake_db

using namespace fake_db;

Table::Table(const DBConnector *db, const std::string &tableName) : TableBase(tableName, ":")
{
}

Table::~Table()
{
}

void Table::hset(const std::string &key, const std::string &field, const std::string &value, const std::string & /*op*/,
                 const std::string & /*prefix*/)
{
    gTables[getTableName()][key][field] = value;
}

void Table::set(const std::string &key, const std::vector<FieldValueTuple> &values, const std::string & /*op*/,
                const std::string & /*prefix*/)
{
    auto &fvs = gTables[getTableName()][key];
    for (const auto &fv : values)
    {
        fvs[fv.first] = fv.second;
    }
}

bool Table::hget(const std::string &key, const std::string &field, std::string &value)
{
    const auto &table_data = gTables[getTableName()];
    const auto &key_it = table_data.find(key);
    if (key_it == table_data.end())
    {
        return false;
    }
    const auto &field_it = key_it->second.find(field);
    if (field_it == key_it->second.end())
    {
        return false;
    }
    value = field_it->second;
    return true;
}

bool Table::get(const std::string &key, std::vector<FieldValueTuple> &ovalues)
{
    const auto &table_data = gTables[getTableName()];
    if (table_data.find(key) == table_data.end())
    {
        return false;
    }

    for (const auto &fv : table_data.at(key))
    {
        ovalues.push_back({fv.first, fv.second});
    }
    return true;
}

void Table::del(const std::string &key, const std::string & /*op*/, const std::string & /*prefix*/)
{
    gTables[getTableName()].erase(key);
}

void Table::hdel(const std::string &key, const std::string &field, const std::string & /*op*/,
                 const std::string & /*prefix*/)
{
    gTables[getTableName()][key].erase(field);
}

void Table::getKeys(std::vector<std::string> &keys)
{
    keys.clear();
    auto table = gTables[getTableName()];
    for (const auto &it : table)
    {
        keys.push_back(it.first);
    }
}

} // namespace swss
