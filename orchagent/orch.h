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

typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(ConsumerTable* consumer) :m_consumer(consumer)  { }
    ConsumerTable* m_consumer;
    /* Store the latest 'golden' status */
    SyncMap m_toSync;
};
typedef std::pair<string, Consumer> ConsumerMapPair;
typedef map<string, Consumer> ConsumerMap;

class Orch
{
public:
    Orch(DBConnector *db, string tableName);
    Orch(DBConnector *db, vector<string> &tableNames);
    ~Orch();

    void getSelectables( _out_ std::vector<Selectable*>& selectables);
    bool is_owned_consumer(ConsumerTable* s)const;

    bool execute(string tableName);

    inline string getOrchName() { return m_name; }

protected:
    virtual void doTask(_in_ Consumer& consumer_info) = 0;
private:
    DBConnector *m_db;
    const string m_name;// TODO: where is this field initialized??

protected:
    ConsumerMap m_consumer_map;

};

#endif /* SWSS_ORCH_H */
