#ifndef __WARMRESTART_HELPER__
#define __WARMRESTART_HELPER__


#include <vector>
#include <map>
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

    bool runRestoration(void);

    void insertRefreshMap(const KeyOpFieldsValuesTuple &kfv);

    void reconcile(void);

    const std::string printKFV(const std::string                  &key,
                               const std::vector<FieldValueTuple> &fv);

  private:

    bool compareAllFV(const std::vector<FieldValueTuple> &left,
                      const std::vector<FieldValueTuple> &right);

    bool compareOneFV(const std::string &v1, const std::string &v2);

    ProducerStateTable       *m_syncTable;         // producer-table to sync/push state to
    Table                     m_restorationTable;  // redis table to import current-state from
    kfvVector                 m_restorationVector; // buffer struct to hold old state
    kfvMap                    m_refreshMap;        // buffer struct to hold new state
    WarmStart::WarmStartState m_state;             // cached value of warmStart's FSM state
    bool                      m_enabled;           // warm-reboot enabled/disabled status
    std::string               m_syncTableName;     // producer-table-name to sync/push state to
    std::string               m_dockName;          // sonic-docker requesting warmStart services
    std::string               m_appName;           // sonic-app requesting warmStart services
};


}

#endif
