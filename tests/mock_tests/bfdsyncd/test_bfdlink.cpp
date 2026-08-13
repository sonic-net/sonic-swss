#include "bfdsyncd/bfdlink.h"
#include "bfdsyncd/bfdd/bfddp_packet.h"

#include <swss/netdispatcher.h>
#include "mock_table.h"

#include <arpa/inet.h>
#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace swss;
using namespace testing;

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

#define STATE_UPDATE_MSG_LEN (sizeof(bfddp_message_header) + sizeof(bfddp_state_change))
#define COUNTER_MSG_LEN (sizeof(bfddp_message_header) + sizeof(bfddp_session_counters))

/* Use port 0 (kernel-assigned ephemeral) so parallel test runs and CI
 * environments where 50700 may be in use don't clash on bind(). */
static constexpr unsigned short BFD_TEST_PORT = 0;

static const size_t BFD_WIRE_MSG_LEN = sizeof(bfddp_message_header) + sizeof(bfddp_session);

static void buildIpv6SessionWire(unsigned char *buf,
                                 size_t bufSize,
                                 uint16_t dpType,
                                 const char *srcIp,
                                 const char *dstIp,
                                 uint32_t ifindex,
                                 const char *ifname,
                                 uint32_t minRxUs,
                                 uint32_t minTxUs,
                                 uint8_t detectMult)
{
    bfddp_message msg = {};
    struct in6_addr src6 = {};
    struct in6_addr dst6 = {};

    ASSERT_GE(bufSize, BFD_WIRE_MSG_LEN);
    ASSERT_EQ(inet_pton(AF_INET6, srcIp, &src6), 1);
    ASSERT_EQ(inet_pton(AF_INET6, dstIp, &dst6), 1);

    msg.header.version = BFD_DP_VERSION;
    msg.header.type = htons(dpType);
    msg.header.length = htons(static_cast<uint16_t>(BFD_WIRE_MSG_LEN));
    msg.data.session.flags = htonl(SESSION_IPV6);
    memcpy(&msg.data.session.src, &src6, sizeof(src6));
    memcpy(&msg.data.session.dst, &dst6, sizeof(dst6));
    msg.data.session.lid = htonl(0x20);
    msg.data.session.min_rx = htonl(minRxUs);
    msg.data.session.min_tx = htonl(minTxUs);
    msg.data.session.detect_mult = detectMult;
    msg.data.session.ifindex = htonl(ifindex);
    if (ifname != nullptr)
    {
        strncpy(msg.data.session.ifname, ifname, IFNAME_LEN - 1);
    }

    memcpy(buf, &msg, BFD_WIRE_MSG_LEN);
}

static void copyDefaultIpv6AddBuffer(unsigned char *buf, size_t bufSize)
{
    buildIpv6SessionWire(buf, bufSize, DP_ADD_SESSION,
                         "2000::1", "2000::2", 0, nullptr,
                         300000, 300000, 3);
}

static void buildIpv4SessionWire(unsigned char *buf,
                                 size_t bufSize,
                                 uint16_t dpType,
                                 const char *srcIp,
                                 const char *dstIp,
                                 uint32_t ifindex,
                                 const char *ifname,
                                 uint32_t minRxUs,
                                 uint32_t minTxUs,
                                 uint8_t detectMult)
{
    bfddp_message msg = {};
    struct in_addr src4 = {};
    struct in_addr dst4 = {};

    ASSERT_GE(bufSize, BFD_WIRE_MSG_LEN);
    ASSERT_EQ(inet_pton(AF_INET, srcIp, &src4), 1);
    ASSERT_EQ(inet_pton(AF_INET, dstIp, &dst4), 1);

    msg.header.version = BFD_DP_VERSION;
    msg.header.type = htons(dpType);
    msg.header.length = htons(static_cast<uint16_t>(BFD_WIRE_MSG_LEN));
    msg.data.session.flags = htonl(0);
    memcpy(&msg.data.session.src, &src4, sizeof(src4));
    memcpy(&msg.data.session.dst, &dst4, sizeof(dst4));
    msg.data.session.lid = htonl(0x20);
    msg.data.session.min_rx = htonl(minRxUs);
    msg.data.session.min_tx = htonl(minTxUs);
    msg.data.session.detect_mult = detectMult;
    msg.data.session.ifindex = htonl(ifindex);
    if (ifname != nullptr)
    {
        strncpy(msg.data.session.ifname, ifname, IFNAME_LEN - 1);
    }

    memcpy(buf, &msg, BFD_WIRE_MSG_LEN);
}

