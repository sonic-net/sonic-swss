#include "asan.h"

#include <unistd.h>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{

struct AsanSigactionState
{
    int calls = 0;
    // Count of calls to sigaction with act != nullptr.
    int set_calls = 0;
    // Count of calls to sigaction with oldact != nullptr.
    int query_calls = 0;

    int last_sig = 0;
    // Last non-null act passed to sigaction (init install or handler restore).
    struct sigaction last_set = {};
    // Value returned via oldact on a query (act == nullptr).
    struct sigaction oldact = {};
    bool oldact_set = false;
    // Per-call return codes. Missing entries (including when empty) succeed with 0.
    std::vector<int> rcs;
};

struct AsanTestState
{
    AsanSigactionState sigaction;

    int access_calls = 0;
    int access_rc = -1;
    std::string access_path;

    int malloc_calls = 0;
    size_t malloc_size = 0;
    // When true, mock_malloc returns nullptr. Otherwise it returns storage.data().
    bool malloc_fail = false;
    std::vector<unsigned char> storage;

    int leak_check_calls = 0;

    int exit_calls = 0;
    int exit_status = -1;

    int raise_calls = 0;
    int raise_signo = -1;
};

AsanTestState *g_state = nullptr;

int mock_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    EXPECT_NE(g_state, nullptr);
    auto& sa = g_state->sigaction;
    sa.calls++;
    sa.last_sig = sig;

    const size_t call_index = static_cast<size_t>(sa.calls - 1);
    const int rc = (call_index < sa.rcs.size()) ? sa.rcs[call_index] : 0;

    if (act)
    {
        sa.last_set = *act;
        sa.set_calls++;
    }
    if (oldact)
    {
        sa.query_calls++;
        if (sa.oldact_set)
        {
            *oldact = sa.oldact;
        }
        else
        {
            std::memset(oldact, 0, sizeof(*oldact));
        }
    }
    return rc;
}

int mock_access(const char *path, int mode)
{
    EXPECT_NE(g_state, nullptr);
    EXPECT_EQ(mode, F_OK);
    g_state->access_calls++;
    g_state->access_path = path ? path : "";
    return g_state->access_rc;
}

void *mock_malloc(size_t size)
{
    EXPECT_NE(g_state, nullptr);
    g_state->malloc_calls++;
    g_state->malloc_size = size;
    if (g_state->malloc_fail)
    {
        return nullptr;
    }
    g_state->storage.assign(size, 0);
    return g_state->storage.data();
}

void mock_leak_check(void)
{
    EXPECT_NE(g_state, nullptr);
    g_state->leak_check_calls++;
}

void mock_exit(int status)
{
    EXPECT_NE(g_state, nullptr);
    g_state->exit_calls++;
    g_state->exit_status = status;
}

int mock_raise(int signo)
{
    EXPECT_NE(g_state, nullptr);
    g_state->raise_calls++;
    g_state->raise_signo = signo;
    return 0;
}

void invoke_handler_impl()
{
    swss_asan_sigterm_handler_impl(SIGTERM, mock_leak_check, mock_sigaction, mock_exit, mock_raise);
}

} // namespace

class AsanInitTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        state_ = {};
        g_state = &state_;
    }

    void TearDown() override
    {
        g_state = nullptr;
    }

    AsanTestState state_;
};

TEST_F(AsanInitTest, InstallsSigtermHandler)
{
    state_.access_rc = -1;

    ASSERT_TRUE(swss_asan_init_impl(mock_sigaction, mock_access, mock_malloc, mock_leak_check));

    EXPECT_EQ(state_.sigaction.calls, 1);
    EXPECT_EQ(state_.sigaction.last_sig, SIGTERM);
    EXPECT_EQ(state_.sigaction.last_set.sa_handler, swss_asan_sigterm_handler);
    EXPECT_EQ(state_.access_calls, 1);
    EXPECT_EQ(state_.access_path, "/etc/sonic/inject_asan_test_leak_enabled");
    EXPECT_EQ(state_.malloc_calls, 0);
}

TEST_F(AsanInitTest, SigactionFailureReturnsFalse)
{
    state_.sigaction.rcs = {-1};

    EXPECT_FALSE(swss_asan_init_impl(mock_sigaction, mock_access, mock_malloc, mock_leak_check));

    EXPECT_EQ(state_.sigaction.calls, 1);
    EXPECT_EQ(state_.access_calls, 0);
    EXPECT_EQ(state_.malloc_calls, 0);
}

TEST_F(AsanInitTest, SkipsLeakInjectionWhenFlagFileMissing)
{
    state_.access_rc = -1;

    ASSERT_TRUE(swss_asan_init_impl(mock_sigaction, mock_access, mock_malloc, mock_leak_check));

    EXPECT_EQ(state_.malloc_calls, 0);
}

TEST_F(AsanInitTest, InjectsLeakWhenFlagFilePresent)
{
    state_.access_rc = 0;

    ASSERT_TRUE(swss_asan_init_impl(mock_sigaction, mock_access, mock_malloc, mock_leak_check));

    EXPECT_EQ(state_.malloc_calls, 1);
    EXPECT_EQ(state_.malloc_size, SWSS_ASAN_TEST_LEAK_SIZE);
    ASSERT_EQ(state_.storage.size(), SWSS_ASAN_TEST_LEAK_SIZE);
    EXPECT_EQ(state_.storage.front(), static_cast<unsigned char>(0xCD));
    EXPECT_EQ(state_.storage.back(), static_cast<unsigned char>(0xCD));
    EXPECT_EQ(state_.storage[state_.storage.size() / 2], static_cast<unsigned char>(0xCD));
}

