#ifndef __CONSUMERTABLE__
#define __CONSUMERTABLE__

#include <string>
#include <vector>
#include <limits>
#include <hiredis/hiredis.h>
#include "dbconnector.h"
#include "table.h"
#include "selectable.h"

namespace swss {

class ConsumerTable : public Table, public Selectable
{
public:
    ConsumerTable(DBConnector *db, std::string tableName);
    virtual ~ConsumerTable();

    /* Get a singlesubsribe channel rpop */
    void pop(KeyOpFieldsValuesTuple &kco);

    virtual void addFd(fd_set *fd);
    virtual bool isMe(fd_set *fd);
    virtual int readCache();
    virtual void readMe();

private:
    /* Create a new redisContext, SELECT DB and SUBSRIBE */
    void subsribe();

    DBConnector *m_subscribe;
    unsigned int m_queueLength;
};

}

#endif
