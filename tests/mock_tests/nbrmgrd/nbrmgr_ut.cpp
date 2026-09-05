#include "gtest/gtest.h"
#include <iostream>
#include <netlink/netlink.h>
#include <netlink/msg.h>
#include <linux/neighbour.h>
#include <net/if.h>
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
static int mockExecResult;

/*
 * Wrap netlink and interface functions to avoid real kernel interaction.
 * setNeighbor() calls nl_socket_alloc, nl_connect, if_nametoindex, and
 * nl_send_auto. We intercept nl_send_auto to record the call and
 * if_nametoindex to return a dummy index.
 */
extern "C" {

struct nl_sock *__wrap_nl_socket_alloc(void)
{
    /* Return a non-null fake pointer; nl_sock is opaque so cast from raw memory */
    static char fake_sock_mem[256];
    return reinterpret_cast<struct nl_sock *>(fake_sock_mem);
}

int __wrap_nl_connect(struct nl_sock *sk, int protocol)
{
    return 0;
}

int __wrap_nl_send_auto(struct nl_sock *sk, struct nl_msg *msg)
{
    struct nlmsghdr *hdr = nlmsg_hdr(msg);
    struct ndmsg *nd = static_cast<struct ndmsg *>(NLMSG_DATA(hdr));
    capturedNeighborRequests.push_back(
        {nd->ndm_state, nd->ndm_flags, nd->ndm_family, hdr->nlmsg_flags});
    operationOrder.push_back("netlink");
    return mockNlSendResult;
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
            mockExecResult = 0;
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
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::1");

        ASSERT_EQ(capturedNeighborRequests.size(), 1u);
        EXPECT_EQ(capturedNeighborRequests[0].state, NUD_INCOMPLETE);
        EXPECT_EQ(capturedNeighborRequests[0].neighborFlags, 0u);
        EXPECT_EQ(capturedNeighborRequests[0].family, AF_INET6);
        EXPECT_EQ(capturedNeighborRequests[0].messageFlags & NLM_F_CREATE, 0);
        EXPECT_NE(capturedNeighborRequests[0].messageFlags & NLM_F_REPLACE, 0);
        EXPECT_NE(capturedNeighborRequests[0].messageFlags & NLM_F_ACK, 0);

        ASSERT_EQ(mockCallArgs.size(), 1u);
        EXPECT_EQ(mockCallArgs[0], "/usr/bin/ndisc6 -q -w 0 -1 \"2001:db8::1\" \"Vlan1000\"");
        EXPECT_EQ(operationOrder, (std::vector<std::string>{"netlink", "ndisc6"}));
    }

    TEST_F(NbrMgrTest, NetlinkFailureSkipsSolicitation)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        mockNlSendResult = -NLE_FAILURE;
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::2");

        ASSERT_EQ(capturedNeighborRequests.size(), 1u);
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(operationOrder, (std::vector<std::string>{"netlink"}));
    }

    TEST_F(NbrMgrTest, SolicitationFailureAfterNetlinkUpdate)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        mockExecResult = 1;
        processFailedNeighborRequest(nbrmgr, "Vlan1000:2001:db8::3");

        EXPECT_EQ(capturedNeighborRequests.size(), 1u);
        EXPECT_EQ(mockCallArgs.size(), 1u);
    }

    TEST_F(NbrMgrTest, RejectsIpv4FailedNeighborRequest)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        processFailedNeighborRequest(nbrmgr, "Vlan1000:192.0.2.1");

        EXPECT_TRUE(capturedNeighborRequests.empty());
        EXPECT_TRUE(mockCallArgs.empty());
    }

    TEST_F(NbrMgrTest, RejectsMalformedFailedNeighborRequest)
    {
        std::vector<std::string> cfg_nbr_tables = {CFG_NEIGH_TABLE_NAME};
        TestableNbrMgr nbrmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_nbr_tables);
        processFailedNeighborRequest(nbrmgr, "invalid-key");

        EXPECT_TRUE(capturedNeighborRequests.empty());
        EXPECT_TRUE(mockCallArgs.empty());
    }
}
