#ifndef PFC_ACTION_HANDLER_H
#define PFC_ACTION_HANDLER_H

#include <vector>
#include <memory>
#include <set>
#include <map>
#include "aclorch.h"
#include "table.h"

extern "C" {
#include "sai.h"
}

using namespace std;
using namespace swss;

struct PfcWdHwStats
{
    uint64_t txPkt;
    uint64_t txDropPkt;
    uint64_t rxPkt;
    uint64_t rxDropPkt;
};

// PFC queue interface class
// It resembles RAII behavior - pause storm is mitigated (queue is locked) on creation,
// and is restored (queue released) on removal
class PfcWdActionHandler
{
    public:
        PfcWdActionHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdActionHandler(void);

        inline sai_object_id_t getPort(void) const
        {
            return m_port;
        }

        inline sai_object_id_t getQueue(void) const
        {
            return m_queue;
        }

        inline uint8_t getQueueId(void) const
        {
            return m_queueId;
        }

        virtual bool isValid(void) const { return true; }

        // Called by PfcWdSwOrch when a handler that deferred its programming
        // turns out to have failed once the batch was flushed. Only handlers
        // that defer work override this.
        virtual void markInvalid(void) { }

        // Hooks that let PfcWdSwOrch keep switch programming off the storm
        // detection path. The defaults are no-ops, so handlers that touch
        // nothing shared (DLR, zero-buffer, lossy) are unaffected.
        //
        //   prepare()   - called when the watchdog is configured on a port,
        //                 long before any storm, to pre-create shared state.
        //   unprepare() - called when the watchdog is removed from a port.
        //   flush()     - called once after a whole batch of storm/restore
        //                 events has been handled. Returns the OIDs of the
        //                 queues whose deferred programming failed.
        static bool prepare(sai_object_id_t, const std::set<uint8_t> &) { return true; }
        static void unprepare(sai_object_id_t) { }
        static std::set<sai_object_id_t> flush(void) { return std::set<sai_object_id_t>(); }

        static void initWdCounters(shared_ptr<Table> countersTable, const string &queueIdStr);
        void initCounters(void);
        void commitCounters(bool periodic = false);
        bool getHwCounters(PfcWdHwStats& counters);

    private:
        struct PfcWdQueueStats
        {
            uint64_t detectCount;
            uint64_t restoreCount;
            uint64_t txPkt;
            uint64_t txDropPkt;
            uint64_t rxPkt;
            uint64_t rxDropPkt;
            uint64_t txPktLast;
            uint64_t txDropPktLast;
            uint64_t rxPktLast;
            uint64_t rxDropPktLast;
            bool     operational;
        };

        static PfcWdQueueStats getQueueStats(shared_ptr<Table> countersTable, const string &queueIdStr);
        void updateWdCounters(const string& queueIdStr, const PfcWdQueueStats& stats);

        sai_object_id_t m_port = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_queue = SAI_NULL_OBJECT_ID;
        uint8_t m_queueId = 0;
        string m_portAlias;
        shared_ptr<Table> m_countersTable = nullptr;
        PfcWdHwStats m_hwStats;
};

// Pfc queue that implements forward action by disabling PFC on queue
class PfcWdLossyHandler: public PfcWdActionHandler
{
    public:
        PfcWdLossyHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdLossyHandler(void);
};

class PfcWdAclHandler: public PfcWdLossyHandler
{
    public:
        PfcWdAclHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdAclHandler(void);

        // class shared cleanup
        static void clear();

        bool isValid(void) const override { return !m_rolledBack; }
        void markInvalid(void) override { m_rolledBack = true; }

        // Pre-create the ACL tables the storm path would otherwise build
        // inline. Binds no ports.
        static bool prepare(sai_object_id_t port, const std::set<uint8_t> &queueIds);

        // Apply every ingress IN_PORTS change accumulated since the last
        // flush: one set_acl_entry_attribute per queue index, instead of one
        // per (port, queue). Returns the OIDs of the queues whose programming
        // failed, so the caller can mark those handlers invalid.
        static std::set<sai_object_id_t> flush(void);

        // A deferred ingress change, recorded by the constructor (add) or the
        // destructor (remove) and applied by flush().
        struct PendingPortOp
        {
            bool add = true;
            sai_object_id_t queueOid = SAI_NULL_OBJECT_ID;
        };

    private:
        // class shared dict: ACL table name -> ACL table
        static std::map<std::string, AclTable> m_aclTables;

        // Ingress IN_PORTS changes waiting for the next flush(), indexed by
        // queue index and then by port. Indexing by port means a storm and a
        // restore on the same (port, TC) inside one batch collapse to a single
        // net change, so the ASIC never sees the intermediate state.
        static std::map<uint8_t, std::map<sai_object_id_t, PendingPortOp>> m_pendingInPorts;

        bool shared_egress_acl_table = false;

        bool m_rolledBack = false;

        string m_strIngressTable;
        string m_strEgressTable;
        string m_strRule;
        string m_strEgressRule;

        // Put a queue's unapplied in-ports changes back for the next flush.
        static void requeue(uint8_t queueId,
                            const std::map<sai_object_id_t, PendingPortOp> &ops);
        static bool useSharedEgressAclTable(void);
        static string ingressRuleName(uint8_t queueId);
        static string egressTableName(uint8_t queueId);
        static bool ensureAclTable(sai_object_id_t port, const string &strTable, bool ingress);
        static bool createPfcAclTable(sai_object_id_t port, string strTable, bool ingress);
        static bool createPfcAclRule(shared_ptr<AclRulePacket> rule, uint8_t queueId,
                const string &strTable, const vector<sai_object_id_t> &ports);
};

class PfcWdDlrHandler: public PfcWdActionHandler
{
    public:
        PfcWdDlrHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdDlrHandler(void);
};

// PFC queue that implements drop action by draining queue with buffer of zero size
class PfcWdZeroBufferHandler: public PfcWdLossyHandler
{
    public:
        PfcWdZeroBufferHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdZeroBufferHandler(void);

    private:
        /*
         * Sets lock bits on port's queue
         * to protect it from being changed by other Orch's
        */
        void setQueueLockFlag(Port& port, bool isLocked) const;

        // Singletone class for keeping shared data - zero buffer profiles
        class ZeroBufferProfile
        {
            public:
                ~ZeroBufferProfile(void);
                static sai_object_id_t getZeroBufferProfile();

            private:
                ZeroBufferProfile(void);
                static ZeroBufferProfile &getInstance(void);
                void createZeroBufferProfile();
                void destroyZeroBufferProfile();

                sai_object_id_t& getProfile()
                {
                    return m_zeroEgressBufferProfile;
                }

                sai_object_id_t& getPool()
                {
                    return m_zeroEgressBufferPool;
                }

                sai_object_id_t m_zeroEgressBufferPool = SAI_NULL_OBJECT_ID;
                sai_object_id_t m_zeroEgressBufferProfile = SAI_NULL_OBJECT_ID;
        };

        sai_object_id_t m_originalQueueBufferProfile = SAI_NULL_OBJECT_ID;
};

#endif
