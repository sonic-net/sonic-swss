#ifndef SWSS_ROUTEBULKSUBMITTER_H
#define SWSS_ROUTEBULKSUBMITTER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include "bulker.h"

class RouteBulkSubmitter
{
public:
    RouteBulkSubmitter();
    ~RouteBulkSubmitter();

    void submit(EntityBulker<sai_route_api_t>& bulker);
    void waitForFlush();
    bool isIdle();
    void stop();

private:
    void threadFunc();

    std::thread m_thread;
    std::mutex m_mtx;
    std::condition_variable m_cv;

    EntityBulker<sai_route_api_t>* m_bulker = nullptr;
    bool m_flushing = false;
    bool m_exit = false;
};

#endif /* SWSS_ROUTEBULKSUBMITTER_H */
