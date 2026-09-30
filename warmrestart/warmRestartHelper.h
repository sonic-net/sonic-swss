#ifndef __WARMRESTART_HELPER__
#define __WARMRESTART_HELPER__


#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <algorithm>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "table.h"
#include "tokenize.h"
#include "warm_restart.h"


namespace swss {


class WarmStartHelper {
  public:

    WarmStartHelper(RedisPipeline      *pipeline,
                    ProducerStateTable *syncTable,
                    const std::string  &syncTableName,
                    const std::string  &dockerName,
                    const std::string  &appName);

    ~WarmStartHelper();

    /* fvVector type to be used to host AppDB restored elements */
    using kfvVector = std::vector<KeyOpFieldsValuesTuple>;

    /*
     * kfvMap type to be utilized to store all the new/refresh state coming
     * from the restarting applications.
     */
    using kfvMap = std::unordered_map<std::string, KeyOpFieldsValuesTuple>;

    void setState(WarmStart::WarmStartState state);

    WarmStart::WarmStartState getState(void) const;

    bool checkAndStart(void);

    bool isReconciled(void) const;

    bool inProgress(void) const;

    uint32_t getRestartTimer(void) const;

    void registerTable(RedisPipeline *pipeline,
               ProducerStateTable *syncTable,
               const std::string &syncTableName);

    bool runRestoration(void);

    void insertRefreshMap(const KeyOpFieldsValuesTuple &kfv);

    void insertRefreshMap(const std::string &syncTableName,
                const KeyOpFieldsValuesTuple &kfv);

    void reconcile(void);

    const std::string printKFV(const std::string                  &key,
                               const std::vector<FieldValueTuple> &fv);

  private:

    bool compareAllFV(const std::vector<FieldValueTuple> &left,
                      const std::vector<FieldValueTuple> &right);

    bool compareOneFV(const std::string &v1, const std::string &v2);

    struct TableContext
    {
      TableContext(RedisPipeline *pipeline,
             ProducerStateTable *producer,
             const std::string &tableName) :
        syncTable(producer),
        restorationTable(pipeline, tableName, false)
      {
      }

      ProducerStateTable *syncTable;
      Table restorationTable;
      kfvVector restorationVector;
      kfvMap refreshMap;
    };

    using TableContextMap = std::map<std::string, std::unique_ptr<TableContext>>;

    TableContextMap           m_tableContexts;
    WarmStart::WarmStartState m_state;             // cached value of warmStart's FSM state
    bool                      m_enabled;           // warm-reboot enabled/disabled status
    std::string               m_syncTableName;     // primary producer-table name
    std::string               m_dockName;          // sonic-docker requesting warmStart services
    std::string               m_appName;           // sonic-app requesting warmStart services
};


}

#endif