TEST_F(AsanInitTest, MallocFailureStillReturnsTrue)
{
    state_.access_rc = 0;
    state_.malloc_fail = true;

    // Injection failure is logged; init itself still succeeds so the daemon
    // keeps running with the SIGTERM handler installed.
    ASSERT_TRUE(swss_asan_init_impl(mock_sigaction, mock_access, mock_malloc, mock_leak_check));

    EXPECT_EQ(state_.malloc_calls, 1);
    EXPECT_EQ(state_.malloc_size, SWSS_ASAN_TEST_LEAK_SIZE);
    EXPECT_TRUE(state_.storage.empty());
}

TEST(AsanInjectTest, FillsAllocationViaInjectedMalloc)
{
    AsanTestState state;
    g_state = &state;

    swss_asan_inject_test_leak(mock_malloc);

    EXPECT_EQ(state.malloc_calls, 1);
    EXPECT_EQ(state.malloc_size, SWSS_ASAN_TEST_LEAK_SIZE);
    ASSERT_EQ(state.storage.size(), SWSS_ASAN_TEST_LEAK_SIZE);
    EXPECT_EQ(state.storage.front(), static_cast<unsigned char>(0xCD));
    EXPECT_EQ(state.storage.back(), static_cast<unsigned char>(0xCD));

    g_state = nullptr;
}

TEST(AsanInjectTest, NullMallocIsANoOp)
{
    AsanTestState state;
    state.malloc_fail = true;
    g_state = &state;

    swss_asan_inject_test_leak(mock_malloc);

    EXPECT_EQ(state.malloc_calls, 1);
    EXPECT_TRUE(state.storage.empty());

    g_state = nullptr;
}

class AsanSigtermHandlerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        state_ = {};
        g_state = &state_;
    }

    void TearDown() override
    {
        g_state = nullptr;
    }

    AsanTestState state_;
};

TEST_F(AsanSigtermHandlerTest, RunsLeakCheckWhenProvided)
{
    state_.sigaction.oldact.sa_handler = SIG_DFL;
    state_.sigaction.oldact_set = true;

    invoke_handler_impl();

    EXPECT_EQ(state_.leak_check_calls, 1);
    EXPECT_EQ(state_.sigaction.query_calls, 1);
    EXPECT_EQ(state_.sigaction.set_calls, 0);
    EXPECT_EQ(state_.raise_calls, 0);
    EXPECT_EQ(state_.exit_calls, 0);
}

TEST_F(AsanSigtermHandlerTest, NullLeakCheckDoesNotCrash)
{
    state_.sigaction.oldact.sa_handler = SIG_DFL;
    state_.sigaction.oldact_set = true;

    swss_asan_sigterm_handler_impl(SIGTERM, nullptr, mock_sigaction, mock_exit, mock_raise);

    EXPECT_EQ(state_.leak_check_calls, 0);
    EXPECT_EQ(state_.sigaction.query_calls, 1);
    EXPECT_EQ(state_.raise_calls, 0);
    EXPECT_EQ(state_.exit_calls, 0);
}

TEST_F(AsanSigtermHandlerTest, OwnHandlerRestoresDefaultAndRaises)
{
    state_.sigaction.oldact.sa_handler = swss_asan_sigterm_handler;
    state_.sigaction.oldact_set = true;

    invoke_handler_impl();

    EXPECT_EQ(state_.leak_check_calls, 1);
    EXPECT_EQ(state_.sigaction.query_calls, 1);
    EXPECT_EQ(state_.sigaction.set_calls, 1);
    EXPECT_EQ(state_.sigaction.last_set.sa_handler, SIG_DFL);
    EXPECT_EQ(state_.raise_calls, 1);
    EXPECT_EQ(state_.raise_signo, SIGTERM);
    EXPECT_EQ(state_.exit_calls, 0);
}

TEST_F(AsanSigtermHandlerTest, AppHandlerOnlyRunsLeakCheck)
{
    // A non-ASAN handler means the application installed its own; after the
    // leak check the ASAN handler must not restore SIG_DFL or re-raise.
    state_.sigaction.oldact.sa_handler = SIG_IGN;
    state_.sigaction.oldact_set = true;

    invoke_handler_impl();

    EXPECT_EQ(state_.leak_check_calls, 1);
    EXPECT_EQ(state_.sigaction.query_calls, 1);
    EXPECT_EQ(state_.sigaction.set_calls, 0);
    EXPECT_EQ(state_.raise_calls, 0);
    EXPECT_EQ(state_.exit_calls, 0);
}

TEST_F(AsanSigtermHandlerTest, QuerySigactionFailureExits)
{
    state_.sigaction.rcs = {-1};

    invoke_handler_impl();

    EXPECT_EQ(state_.leak_check_calls, 1);
    EXPECT_EQ(state_.sigaction.query_calls, 1);
    EXPECT_EQ(state_.sigaction.set_calls, 0);
    EXPECT_EQ(state_.raise_calls, 0);
    EXPECT_EQ(state_.exit_calls, 1);
    EXPECT_EQ(state_.exit_status, EXIT_FAILURE);
}

TEST_F(AsanSigtermHandlerTest, RestoreDefaultSigactionFailureExits)
{
    state_.sigaction.oldact.sa_handler = swss_asan_sigterm_handler;
    state_.sigaction.oldact_set = true;
    // First call (query) succeeds; second call (set SIG_DFL) fails.
    state_.sigaction.rcs = {0, -1};

    invoke_handler_impl();

    EXPECT_EQ(state_.leak_check_calls, 1);
    EXPECT_EQ(state_.sigaction.query_calls, 1);
    EXPECT_EQ(state_.sigaction.set_calls, 1);
    EXPECT_EQ(state_.raise_calls, 0);
    EXPECT_EQ(state_.exit_calls, 1);
    EXPECT_EQ(state_.exit_status, EXIT_FAILURE);
}
