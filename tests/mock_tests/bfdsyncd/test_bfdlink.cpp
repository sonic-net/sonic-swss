#include "bfdsyncd/bfdlink.h"

#include <swss/netdispatcher.h>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace swss;
using namespace testing;

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

#define STATE_UPDATE_MSG_LEN 36
#define COUNTER_MSG_LEN 80

/* Use port 0 (kernel-assigned ephemeral) so parallel test runs and CI
 * environments where 50700 may be in use don't clash on bind(). The test
 * never accepts any real TCP traffic — it pumps wire bytes into
 * m_messageBuffer directly and calls handleBfdDpMessage(). */
static constexpr unsigned short BFD_TEST_PORT = 0;

class MockBfdLink : public BfdLink
{
public:
    MockBfdLink(DBConnector *db, DBConnector *stateDb, unsigned short port = BFD_TEST_PORT, int debug = 0)
        : BfdLink(db, stateDb, port, debug) {}
    MOCK_METHOD(bool, sendmsg, (uint16_t msglen), ());
    MOCK_METHOD(string, get_intf_mac, (const char* intf), (override));
};


class BfdSyncdTest : public ::testing::Test
{
public:
    void SetUp() override
    {
    }

    void TearDown() override
    {
    }

    DBConnector m_appl_db{"APPL_DB", 0};
    DBConnector m_state_db{"STATE_DB", 0};
    NiceMock<MockBfdLink>  m_bfd{&m_appl_db, &m_state_db, BFD_TEST_PORT, 1};

protected:
    /* Test-only helper: write raw bytes into BfdLink's reception buffer
     * so handleBfdDpMessage() can be driven without a real TCP peer.
     * The access goes through `m_bfd.m_messageBuffer`, which is private
     * on BfdLink — friendship is granted by the `friend class
     * ::BfdSyncdTest;` declaration in bfdlink.h. Centralising the access
     * in this helper means individual TEST_F bodies don't have to be
     * named friends one by one (gtest expands TEST_F(BfdSyncdTest, X)
     * into a class derived from BfdSyncdTest, and friendship in C++
     * does NOT inherit; placing the access in a method of BfdSyncdTest
     * itself keeps it within the friendship scope). */
    void injectMessageBuffer(const void *data, size_t len)
    {
        memcpy(m_bfd.m_messageBuffer, data, len);
    }
};

/* Regression for the link-local refusal: bfdsyncd must reject any
 * DP_ADD_SESSION whose ifindex != 0 (FRR's signal that this session is
 * pinned to a specific L3 interface, i.e. BGP-unnumbered or link-local
 * peering). The Broadcom DNX BFD PD driver doesn't implement inject-down
 * mode, so attempting to create such a session would silently bind it
 * to a drop next-hop in silicon. We refuse at the bfdsyncd layer so the
 * operator gets a clear ERROR in syslog instead. */
TEST_F(BfdSyncdTest, LinkLocalDpAddIsRefused)
{
    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);

    /* DP_ADD_SESSION for an IPv6 link-local peer:
     *   src = fe80::7aa4:3eff:fe72:ac00
     *   dst = fe80::7a11:8ff:fe55:d400
     *   ifindex = 0xff03 (non-zero — interface-bound)
     *   ifname = "Ethernet1_1"
     */
    unsigned char s[] = {
        0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x10, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0xa4, 0x3e, 0xff, 0xfe, 0x72, 0xac, 0x00, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0x11, 0x08, 0xff, 0xfe, 0x55, 0xd4, 0x00, 0x24, 0x08, 0xc7, 0x9e,
        0x00, 0x04, 0x93, 0xe0, 0x00, 0x04, 0x93, 0xe0, 0x00, 0x00, 0xc3, 0x50, 0x00, 0x00, 0xc3, 0x50,
        0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x45, 0x74, 0x68, 0x65,
        0x72, 0x6e, 0x65, 0x74, 0x31, 0x5f, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    /* Production code no longer needs to query the interface MAC from this
     * path. Assert get_intf_mac is not invoked so a future regression that
     * re-introduces that lookup fails loudly. */
    EXPECT_CALL(m_bfd, get_intf_mac(_)).Times(0);

    injectMessageBuffer(s, sizeof(s));
    m_bfd.handleBfdDpMessage(0);

    /* Session must NOT have landed in APP_DB. */
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);

    /* A subsequent state-update for the (never-created) key must also
     * be a no-op — m_key2bfd doesn't have it, so handleBfdStateUpdate
     * returns without calling sendmsg. */
    EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(0);
    std::vector<FieldValueTuple> fieldValues = { {"state", "Up"} };
    m_bfd.handleBfdStateUpdate("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400", fieldValues);
}

/* DP_REQUEST_SESSION_COUNTERS is handled before the ifindex gate, so
 * the link-local refusal does not apply to it. bfdsyncd builds a synthetic
 * BFD_SESSION_COUNTERS reply with all-zero counters (HW counters are not
 * read because the session is offloaded) and ships it back to bfdd via
 * sendmsg(80). */
