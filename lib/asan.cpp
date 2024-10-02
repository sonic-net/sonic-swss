#include <unistd.h>
#include <signal.h>
#include <sanitizer/lsan_interface.h>

#include <logger.h>

extern "C" {
    const char* __lsan_default_suppressions() {
        return "leak:__static_initialization_and_destruction_0\n";
    }
}

static void swss_asan_sigterm_handler(int signo)
{
    SWSS_LOG_ENTER();

    __lsan_do_leak_check();

    struct sigaction sigact;
    if (sigaction(SIGTERM, NULL, &sigact))
    {
        SWSS_LOG_ERROR("failed to get current SIGTERM action handler");
        _exit(EXIT_FAILURE);
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
        if (sigaction(SIGTERM, &sigact, NULL))
        {
            SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
            _exit(EXIT_FAILURE);
        }

        raise(signo);
    }
}

__attribute__((constructor))
static void swss_asan_init()
{
    SWSS_LOG_ENTER();

    struct sigaction sigact = {};
    sigact.sa_handler = swss_asan_sigterm_handler;
    if (sigaction(SIGTERM, &sigact, NULL))
    {
        SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
        exit(EXIT_FAILURE);
    }
}
