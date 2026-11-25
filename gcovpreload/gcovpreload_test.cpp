#include <csignal>
#include <cstdio>

// The preload library normally resolves this function from the executable's
// coverage runtime. Keep the test independent of coverage instrumentation.
extern "C" void __gcov_dump()
{
}

int main()
{
    const int signals[] = {
        SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGSEGV,
        SIGHUP, SIGINT, SIGQUIT, SIGTERM,
    };

    // Do not reference ctor() or sighandler(): retaining the library must not
    // depend on an explicit symbol reference from this executable.
    for (const int signal : signals)
    {
        struct sigaction action = {};
        if (sigaction(signal, nullptr, &action) != 0)
        {
            std::perror("sigaction");
            return 1;
        }
        if (action.sa_handler == SIG_DFL || action.sa_handler == SIG_IGN)
        {
            std::fprintf(stderr, "Missing preload handler for signal %d\n", signal);
            return 1;
        }
        if ((static_cast<unsigned int>(action.sa_flags) &
             static_cast<unsigned int>(SA_RESETHAND)) == 0)
        {
            std::fprintf(stderr, "Missing SA_RESETHAND for signal %d\n", signal);
            return 1;
        }
    }
    return 0;
}
