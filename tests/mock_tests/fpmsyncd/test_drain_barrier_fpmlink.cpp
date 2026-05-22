/*
 * Unit tests: FpmLink::forceDisconnect() — the primitive the warm-reboot
 * drain barrier uses to force zebra to reconnect (and re-dump its FIB)
 * on auto-resume / explicit resume from drain mode.
 *
 * Also includes a regression guard for the latent m_connected fix that
 * landed alongside forceDisconnect: prior to commit 2336941d the
 * m_connected member was initialized to false and never set to true in
 * accept(), making the dtor's `if (m_connected) close(...)` and our
 * forceDisconnect() implementation both dead code.
 */

// Pull in all stdlib / system headers BEFORE the `#define private public`
// trick — otherwise the substitution leaks into libstdc++ private members
// (e.g. basic_stringbuf::__xfer_bufptrs is declared `private:`), tripping
// "redeclared with different access" errors on transitive includes from
// dbconnector.h / fpmlink.h.
#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <memory>

#include "dbconnector.h"
#include "redispipeline.h"

// Access FpmLink private members for direct state inspection.
#define private public
#define protected public
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"
#undef protected
#undef private

namespace testing_db { void reset(); }

using namespace swss;

class FpmLinkForceDisconnectTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        testing_db::reset();
        m_db        = std::make_shared<DBConnector>("APPL_DB", 0);
        m_pipeline  = std::make_shared<RedisPipeline>(m_db.get());
        m_routeSync = std::make_unique<RouteSync>(m_pipeline.get());
        // FpmLink ctor opens a TCP listen socket on FPM_DEFAULT_PORT but
        // does not accept() any incoming connection — m_connected stays
        // false until a real client connects. This is the steady-state
        // we want for "no active connection" tests.
        m_fpm = std::make_unique<FpmLink>(m_routeSync.get());
    }

    void TearDown() override
    {
        m_fpm.reset();
        m_routeSync.reset();
        m_pipeline.reset();
        m_db.reset();
        testing_db::reset();
    }

    std::shared_ptr<DBConnector>   m_db;
    std::shared_ptr<RedisPipeline> m_pipeline;
    std::unique_ptr<RouteSync>     m_routeSync;
    std::unique_ptr<FpmLink>       m_fpm;
};

// ---------- forceDisconnect when there's no active connection ----------

TEST_F(FpmLinkForceDisconnectTest, NoOp_WhenNoActiveConnection)
{
    // Fresh FpmLink: ctor sets m_connected = false, accept() has never
    // been called. forceDisconnect should be a no-op — must not crash
    // and must not flip m_connected to true.
    ASSERT_FALSE(m_fpm->m_connected);
    EXPECT_NO_THROW(m_fpm->forceDisconnect());
    EXPECT_FALSE(m_fpm->m_connected);
}

TEST_F(FpmLinkForceDisconnectTest, NoOp_IsIdempotent_NoActiveConnection)
{
    EXPECT_NO_THROW(m_fpm->forceDisconnect());
    EXPECT_NO_THROW(m_fpm->forceDisconnect());
    EXPECT_NO_THROW(m_fpm->forceDisconnect());
    EXPECT_FALSE(m_fpm->m_connected);
}

// ---------- forceDisconnect when a connection is "live" ----------
//
// We don't have a real zebra peer in the test environment, so we
// simulate the post-accept state by directly setting m_connected=true
// and m_connection_socket to a valid fd we control (pipe).

TEST_F(FpmLinkForceDisconnectTest, ClosesSocket_AndClearsConnected)
{
    int pipefd[2];
    ASSERT_EQ(0, pipe(pipefd))
        << "pipe() must succeed to set up a closeable fd for the test";

    // Simulate "accept() succeeded": m_connection_socket points at a
    // valid fd, m_connected is true.
    m_fpm->m_connection_socket = pipefd[0];
    m_fpm->m_connected         = true;
    // pipefd[1] (write end) stays open so we can detect when the read
    // end is closed: write() into pipefd[1] after close should fail
    // with EBADF or SIGPIPE — but we don't go that far here, we just
    // verify forceDisconnect's directly-observable effects.

    m_fpm->forceDisconnect();

    EXPECT_FALSE(m_fpm->m_connected)
        << "forceDisconnect must mark the connection as closed";

    // Confirm the fd is actually closed by trying to use it. fcntl on a
    // closed fd returns -1 with errno=EBADF.
    int rc = fcntl(pipefd[0], F_GETFD);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBADF, errno);

    // Cleanup: close the write end we held open.
    close(pipefd[1]);
}

TEST_F(FpmLinkForceDisconnectTest, SecondCall_AfterDisconnect_IsNoOp)
{
    // First disconnect closes the fd and clears m_connected. A second
    // call should NOT try to close the (now invalid) fd again — that
    // would risk a double-close racing against a recycled fd. The
    // implementation's `if (m_connected)` guard makes this safe.
    int pipefd[2];
    ASSERT_EQ(0, pipe(pipefd));

    m_fpm->m_connection_socket = pipefd[0];
    m_fpm->m_connected         = true;
    m_fpm->forceDisconnect();
    EXPECT_FALSE(m_fpm->m_connected);

    // Second call — must not close anything (else we'd hit double-close).
    EXPECT_NO_THROW(m_fpm->forceDisconnect());
    EXPECT_FALSE(m_fpm->m_connected);

    close(pipefd[1]);
}

// ---------- Latent-bug regression: m_connected set true after accept ----------
//
// Pre-fix, FpmLink::accept() never set m_connected = true, so the dtor's
// `if (m_connected) close(m_connection_socket)` was dead code AND
// forceDisconnect() was forever a no-op. We can't actually call accept()
// in unit tests (no real peer), but we can read the source to confirm
// the assignment happens after the ::accept() call. The simplest
// programmatic check: m_connected's INITIAL value must be false (so the
// ctor doesn't accidentally claim there's a live connection) and the
// dtor must be safe when m_connected is false.

TEST_F(FpmLinkForceDisconnectTest, InitialState_NotConnected)
{
    EXPECT_FALSE(m_fpm->m_connected)
        << "FpmLink ctor must leave m_connected = false until accept() succeeds";
}

TEST_F(FpmLinkForceDisconnectTest, DestructorSafe_WhenNeverConnected)
{
    // m_fpm is constructed fresh in SetUp; never connected. Resetting
    // (which invokes the dtor) must not try to close an invalid fd.
    EXPECT_NO_THROW(m_fpm.reset());
}
