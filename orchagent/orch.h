#ifndef SWSS_ORCH_H
#define SWSS_ORCH_H

extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include "dbconnector.h"
#include "consumertable.h"
#include "producertable.h"

#include <map>

using namespace std;
using namespace swss;

class Orch
{
public:
    Orch(DBConnector *db, string tableName);
    ~Orch();

    inline ConsumerTable *getConsumer() { return m_consumer; }

    void execute();
    virtual void doTask() = 0;

    inline string getOrchName() { return m_name; }

private:
    DBConnector *m_db;
    const string m_name;

protected:
    ConsumerTable *m_consumer;

    /* Store the latest 'golden' status */
    map<string, KeyOpFieldsValuesTuple> m_toSync;

};

#endif /* SWSS_ORCH_H */
