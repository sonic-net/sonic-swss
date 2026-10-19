/*
 * ASAN support helpers:
 * 1. Install a SIGTERM handler that runs an injected LSan leak check.
 * 2. When /etc/sonic/inject_asan_test_leak_enabled exists, inject a known test
 *    leak used to verify the ASAN/LSan path is working as expected.
 *
 * ENABLE_ASAN=y daemon builds also link asan_ctor.cpp, whose constructor calls
 * swss_asan_init_impl() before main(). Unit tests leave asan_ctor.cpp out and
 * call swss_asan_init_impl() / swss_asan_sigterm_handler_impl() with test-
 * double functions for the dependencies.
 */

#include "asan.h"

#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

#include <logger.h>

/* ASAN test-leak injection
 *
 * When ASAN is enabled and /etc/sonic/inject_asan_test_leak_enabled exists,
 * allocate a block and deliberately never free it so LSAN has a known leak to
 * report on process exit or in the SIGTERM handler. This is useful for
 * verifying that the ASAN build, configuration, and SIGTERM handlers are
 * working as expected.
 *
 * The memory block has to still look unreachable when the leak check runs,
 * which is difficult. LSan scans thread stacks conservatively, and at -O2 ASAN
 * moves the injector's locals into a "fake stack" frame that lives on the heap
 * for the lifetime of the thread. Overwriting the real stack never reaches
 * those copies, so a leak injected on the main thread stays reachable and is
 * silently dropped by __lsan_do_leak_check() in the SIGTERM handler.
 *
 * Injecting from a short-lived helper thread sidesteps that: once the thread is
 * joined, both its stack and its ASAN fake stack are gone, so no stale pointer
 * survives for LSan to trip over. This works at every optimization level and
 * needs no ASAN_OPTIONS tuning.
 *
 * The intentional leak is injected at startup so it is present when the leak
 * check runs in the SIGTERM handler. Do not call the LSan leak-check callback
 * here: that terminates the process when leaks are present; call it only from
 * the SIGTERM handler.
 */

// Set by swss_asan_init_impl(); invoked from the SIGTERM handler.
static SwssLsanLeakCheckFn g_lsan_leak_check = nullptr;

__attribute__((noinline))
void swss_asan_inject_test_leak(SwssMallocFn malloc_fn)
{
    void *probe = malloc_fn(SWSS_ASAN_TEST_LEAK_SIZE);
    if (!probe)
    {
        SWSS_LOG_ERROR("failed to allocate %zu bytes for the ASAN test leak, no leak injected",
                       SWSS_ASAN_TEST_LEAK_SIZE);
        return;
    }

    std::memset(probe, 0xCD, SWSS_ASAN_TEST_LEAK_SIZE);

    // Feed the pointer to an opaque asm that also reads memory, so -O2 cannot
    // drop the malloc and memset as dead stores. Nothing stores the pointer, so
    // the block remains unreachable.
    asm volatile("" : : "r"(probe) : "memory");
}

void swss_asan_sigterm_handler_impl(int signo,
                                    SwssLsanLeakCheckFn leak_check_fn,
                                    SwssSigactionFn sigaction_fn,
                                    SwssExitFn exit_fn,
                                    SwssRaiseFn raise_fn)
{
    SWSS_LOG_ENTER();

    if (leak_check_fn)
    {
        leak_check_fn();
    }

    struct sigaction sigact;
    if (sigaction_fn(SIGTERM, NULL, &sigact))
    {
        SWSS_LOG_ERROR("failed to get current SIGTERM action handler");
        exit_fn(EXIT_FAILURE);
        return;
    }

    // Check the currently set signal handler.
    // If it is ASAN's signal handler this means that the application didn't set its own handler.
    // To preserve default behavior set the default signal handler and raise the signal to trigger its execution.
    // Otherwise, the application installed its own signal handler.
    // In this case, just trigger a leak check and do nothing else.
    if (sigact.sa_handler == swss_asan_sigterm_handler) {
        sigemptyset(&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigact.sa_handler = SIG_DFL;
        if (sigaction_fn(SIGTERM, &sigact, NULL))
        {
            SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
            exit_fn(EXIT_FAILURE);
            return;
        }

        raise_fn(signo);
    }
}

void swss_asan_sigterm_handler(int signo)
{
    swss_asan_sigterm_handler_impl(signo, g_lsan_leak_check, ::sigaction, ::_exit, ::raise);
}

bool swss_asan_init_impl(SwssSigactionFn sigaction_fn,
                         SwssAccessFn access_fn,
                         SwssMallocFn malloc_fn,
                         SwssLsanLeakCheckFn leak_check_fn)
{
    SWSS_LOG_ENTER();

    g_lsan_leak_check = leak_check_fn;

    struct sigaction sigact = {};
    sigact.sa_handler = swss_asan_sigterm_handler;
    if (sigaction_fn(SIGTERM, &sigact, nullptr))
    {
        SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
        return false;
    }

    if (access_fn("/etc/sonic/inject_asan_test_leak_enabled", F_OK) == 0)
    {
        try
        {
            // See comment above swss_asan_inject_test_leak() for why this must
            // run in a separate thread.
            std::thread(swss_asan_inject_test_leak, malloc_fn).join();
        }
        catch (const std::exception& e)
        {
            SWSS_LOG_ERROR("failed to inject ASAN test leak: %s", e.what());
        }
    }

    return true;
}
