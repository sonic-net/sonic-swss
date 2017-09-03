#ifndef PFC_ACTION_HANDLER_H
#define PFC_ACTION_HANDLER_H

#include <vector>

extern "C" {
#include "sai.h"
}

using namespace std;

// PFC queue interface class
// It resembles RAII behavior - pause storm is mitigated (queue is locked) on creation,
// and is restored (queue released) on removal
class PfcWdActionHandler
{
    public:
        PfcWdActionHandler(sai_object_id_t port, sai_object_id_t queue, uint8_t queueId);
        virtual ~PfcWdActionHandler(void) = 0;

        inline sai_object_id_t getPort(void)
        {
            return m_port;
        }

        inline sai_object_id_t getQueue(void)
        {
            return m_queue;
        }

        inline sai_object_id_t getQueueId(void)
        {
            return m_queueId;
        }

    private:
        vector<uint64_t> getQueueStats(void);

        sai_object_id_t m_port = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_queue = SAI_NULL_OBJECT_ID;
        uint8_t m_queueId = 0;
        vector<uint64_t> m_stats;
};

// Pfc queue that implements forward action by disabling PFC on queue
class PfcWdLossyHandler: public PfcWdActionHandler
{
    public:
        PfcWdLossyHandler(sai_object_id_t port, sai_object_id_t queue, uint8_t queueId);
        virtual ~PfcWdLossyHandler(void);
};

// PFC queue that implements drop action by draining queue with buffer of zero size
class PfcWdZeroBufferHandler: public PfcWdLossyHandler
{
    public:
        PfcWdZeroBufferHandler(sai_object_id_t port, sai_object_id_t queue, uint8_t queueId);
        virtual ~PfcWdZeroBufferHandler(void);

    private:
        // Singletone class for keeping shared data - zero buffer profiles
        class ZeroBufferProfile
        {
            public:
                ~ZeroBufferProfile(void);
                static sai_object_id_t getStaticProfile(void);
                static sai_object_id_t getDynamicProfile(void);

            private:
                ZeroBufferProfile(void);
                static ZeroBufferProfile &getInstance(void);
                void createStaticProfile(void);
                void createDynamicProfile(void);
                void destroyStaticProfile(void);
                void destroyDynamicProfile(void);

                sai_object_id_t m_zeroStaticBufferPool = SAI_NULL_OBJECT_ID;
                sai_object_id_t m_zeroDynamicBufferPool = SAI_NULL_OBJECT_ID;
                sai_object_id_t m_zeroStaticBufferProfile = SAI_NULL_OBJECT_ID;
                sai_object_id_t m_zeroDynamicBufferProfile = SAI_NULL_OBJECT_ID;
        };

        sai_object_id_t m_originalBufferProfile;
};

#endif
