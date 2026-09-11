#include "gtest/gtest.h"
#include <cstddef>
#include <iostream>
#include <netlink/netlink.h>
#include <netlink/msg.h>
#include <linux/neighbour.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "../mock_table.h"
#include "warm_restart.h"
#include "nbrmgr.h"

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

struct CapturedNeighborRequest
{
    int state;
    unsigned int neighborFlags;
    int family;
    int messageFlags;
};

static std::vector<CapturedNeighborRequest> capturedNeighborRequests;
static std::vector<std::string> operationOrder;
static int mockNlSendResult;
static int mockNlAckResult;
static int mockExecResult;
static bool mockAutoAckDisabled;
static int mockNlSocketAllocCount;
static int mockNlSocketFreeCount;
static int mockNlConnectCount;
static bool mockAckTimeoutConfigured;
static struct timeval mockAckTimeout;
static struct nl_sock *mockPersistentSocket;
static std::vector<struct nl_sock *> mockAckSockets;
static std::vector<struct nl_sock *> mockSendSockets;
static std::vector<struct nl_sock *> mockAckWaitSockets;
static struct nl_sock *mockAutoAckDisabledSocket;
static const struct nl_sock *mockTimeoutSocket;
static std::vector<struct nl_sock *> mockFreedSockets;

/*
 * Wrap netlink and interface functions to avoid real kernel interaction.
 * setNeighbor() calls nl_socket_alloc, nl_connect, if_nametoindex, and
 * nl_send_auto. We intercept nl_send_auto to record the call and
 * if_nametoindex to return a dummy index.
 */
extern "C" {

struct nl_sock *__wrap_nl_socket_alloc(void)
{
    static std::max_align_t fakeSockets[8];
    struct nl_sock *sock = reinterpret_cast<struct nl_sock *>(
        &fakeSockets[mockNlSocketAllocCount]);
    if (mockNlSocketAllocCount == 0)
    {
        mockPersistentSocket = sock;
    }
    else
    {
        mockAckSockets.push_back(sock);
    }
    mockNlSocketAllocCount++;
    return sock;
}

void __wrap_nl_socket_free(struct nl_sock *sk)
{
    mockNlSocketFreeCount++;
    mockFreedSockets.push_back(sk);
}

int __wrap_nl_connect(struct nl_sock *sk, int protocol)
{
    mockNlConnectCount++;
    return 0;
}

int __wrap_nl_send_auto(struct nl_sock *sk, struct nl_msg *msg)
{
    struct nlmsghdr *hdr = nlmsg_hdr(msg);
    struct ndmsg *nd = static_cast<struct ndmsg *>(NLMSG_DATA(hdr));
    capturedNeighborRequests.push_back(
        {nd->ndm_state, nd->ndm_flags, nd->ndm_family, hdr->nlmsg_flags});
    mockSendSockets.push_back(sk);
    operationOrder.push_back("netlink");
    return mockNlSendResult;
}

int __wrap_nl_wait_for_ack(struct nl_sock *sk)
{
    mockAckWaitSockets.push_back(sk);
    operationOrder.push_back("ack");
    return mockNlAckResult;
}

void __wrap_nl_socket_disable_auto_ack(struct nl_sock *sk)
{
    mockAutoAckDisabled = true;
    mockAutoAckDisabledSocket = sk;
}

int __wrap_nl_socket_get_fd(const struct nl_sock *sk)
{
    mockTimeoutSocket = sk;
    return 42;
}

static int mockSetsockopt(int socket, int level, int optionName,
                          const void *optionValue, socklen_t optionLength)
{
    if (level == SOL_SOCKET && optionName == SO_RCVTIMEO &&
        optionLength == sizeof(struct timeval))
    {
        mockAckTimeout = *static_cast<const struct timeval *>(optionValue);
        mockAckTimeoutConfigured = true;
    }
    return 0;
}

int __wrap_setsockopt(int socket, int level, int optionName,
                      const void *optionValue, socklen_t optionLength)
{
    return mockSetsockopt(socket, level, optionName, optionValue, optionLength);
}

int __wrap___setsockopt64(int socket, int level, int optionName,
                          const void *optionValue, socklen_t optionLength)
{
    return mockSetsockopt(socket, level, optionName, optionValue, optionLength);
}

/* Control whether nlmsg_alloc returns NULL to simulate setNeighbor failure */
static bool mock_nlmsg_alloc_fail = false;

struct nl_msg *__real_nlmsg_alloc(void);

struct nl_msg *__wrap_nlmsg_alloc(void)
{
    if (mock_nlmsg_alloc_fail)
    {
        return nullptr;
    }
    return __real_nlmsg_alloc();
}

unsigned int __wrap_if_nametoindex(const char *ifname)
{
    /* Return a dummy interface index */
    return 1;
}

}