class MockBfdLink : public BfdLink
{
public:
    MockBfdLink(DBConnector *db, DBConnector *stateDb, unsigned short port = BFD_TEST_PORT, int debug = 0):BfdLink(db, stateDb, port, debug){}
    MOCK_METHOD(bool, sendmsg, (uint16_t msglen), ());
    MOCK_METHOD(string, exec, (const char* cmd), (override));
};

static void addDefaultIpv6Session(MockBfdLink &bfd)
{
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    memcpy(bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    bfd.handleBfdDpMessage(0);
}


class BfdSyncdTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        ::testing_db::reset();
    }

    void TearDown() override
    {
    }

    DBConnector m_appl_db{"APPL_DB", 0};
    DBConnector m_state_db{"STATE_DB", 0};
    NiceMock<MockBfdLink>  m_bfd{&m_appl_db, &m_state_db, BFD_TEST_PORT, 1};

};

TEST_F(BfdSyncdTest, SingleMessageInBfdMessage)
{
    shared_ptr<swss::DBConnector> app_db;
    app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);

    //Create BFD session
    unsigned char s[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(s, sizeof(s));

    memcpy(m_bfd.m_messageBuffer, static_cast<void *>(s), BFD_WIRE_MSG_LEN);

    m_bfd.handleBfdDpMessage(0);

    //Check APPL DB
    {
        vector<string> keys;
        vector<FieldValueTuple> fieldValues;
        string multihop;
        string local_addr;
        string rx_interval;
        string tx_interval;
        string multiplier;

        app_bfd_session_table.getKeys(keys);
        ASSERT_EQ(keys.size(), 1);

        app_bfd_session_table.get(keys[0], fieldValues);
        for (const auto& fv: fieldValues)
        {
            const auto& field = fvField(fv);
            const auto& value = fvValue(fv);

            if (field == "multihop")    multihop = value;
            if (field == "local_addr")  local_addr = value;
            if (field == "rx_interval") rx_interval = value;
            if (field == "tx_interval") tx_interval = value;
            if (field == "multiplier" ) multiplier = value;
        }
        ASSERT_EQ( multihop, "false");
        ASSERT_EQ( local_addr, "2000::1");
        ASSERT_EQ( rx_interval, "300");
        ASSERT_EQ( tx_interval, "300");
        ASSERT_EQ( multiplier, "3");
    }

    //Update BFD session state, call sendmsg
    {
        EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(1);

        std::vector<FieldValueTuple> fieldValues = {
            {"state", "Up"},
        };
        auto key = string("default|default|2000::2");
        m_bfd.handleBfdStateUpdate(key, fieldValues);

    }

    //handle counter request
    {
        EXPECT_CALL(m_bfd, sendmsg(COUNTER_MSG_LEN)).Times(1);

        unsigned char s[BFD_WIRE_MSG_LEN];
        copyDefaultIpv6AddBuffer(s, sizeof(s));
        s[2] = 0;
        s[3] = DP_REQUEST_SESSION_COUNTERS;
        memcpy(m_bfd.m_messageBuffer, static_cast<void *>(s), BFD_WIRE_MSG_LEN);

        m_bfd.handleBfdDpMessage(0);
    }

    //Delete BFD session and Check APPL DB
    {
        unsigned char s[BFD_WIRE_MSG_LEN];
        copyDefaultIpv6AddBuffer(s, sizeof(s));
        s[2] = 0;
        s[3] = DP_DELETE_SESSION;
        memcpy(m_bfd.m_messageBuffer, static_cast<void *>(s), BFD_WIRE_MSG_LEN);

        m_bfd.handleBfdDpMessage(0);

        vector<string> keys;
        vector<FieldValueTuple> fieldValues;

        app_bfd_session_table.getKeys(keys);
        ASSERT_EQ(keys.size(), 0);
    }

    //Update BFD session state, expecting session is not found, no message sent
    {
        EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(0);

        std::vector<FieldValueTuple> fieldValues = {
            {"state", "Up"},
        };
        auto key = string("default|default|2000::2");
        m_bfd.handleBfdStateUpdate(key, fieldValues);
    }
}

