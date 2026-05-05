#include "asan.h"

#include <unistd.h>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{

struct AsanTestState
{
    int sigaction_calls = 0;
    int sigaction_rc = 0;
    int last_sig = 0;
    struct sigaction installed = {};

    int access_calls = 0;
    int access_rc = -1;
    std::string access_path;

    int malloc_calls = 0;
    size_t malloc_size = 0;
    // When true, mock_malloc returns nullptr. Otherwise it returns storage.data().
    bool malloc_fail = false;
    std::vector<unsigned char> storage;
};

AsanTestState *g_state = nullptr;

int mock_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    EXPECT_NE(g_state, nullptr);
    g_state->sigaction_calls++;
    g_state->last_sig = sig;
    if (act)
    {
        g_state->installed = *act;
    }
    (void)oldact;
    return g_state->sigaction_rc;
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

    EXPECT_EQ(state_.sigaction_calls, 1);
    EXPECT_EQ(state_.last_sig, SIGTERM);
    EXPECT_EQ(state_.installed.sa_handler, swss_asan_sigterm_handler);
    EXPECT_EQ(state_.access_calls, 1);
    EXPECT_EQ(state_.access_path, "/etc/sonic/inject_asan_test_leak_enabled");
    EXPECT_EQ(state_.malloc_calls, 0);
}

TEST_F(AsanInitTest, SigactionFailureReturnsFalse)
{
    state_.sigaction_rc = -1;

    EXPECT_FALSE(swss_asan_init_impl(mock_sigaction, mock_access, mock_malloc, mock_leak_check));

    EXPECT_EQ(state_.sigaction_calls, 1);
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
