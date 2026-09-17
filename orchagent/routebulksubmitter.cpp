#include "routebulksubmitter.h"
#include "logger.h"

RouteBulkSubmitter::RouteBulkSubmitter()
{
    m_thread = std::thread(&RouteBulkSubmitter::threadFunc, this);
}

RouteBulkSubmitter::~RouteBulkSubmitter()
{
    stop();
}

void RouteBulkSubmitter::submit(EntityBulker<sai_route_api_t>& bulker)
{
    std::unique_lock<std::mutex> lock(m_mtx);
    m_cv.wait(lock, [this] { return !m_flushing; });
    m_bulker = &bulker;
    m_flushing = true;
    m_cv.notify_one();
}

void RouteBulkSubmitter::waitForFlush()
{
    std::unique_lock<std::mutex> lock(m_mtx);
    m_cv.wait(lock, [this] { return !m_flushing; });
}

bool RouteBulkSubmitter::isIdle()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return !m_flushing;
}

void RouteBulkSubmitter::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_exit)
            return;
        m_exit = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable())
        m_thread.join();
}

void RouteBulkSubmitter::threadFunc()
{
    SWSS_LOG_NOTICE("RouteBulkSubmitter thread started");

    while (true)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait(lock, [this] { return m_bulker != nullptr || m_exit; });

        if (m_exit)
            break;

        auto* bulker = m_bulker;
        lock.unlock();

        bulker->flush();

        lock.lock();
        m_bulker = nullptr;
        m_flushing = false;
        m_cv.notify_all();
    }

    SWSS_LOG_NOTICE("RouteBulkSubmitter thread exited");
}