TEST_F(BfdSyncdTest, InvalidBfdDpMessage)
{
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    buf[6] = 0;
    buf[7] = 4;

    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);
    memcpy(m_bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    m_bfd.handleBfdDpMessage(0);
}

TEST_F(BfdSyncdTest, StateUpdateWithRemoteFields)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(1).WillOnce(Return(true));

    std::vector<FieldValueTuple> fieldValues = {
        {"state", "Down"},
        {"remote_discriminator", "42"},
        {"remote_min_rx", "100000"},
        {"remote_min_tx", "200000"},
        {"remote_multiplier", "3"},
    };
    auto key = string("default|default|2000::2");
    ASSERT_TRUE(m_bfd.handleBfdStateUpdate(key, fieldValues));
}

TEST_F(BfdSyncdTest, StateUpdateInvalidRemoteField)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);

    std::vector<FieldValueTuple> fieldValues = {
        {"state", "Up"},
        {"remote_discriminator", "not-a-number"},
    };
    auto key = string("default|default|2000::2");
    ASSERT_FALSE(m_bfd.handleBfdStateUpdate(key, fieldValues));
}

TEST_F(BfdSyncdTest, StateUpdateInvalidKey)
{
    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);

    std::vector<FieldValueTuple> fieldValues = {{"state", "Up"}};
    ASSERT_FALSE(m_bfd.handleBfdStateUpdate("default|Ethernet1|not-an-ip", fieldValues));
}

TEST_F(BfdSyncdTest, DuplicateAddIgnore)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));

    addDefaultIpv6Session(m_bfd);

    Table stateTable(&m_state_db, STATE_BFD_SESSION_TABLE_NAME);
    stateTable.set("default|default|2000::2", {{"state", "Up"}});

    EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(1);
    addDefaultIpv6Session(m_bfd);

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1u);
}

TEST_F(BfdSyncdTest, DuplicateAddRecreate)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));

    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    memcpy(m_bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    m_bfd.handleBfdDpMessage(0);

    const size_t minRxOffset = sizeof(bfddp_message_header) + offsetof(bfddp_session, min_rx);
    buf[minRxOffset] = 0x00;
    buf[minRxOffset + 1] = 0x05;
    buf[minRxOffset + 2] = 0x93;
    buf[minRxOffset + 3] = 0xe0;
    memcpy(m_bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    m_bfd.handleBfdDpMessage(0);

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1u);
}

TEST_F(BfdSyncdTest, Ipv4AddSession)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("00:22:33:44:55:66"));

    unsigned char buf[BFD_WIRE_MSG_LEN];
    buildIpv4SessionWire(buf, sizeof(buf), DP_ADD_SESSION,
                         "10.0.0.1", "10.0.0.2", 5, "Ethernet0",
                         300000, 300000, 3);
    memcpy(m_bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    m_bfd.handleBfdDpMessage(0);

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1u);
    ASSERT_EQ(keys[0], "default:default:10.0.0.2");
}

TEST_F(BfdSyncdTest, BfdStateUpdateFromStateDb)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    Table stateTable(&m_state_db, STATE_BFD_SESSION_TABLE_NAME);
    stateTable.set("default|default|2000::2", {{"state", "Up"}});

    EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(1);
    m_bfd.bfdStateUpdate("default|default|2000::2");
}

TEST_F(BfdSyncdTest, StateUpdateMalformedKeyFormat)
{
    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);

    std::vector<FieldValueTuple> fieldValues = {{"state", "Up"}};
    ASSERT_FALSE(m_bfd.handleBfdStateUpdate("default|Ethernet0", fieldValues));
}

TEST_F(BfdSyncdTest, StateUpdateRemoteMultiplierOverflow)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);

    std::vector<FieldValueTuple> fieldValues = {
        {"state", "Up"},
        {"remote_multiplier", "300"},
    };
    auto key = string("default|default|2000::2");
    ASSERT_FALSE(m_bfd.handleBfdStateUpdate(key, fieldValues));
}

TEST_F(BfdSyncdTest, UnsupportedMessageType)
{
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    buf[2] = 0;
    buf[3] = ECHO_REQUEST;

    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);
    memcpy(m_bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    m_bfd.handleBfdDpMessage(0);
}

