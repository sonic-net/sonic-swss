#include "table.h"
#include "producerstatetable.h"
#include "producertable.h"
#include <set>
#include <memory>

using TableDataT = std::map<std::string, std::vector<swss::FieldValueTuple>>;
using TablesT = std::map<std::string, TableDataT>;

namespace testing_db
{

    TableDataT gTableData;
    TablesT gTables;
    std::map<int, TablesT> gDB;

    void reset()
    {
        gDB.clear();
    }
}

namespace swss
{

    using namespace testing_db;

    void merge_values(std::vector<FieldValueTuple> &existing_values, const std::vector<FieldValueTuple> &values)
    {
        std::vector<FieldValueTuple> new_values(values);
        std::set<std::string> field_set;
        for (auto &value : values)
        {
            field_set.insert(fvField(value));
        }
        for (auto &value : existing_values)
        {
            auto &field = fvField(value);
            if (field_set.find(field) != field_set.end())
            {
                continue;
            }
            new_values.push_back(value);
        }
        existing_values.swap(new_values);
    }

    bool _hget(int dbId, const std::string &tableName, const std::string &key, const std::string &field, std::string &value)
    {
        auto table = gDB[dbId][tableName];
        if (table.find(key) == table.end())
        {
            return false;
        }

        for (const auto &it : table[key])
        {
            if (it.first == field)
            {
                value = it.second;
                return true;
            }
        }

        return false;
    }

    bool Table::get(const std::string &key, std::vector<FieldValueTuple> &ovalues)
    {
        auto table = gDB[m_pipe->getDbId()][getTableName()];
        if (table.find(key) == table.end())
        {
            return false;
        }

        ovalues = table[key];
        return true;
    }

    bool Table::hget(const std::string &key, const std::string &field, std::string &value)
    {
        return _hget(m_pipe->getDbId(), getTableName(), key, field, value);
    }

    void Table::set(const std::string &key,
                    const std::vector<FieldValueTuple> &values,
                    const std::string &op,
                    const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        auto iter = table.find(key);
        if (iter == table.end())
        {
            table[key] = values;
        }
        else
        {
            merge_values(iter->second, values);
        }
    }

    void Table::getKeys(std::vector<std::string> &keys)
    {
        keys.clear();
        auto table = gDB[m_pipe->getDbId()][getTableName()];
        for (const auto &it : table)
        {
            keys.push_back(it.first);
        }
    }

    void Table::del(const std::string &key, const std::string& /* op */, const std::string& /*prefix*/)
    {
        auto table = gDB[m_pipe->getDbId()].find(getTableName());
        if (table != gDB[m_pipe->getDbId()].end()){
            table->second.erase(key);
        }
    }
    
    void ProducerStateTable::set(const std::string &key,
                                 const std::vector<FieldValueTuple> &values,
                                 const std::string &op,
                                 const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        auto iter = table.find(key);
        if (iter == table.end())
        {
            table[key] = values;
        }
        else
        {
            merge_values(iter->second, values);
        }
    }

    void ProducerStateTable::del(const std::string &key,
                                 const std::string &op,
                                 const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        table.erase(key);
    }

    std::shared_ptr<std::string> DBConnector::hget(const std::string &key, const std::string &field)
    {
        std::string value;
        if (_hget(getDbId(), key, "", field, value))
        {
            std::shared_ptr<std::string> ptr(new std::string(value));
            return ptr;
        }
        else
        {
            return std::shared_ptr<std::string>(NULL);
        }
    }

    void ProducerTable::set(const std::string &key,
                            const std::vector<FieldValueTuple> &values,
                            const std::string &op,
                            const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        auto iter = table.find(key);
        if (iter == table.end())
        {
            table[key] = values;
        }
        else
        {
            merge_values(iter->second, values);
        }
    }

    void ProducerTable::del(const std::string &key,
                            const std::string &op,
                            const std::string &prefix)
    {
        auto &table = gDB[m_pipe->getDbId()][getTableName()];
        table.erase(key);
    }
}