int noop_cb(const std::string &cmd, std::string &out){
    mockCallArgs.push_back(cmd);
    operationOrder.push_back("ndisc6");
    return mockExecResult;
}

namespace nbrmgr_ut
{
    class TestableNbrMgr : public swss::NbrMgr
    {
    public:
        using swss::NbrMgr::NbrMgr;
        using Orch::getExecutor;
    };

    struct NbrMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::WarmStart::initialize("nbrmgrd", "swss");

            mockCallArgs.clear();
            capturedNeighborRequests.clear();
            operationOrder.clear();
            mock_nlmsg_alloc_fail = false;
            mockNlSendResult = 0;
            mockNlAckResult = 0;
            mockExecResult = 0;
            mockAutoAckDisabled = false;
            mockNlSocketAllocCount = 0;
            mockNlSocketFreeCount = 0;
            mockNlConnectCount = 0;
            mockAckTimeoutConfigured = false;
            memset(&mockAckTimeout, 0, sizeof(mockAckTimeout));
            mockPersistentSocket = nullptr;
            mockAckSockets.clear();
            mockSendSockets.clear();
            mockAckWaitSockets.clear();
            mockAutoAckDisabledSocket = nullptr;
            mockTimeoutSocket = nullptr;
            mockFreedSockets.clear();
            callback = noop_cb;
        }

        void processFailedNeighborRequest(TestableNbrMgr& nbrmgr, const std::string& key)
        {
            swss::Table failedNeighTable(m_app_db.get(), APP_NEIGH_FAILED_TABLE_NAME);
            failedNeighTable.set(key, {{"NULL", "NULL"}});
            std::vector<swss::FieldValueTuple> values;
            ASSERT_TRUE(failedNeighTable.get(key, values));

            auto executor = nbrmgr.getExecutor(APP_NEIGH_FAILED_TABLE_NAME);
            ASSERT_NE(executor, nullptr);
            executor->execute();
        }

        void enableDualTor()
        {
            swss::Table peerSwitchTable(m_config_db.get(), CFG_PEER_SWITCH_TABLE_NAME);
            peerSwitchTable.set("peer_switch_hostname", {{"address_ipv4", "10.0.0.1"}});
        }

        bool hasPendingFailedNeighborTask(TestableNbrMgr& nbrmgr)
        {
            auto consumer = dynamic_cast<Consumer *>(nbrmgr.getExecutor(APP_NEIGH_FAILED_TABLE_NAME));
            EXPECT_NE(consumer, nullptr);
            return consumer && !consumer->m_toSync.empty();
        }
    };

    /*
     * Test that when NEIGH_RESOLVE_TABLE is empty at startup,
     * NbrMgr constructs successfully without errors.
     */
    TEST_F(NbrMgrTest, ReconcileEmptyTable)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        /* No entries in NEIGH_RESOLVE_TABLE - should construct without issue */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        /* Verify the table is indeed empty */
        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<std::string> keys;
        neighResolveTable.getKeys(keys);
        ASSERT_TRUE(keys.empty());
    }

    /*
     * Test that pre-existing entries in NEIGH_RESOLVE_TABLE are
     * picked up and processed during NbrMgr construction.
     */
    TEST_F(NbrMgrTest, ReconcilePendingEntries)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        /* Pre-populate NEIGH_RESOLVE_TABLE with entries (simulating entries
         * left over from before a restart) */
        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("family", "IPv4");
        neighResolveTable.set("Ethernet0:10.0.0.1", fvs);
        neighResolveTable.set("Ethernet4:10.0.0.3", fvs);

        /* Verify entries exist before construction */
        std::vector<std::string> keys;
        neighResolveTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 2u);

        /* Construct NbrMgr - reconciliation should process these entries */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        /* NbrMgr constructor calls setNeighbor for each entry via
         * reconcileNeighResolveTable. Since setNeighbor uses netlink
         * (wrapped here), we verify construction succeeds without error.
         * The entries are resolved via netlink, not removed from the table
         * (orchagent removes them after neighbor is learned). */
    }

    /*
     * Test reconciliation with IPv6 entries.
     */
    TEST_F(NbrMgrTest, ReconcileIPv6Entries)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("family", "IPv6");
        neighResolveTable.set("Ethernet0:2000::2", fvs);
        neighResolveTable.set("Ethernet4:2001::2", fvs);

        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        /* Verify construction completes - IPv6 entries are reconciled */
    }

    /*
     * Test that entries with invalid key format (no ':' separator)
     * are skipped during reconciliation.
     */
    TEST_F(NbrMgrTest, ReconcileInvalidKeyFormat)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        /* Valid entry */
        neighResolveTable.set("Ethernet0:10.0.0.1", fvs);
        /* Invalid entry - no ':' separator */
        neighResolveTable.set("InvalidKeyNoSeparator", fvs);

        std::vector<std::string> keys;
        neighResolveTable.getKeys(keys);
        ASSERT_EQ(keys.size(), 2u);

        /* Should not crash; invalid key is skipped, valid key is processed */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
    }

    /*
     * Test that setNeighbor failure during reconciliation is handled
     * gracefully (logs warning, continues with remaining entries).
     */
    TEST_F(NbrMgrTest, ReconcileSetNeighborFailure)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};

        swss::Table neighResolveTable(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
        std::vector<swss::FieldValueTuple> fvs;
        neighResolveTable.set("Ethernet0:10.0.0.1", fvs);
        neighResolveTable.set("Ethernet4:10.0.0.3", fvs);

        /* Force nlmsg_alloc to fail, causing setNeighbor to return false */
        mock_nlmsg_alloc_fail = true;

        /* Should not crash; failures are logged as warnings */
        swss::NbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
    }

    TEST_F(NbrMgrTest, ProcessFailedIpv6Neighbor)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::1");

        EXPECT_TRUE(mockAutoAckDisabled);
        EXPECT_EQ(mockNlSocketAllocCount, 2);
        EXPECT_EQ(mockNlSocketFreeCount, 1);
        EXPECT_EQ(mockNlConnectCount, 2);
        EXPECT_TRUE(mockAckTimeoutConfigured);
        EXPECT_EQ(mockAckTimeout.tv_sec, 1);
        EXPECT_EQ(mockAckTimeout.tv_usec, 0);
        ASSERT_EQ(mockAckSockets.size(), 1u);
        ASSERT_EQ(mockSendSockets.size(), 1u);
        ASSERT_EQ(mockAckWaitSockets.size(), 1u);
        ASSERT_EQ(mockFreedSockets.size(), 1u);
        EXPECT_NE(mockAckSockets[0], mockPersistentSocket);
        EXPECT_EQ(mockAutoAckDisabledSocket, mockPersistentSocket);
        EXPECT_EQ(mockSendSockets[0], mockAckSockets[0]);
        EXPECT_EQ(mockAckWaitSockets[0], mockAckSockets[0]);
        EXPECT_EQ(mockTimeoutSocket, mockAckSockets[0]);
        EXPECT_EQ(mockFreedSockets[0], mockAckSockets[0]);
        ASSERT_EQ(capturedNeighborRequests.size(), 1u);
        EXPECT_EQ(capturedNeighborRequests[0].state, NUD_INCOMPLETE);
        EXPECT_EQ(capturedNeighborRequests[0].neighborFlags, 0u);
        EXPECT_EQ(capturedNeighborRequests[0].family, AF_INET6);
        EXPECT_EQ(capturedNeighborRequests[0].messageFlags & NLM_F_CREATE, 0);
        EXPECT_NE(capturedNeighborRequests[0].messageFlags & NLM_F_REPLACE, 0);
        EXPECT_NE(capturedNeighborRequests[0].messageFlags & NLM_F_ACK, 0);

        ASSERT_EQ(mockCallArgs.size(), 1u);
        EXPECT_EQ(mockCallArgs[0], "/usr/bin/ndisc6 -q -r 1 -w 0 \"2001:db8::1\" \"Vlan1000\"");
        EXPECT_EQ(operationOrder, (std::vector<std::string>{"netlink", "ack", "ndisc6"}));
        EXPECT_FALSE(hasPendingFailedNeighborTask(nbrmgr));
    }

    TEST_F(NbrMgrTest, NetlinkSendFailureRetries)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        mockNlSendResult = -NLE_FAILURE;
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::2");

        ASSERT_EQ(capturedNeighborRequests.size(), 1u);
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(operationOrder, (std::vector<std::string>{"netlink"}));
        EXPECT_TRUE(hasPendingFailedNeighborTask(nbrmgr));

        mockNlSendResult = 0;
        nbrmgr.doTask();

        EXPECT_EQ(operationOrder,
                  (std::vector<std::string>{"netlink", "netlink", "ack", "ndisc6"}));
        EXPECT_FALSE(hasPendingFailedNeighborTask(nbrmgr));
    }

    TEST_F(NbrMgrTest, NetlinkAckTimeoutRetries)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        mockNlAckResult = -NLE_AGAIN;
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::3");

        EXPECT_EQ(operationOrder, (std::vector<std::string>{"netlink", "ack"}));
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_TRUE(hasPendingFailedNeighborTask(nbrmgr));
        EXPECT_EQ(mockNlSocketAllocCount, 2);
        EXPECT_EQ(mockNlSocketFreeCount, 1);

        mockNlAckResult = 0;
        nbrmgr.doTask();

        EXPECT_EQ(operationOrder,
                  (std::vector<std::string>{"netlink", "ack", "netlink", "ack", "ndisc6"}));
        EXPECT_FALSE(hasPendingFailedNeighborTask(nbrmgr));
        EXPECT_EQ(mockNlSocketAllocCount, 3);
        EXPECT_EQ(mockNlSocketFreeCount, 2);
    }

    TEST_F(NbrMgrTest, NoSolicitationResponseIsSuccess)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        mockExecResult = 2;
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::4");

        EXPECT_EQ(capturedNeighborRequests.size(), 1u);
        EXPECT_EQ(mockCallArgs.size(), 1u);
        EXPECT_FALSE(hasPendingFailedNeighborTask(nbrmgr));
    }

    TEST_F(NbrMgrTest, SolicitationExecutionFailureRetries)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        mockExecResult = 1;
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::5");

        EXPECT_EQ(capturedNeighborRequests.size(), 1u);
        EXPECT_EQ(mockCallArgs.size(), 1u);
        EXPECT_TRUE(hasPendingFailedNeighborTask(nbrmgr));

        mockExecResult = 0;
        nbrmgr.doTask();

        EXPECT_EQ(capturedNeighborRequests.size(), 2u);
        EXPECT_EQ(mockCallArgs.size(), 2u);
        EXPECT_EQ(operationOrder,
                  (std::vector<std::string>{
                      "netlink", "ack", "ndisc6",
                      "netlink", "ack", "ndisc6",
                  }));
        EXPECT_FALSE(hasPendingFailedNeighborTask(nbrmgr));
    }

    TEST_F(NbrMgrTest, RejectsIpv4FailedNeighborRequest)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        processFailedNeighborRequest(nbrmgr, "Vlan1000:192.0.2.1");

        EXPECT_TRUE(capturedNeighborRequests.empty());
        EXPECT_TRUE(mockCallArgs.empty());
    }

    TEST_F(NbrMgrTest, RejectsMalformedFailedNeighborRequest)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        processFailedNeighborRequest(nbrmgr, "invalid-key");

        EXPECT_TRUE(capturedNeighborRequests.empty());
        EXPECT_TRUE(mockCallArgs.empty());
    }

    TEST_F(NbrMgrTest, RejectsEmptyInterfaceFailedNeighborRequest)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        enableDualTor();
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        processFailedNeighborRequest(nbrmgr, ":2001:db8::6");

        EXPECT_TRUE(capturedNeighborRequests.empty());
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_FALSE(hasPendingFailedNeighborTask(nbrmgr));
    }

    TEST_F(NbrMgrTest, DoesNotSubscribeToFailedNeighborTableOnNonDualTor)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);

        EXPECT_EQ(nbrmgr.getExecutor(APP_NEIGH_FAILED_TABLE_NAME), nullptr);
    }
}