TEST_F(BfdSyncdTest, Ipv4MultihopNoInterface)
{
    unsigned char buf[BFD_WIRE_MSG_LEN];
    buildIpv4SessionWire(buf, sizeof(buf), DP_ADD_SESSION,
                         "10.0.0.1", "10.0.0.2", 0, nullptr,
                         300000, 300000, 3);
    memcpy(m_bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    m_bfd.handleBfdDpMessage(0);

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1u);
    ASSERT_EQ(keys[0], "default:default:10.0.0.2");

    vector<FieldValueTuple> fieldValues;
    app_bfd_session_table.get(keys[0], fieldValues);
    bool hasDstMac = false;
    for (const auto &fv : fieldValues)
    {
        if (fvField(fv) == "dst_mac")
        {
            hasDstMac = true;
        }
    }
    ASSERT_FALSE(hasDstMac);
}

TEST_F(BfdSyncdTest, StateUpdateInvalidRemoteMinRx)
{
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);

    std::vector<FieldValueTuple> fieldValues = {
        {"state", "Up"},
        {"remote_min_rx", "bad-value"},
    };
    auto key = string("default|default|2000::2");
    ASSERT_FALSE(m_bfd.handleBfdStateUpdate(key, fieldValues));
}

/* Fixture for readData() tests: sets up a real connected loopback socket pair. */
class BfdSyncdReadDataTest : public BfdSyncdTest
{
public:
    void SetUp() override
    {
        BfdSyncdTest::SetUp();

        unsigned short port = m_bfd.getServerPort();
        ASSERT_NE(port, 0u);

        m_client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_GE(m_client_fd, 0);

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(connect(m_client_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);

        m_bfd.accept();
    }

    void TearDown() override
    {
        if (m_client_fd >= 0)
        {
            close(m_client_fd);
            m_client_fd = -1;
        }
        BfdSyncdTest::TearDown();
    }

    int m_client_fd = -1;
};

TEST_F(BfdSyncdReadDataTest, ReadDataSingleCompleteMessage)
{
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    ASSERT_EQ(write(m_client_fd, buf, BFD_WIRE_MSG_LEN), (ssize_t)BFD_WIRE_MSG_LEN);

    m_bfd.readData();

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 1u);
    ASSERT_EQ(keys[0], "default:default:2000::2");
}

TEST_F(BfdSyncdReadDataTest, ReadDataPartialHeaderBuffered)
{
    /* Write fewer bytes than a header — message must be buffered, not dispatched. */
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    ASSERT_EQ(write(m_client_fd, buf, BFD_MSG_HDR_LEN - 1), (ssize_t)(BFD_MSG_HDR_LEN - 1));

    m_bfd.readData();

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0u);
}

TEST_F(BfdSyncdReadDataTest, ReadDataPartialBodyBuffered)
{
    /* Write a complete header but no body — body must be buffered, not dispatched. */
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    ASSERT_EQ(write(m_client_fd, buf, BFD_MSG_HDR_LEN), (ssize_t)BFD_MSG_HDR_LEN);

    m_bfd.readData();

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0u);
}

TEST_F(BfdSyncdReadDataTest, ReadDataMultipleMessagesInOneRead)
{
    unsigned char combined[BFD_WIRE_MSG_LEN * 2];
    buildIpv6SessionWire(combined, BFD_WIRE_MSG_LEN,
                         DP_ADD_SESSION, "2001::1", "2001::2", 0, nullptr, 300000, 300000, 3);
    buildIpv6SessionWire(combined + BFD_WIRE_MSG_LEN, BFD_WIRE_MSG_LEN,
                         DP_ADD_SESSION, "2002::1", "2002::2", 0, nullptr, 300000, 300000, 3);
    ASSERT_EQ(write(m_client_fd, combined, sizeof(combined)), (ssize_t)sizeof(combined));

    m_bfd.readData();

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 2u);
}

TEST_F(BfdSyncdReadDataTest, ReadDataConnectionClosed)
{
    close(m_client_fd);
    m_client_fd = -1;

    ASSERT_THROW(m_bfd.readData(), BfdLink::BfdConnectionClosedException);
}

TEST_F(BfdSyncdReadDataTest, ReadDataInvalidHeaderDropped)
{
    /* ECHO_REQUEST fails bfd_msg_ok(); the buffer should be cleared. */
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    buf[2] = 0;
    buf[3] = ECHO_REQUEST;
    ASSERT_EQ(write(m_client_fd, buf, BFD_WIRE_MSG_LEN), (ssize_t)BFD_WIRE_MSG_LEN);

    m_bfd.readData();

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0u);
}

