#ifndef __PRODUCERTABLE__
#define __PRODUCERTABLE__

#include <string>
#include <vector>

#include <hiredis/hiredis.h>
#include "dbconnector.h"
#include "table.h"
#include "scheme.h"

namespace swss {

class ProducerTable : public Table
{
public:
    ProducerTable(DBConnector *db, std::string tableName);

    /* Implements set() and del() commands using notification messages */
    virtual void set(std::string key, std::vector<FieldValueTuple> &values,
                     std::string op = SET_COMMAND);
    virtual void del(std::string key, std::string op = DEL_COMMAND);

private:
    /* Disable copy-constructor and operator = */
    ProducerTable(const ProducerTable &other);
    ProducerTable & operator = (const ProducerTable &other);

    void enqueueDbChange(std::string key, std::string value, std::string op);
};

}

#endif
