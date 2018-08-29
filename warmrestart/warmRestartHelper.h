#ifndef __WARMRESTART_HELPER__
#define __WARMRESTART_HELPER__


#include <vector>
#include <map>
#include <unordered_map>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "table.h"
#include "tokenize.h"
#include "warm_restart.h"


namespace swss {


/* FieldValueTuple functor to serve as comparator for recoveryMap */
struct fvComparator
{
    bool operator()(const std::vector<FieldValueTuple> &left,
                    const std::vector<FieldValueTuple> &right) const
    {
        /*
         * The sizes of both tuple-vectors should always match within any given
         * application, otherwise we are running into some form of bug.
         */
        assert(left.size() == right.size());

        /*
         * Iterate through all the tuples present in left-vector and compare them
         * with those in the right one.
         */
        for (auto &fvLeft : left)
        {
            /*
             * Notice that we are forced to iterate through all the tuples in the
             * right-vector given that the order of fields within a tuple is not
             * fully deterministic (i.e. 'left' could hold 'nh: 1.1.1.1 / if: eth0'
             * and 'right' could be 'if: eth0, nh: 1.1.1.1').
             */
            for (auto &fvRight : right)
            {
                if (fvField(fvRight) == fvField(fvLeft))
                {
                    return fvLeft < fvRight;
                }
            }
        }

        return true;
    }
};


class WarmStartHelper {
  public:

    WarmStartHelper(RedisPipeline      *pipeline,
                    ProducerStateTable *syncTable,
                    const std::string  &dockerName,
                    const std::string  &appName);

    ~WarmStartHelper();

    /* State of collected fieldValue tuples */
    enum fvState_t
    {
        INVALID = 0,
        STALE   = 1,
        CLEAN   = 2,
        NEW     = 3,
        DELETE  = 4
    };

    /*
     * RecoveryMap types serve as the buffer data-struct where to hold the state
     * over which to run the reconciliation logic.
     */
    using fvRecoveryMap = std::map<std::vector<FieldValueTuple>, fvState_t, fvComparator>;
    using recoveryMap = std::unordered_map<std::string, fvRecoveryMap>;

    /* Useful type for recoveryMap manipulation */
    using fieldValuesTupleVoV = std::vector<std::vector<FieldValueTuple>>;


    void setState(WarmStart::WarmStartState state);

    WarmStart::WarmStartState getState(void) const;

    bool isEnabled(void) const;

    bool isReconciled(void) const;

    uint32_t getRestartTimer(void) const;

    bool runRecovery(void);

    bool buildRecoveryMap(void);

    void insertRecoveryMap(const KeyOpFieldsValuesTuple &kfv, fvState_t state);

    void removeRecoveryMap(const KeyOpFieldsValuesTuple &kfv, fvState_t state);

    void adjustRecoveryMap(fvRecoveryMap             &fvMap,
                           const fieldValuesTupleVoV &fvVector,
                           const std::string         &key);

    void reconciliate(void);

    void transformKFV(const std::vector<FieldValueTuple> &data,
                      std::vector<FieldValueTuple>       &fvVector);

  private:
    Table                     m_recoveryTable;  // redis table to import current-state from
    ProducerStateTable       *m_syncTable;      // producer-table to sync/push state to
    recoveryMap               m_recoveryMap;    // buffer struct to hold old&new state
    WarmStart::WarmStartState m_state;          // cached value of warmStart's FSM state
    std::string               m_dockName;       // sonic-docker requesting warmStart services
    std::string               m_appName;        // sonic-app requesting warmStart services
};


}

#endif
