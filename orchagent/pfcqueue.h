#ifndef PFC_QUEUE_H
#define PFC_QUEUE_H

#include <vector>

extern "C" {
#include "sai.h"
}

// PFC queue interface class
// It resembles RAII behavior - pause storm is mitigated (queue is locked) on creation,
// and is restored (queue released) on removal
class PfcQueue
{
    public:
        PfcQueue(sai_object_id_t port, sai_object_id_t queue, uint32_t queueId);
        virtual ~PfcQueue(void) = 0;

        inline sai_object_id_t port(void)
        {
            return m_port;
        }

        inline sai_object_id_t queue(void)
        {
            return m_queue;
        }

        inline sai_object_id_t queueId(void)
        {
            return m_queueId;
        }

    private:
        std::vector<uint64_t> getQueueStats(void);

        sai_object_id_t m_port = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_queue = SAI_NULL_OBJECT_ID;
        uint32_t m_queueId = 0;
        std::vector<uint64_t> m_stats;
};

// Pfc queue that implements forward action by disabling PFC on queue
class PfcLossyQueue: public PfcQueue
{
    public:
        PfcLossyQueue(sai_object_id_t port, sai_object_id_t queue, uint32_t queueId);
        virtual ~PfcLossyQueue(void);
};

// PFC queue that implements drop action by draining queue with buffer of zero size
class PfcZeroBufferQueue: public PfcLossyQueue
{
    public:
        PfcZeroBufferQueue(sai_object_id_t port, sai_object_id_t queue, uint32_t queueId);
        virtual ~PfcZeroBufferQueue(void);

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