TEST_F(BfdSyncdTest, CounterRequestTriggersResponse)
{
    /* Same byte layout as the DP_ADD_SESSION fixture but with the message
     * type set to 0x05 (DP_REQUEST_SESSION_COUNTERS). */
    unsigned char s[] = {
        0x01, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x10, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0xa4, 0x3e, 0xff, 0xfe, 0x72, 0xac, 0x00, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0x11, 0x08, 0xff, 0xfe, 0x55, 0xd4, 0x00, 0x24, 0x08, 0xc7, 0x9e,
        0x00, 0x04, 0x93, 0xe0, 0x00, 0x04, 0x93, 0xe0, 0x00, 0x00, 0xc3, 0x50, 0x00, 0x00, 0xc3, 0x50,
        0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x45, 0x74, 0x68, 0x65,
        0x72, 0x6e, 0x65, 0x74, 0x31, 0x5f, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    EXPECT_CALL(m_bfd, sendmsg(COUNTER_MSG_LEN)).Times(1);

    injectMessageBuffer(s, sizeof(s));
    m_bfd.handleBfdDpMessage(0);
}

/* Coverage for the B4 fix in handleBfdDpMessage: a frame with an unknown
 * message type (0x00FF) and otherwise valid header must be rejected by
 * bfd_msg_ok and cause an early return without writing to APP_DB. The
 * companion buffer-flush behaviour lives in readData() and would need a
 * socket-level test rig to exercise; the inline ERROR log + hexdump is
 * verified by inspection in that path.
 *
 * This regression-tests the silent-swallow behaviour: previously
 * handleBfdDpMessage would have run header parsing on garbage. */
TEST_F(BfdSyncdTest, MalformedMessageDoesNotWriteToAppDb)
{
    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);

    /* Same byte layout as the DP_ADD_SESSION fixture, but with the
     * msg type field (offset 2..3) set to 0x00FF — not in the small
     * set of known types {DP_ADD/DELETE/REQUEST_COUNTERS}. */
    unsigned char s[] = {
        0x01, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x10, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0xa4, 0x3e, 0xff, 0xfe, 0x72, 0xac, 0x00, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0x11, 0x08, 0xff, 0xfe, 0x55, 0xd4, 0x00, 0x24, 0x08, 0xc7, 0x9e,
        0x00, 0x04, 0x93, 0xe0, 0x00, 0x04, 0x93, 0xe0, 0x00, 0x00, 0xc3, 0x50, 0x00, 0x00, 0xc3, 0x50,
        0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x45, 0x74, 0x68, 0x65,
        0x72, 0x6e, 0x65, 0x74, 0x31, 0x5f, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    injectMessageBuffer(s, sizeof(s));

    /* No get_intf_mac call expected — handler must reject before
     * reaching the MAC-resolution stage. */
    EXPECT_CALL(m_bfd, get_intf_mac(_)).Times(0);

    m_bfd.handleBfdDpMessage(0);

    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}

/* Coverage for the B3 fix: an ifname containing shell metacharacters or
 * other suspicious bytes must be rejected by is_valid_ifname before it
 * propagates further into the handler. */
TEST_F(BfdSyncdTest, InjectionInIfnameIsRejected)
{
    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);

    /* Same DP_ADD_SESSION fixture as SingleMessageInBfdMessage, but with
     * the ifname bytes (offset 76..139, 64 bytes) overwritten with
     * "Ethernet0; touch /tmp/pwn" — should be rejected at validation
     * before any further processing. */
    unsigned char s[] = {
        0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x10, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0xa4, 0x3e, 0xff, 0xfe, 0x72, 0xac, 0x00, 0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7a, 0x11, 0x08, 0xff, 0xfe, 0x55, 0xd4, 0x00, 0x24, 0x08, 0xc7, 0x9e,
        0x00, 0x04, 0x93, 0xe0, 0x00, 0x04, 0x93, 0xe0, 0x00, 0x00, 0xc3, 0x50, 0x00, 0x00, 0xc3, 0x50,
        0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a,
        /* ifname [76..139] = "Ethernet0; touch /tmp/pwn\0..." */
        'E','t','h','e','r','n','e','t','0',';',' ','t','o','u','c','h',
        ' ','/','t','m','p','/','p','w','n', 0,  0,  0,  0,  0,  0,  0,
          0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
          0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    injectMessageBuffer(s, sizeof(s));

    /* If the validator works, get_intf_mac must NEVER be invoked with this
     * ifname (handler returns early on the rejection). */
    EXPECT_CALL(m_bfd, get_intf_mac(_)).Times(0);

    m_bfd.handleBfdDpMessage(0);

    /* Session must NOT have landed in APP_DB — handler must have returned
     * early at the is_valid_ifname check. */
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0);
}
