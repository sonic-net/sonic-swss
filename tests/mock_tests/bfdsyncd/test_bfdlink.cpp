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

static void copyDefaultIpv6AddBuffer(unsigned char *buf, size_t bufSize)
{
    static const unsigned char s[BFD_WIRE_MSG_LEN] = {
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

    ASSERT_GE(bufSize, BFD_WIRE_MSG_LEN);
    memcpy(buf, s, BFD_WIRE_MSG_LEN);
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

static void addDefaultIpv6Session(MockBfdLink &bfd)
{
    unsigned char buf[BFD_WIRE_MSG_LEN];
    copyDefaultIpv6AddBuffer(buf, sizeof(buf));
    memcpy(bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    bfd.handleBfdDpMessage(0);
}

class MockBfdLink : public BfdLink
{
public:
    MockBfdLink(DBConnector *db, DBConnector *stateDb, unsigned short port = BFD_TEST_PORT, int debug = 0):BfdLink(db, stateDb, port, debug){}
    MOCK_METHOD(bool, sendmsg, (uint16_t msglen), ());
    MOCK_METHOD(string, exec, (const char* cmd), (override));
    MOCK_METHOD(string, get_intf_mac, (const char* intf), (override));
};


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

    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("78:12:83:58:08:00"));
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));

    memcpy(m_bfd.m_messageBuffer, static_cast<void *>(s), BFD_WIRE_MSG_LEN);

    m_bfd.handleBfdDpMessage(0);

    //Check APPL DB
    {
        vector<string> keys;
        vector<FieldValueTuple> fieldValues;
        string multihop;
        string local_addr;
        string dst_mac;
        string src_mac;
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
            if (field == "dst_mac")     dst_mac = value;
            if (field == "src_mac")     src_mac = value;
            if (field == "rx_interval") rx_interval = value;
            if (field == "tx_interval") tx_interval = value;
            if (field == "multiplier" ) multiplier = value;
        }
        ASSERT_EQ( multihop, "false");
        ASSERT_EQ( local_addr, "fe80::7aa4:3eff:fe72:ac00");
        ASSERT_EQ( dst_mac, "78:12:83:58:08:01");
        ASSERT_EQ( src_mac, "78:12:83:58:08:00");
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
        auto key = string("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400");
        m_bfd.handleBfdStateUpdate(key, fieldValues);

    }

    //handle counter request
    {
        EXPECT_CALL(m_bfd, sendmsg(COUNTER_MSG_LEN)).Times(1);

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

        memcpy(m_bfd.m_messageBuffer, static_cast<void *>(s), sizeof(s));

        m_bfd.handleBfdDpMessage(0);
    }

    //Delete BFD session and Check APPL DB
    {
        unsigned char s[] = {
            0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x10, 0xfe, 0x80, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x7a, 0xa4, 0x3e, 0xff, 0xfe, 0x72, 0xac, 0x00, 0xfe, 0x80, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x7a, 0x11, 0x08, 0xff, 0xfe, 0x55, 0xd4, 0x00, 0x24, 0x08, 0xc7, 0x9e,
            0x00, 0x04, 0x93, 0xe0, 0x00, 0x04, 0x93, 0xe0, 0x00, 0x00, 0xc3, 0x50, 0x00, 0x00, 0xc3, 0x50,
            0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x45, 0x74, 0x68, 0x65,
            0x72, 0x6e, 0x65, 0x74, 0x31, 0x5f, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        memcpy(m_bfd.m_messageBuffer, static_cast<void *>(s), sizeof(s));

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
        auto key = string("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400");
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
    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("78:12:83:58:08:00"));
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(1);

    std::vector<FieldValueTuple> fieldValues = {
        {"state", "Down"},
        {"remote_discriminator", "42"},
        {"remote_min_rx", "100000"},
        {"remote_min_tx", "200000"},
        {"remote_multiplier", "3"},
    };
    auto key = string("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400");
    ASSERT_TRUE(m_bfd.handleBfdStateUpdate(key, fieldValues));
}

TEST_F(BfdSyncdTest, StateUpdateInvalidRemoteField)
{
    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("78:12:83:58:08:00"));
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    EXPECT_CALL(m_bfd, sendmsg(_)).Times(0);

    std::vector<FieldValueTuple> fieldValues = {
        {"state", "Up"},
        {"remote_discriminator", "not-a-number"},
    };
    auto key = string("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400");
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
    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("78:12:83:58:08:00"));
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));

    addDefaultIpv6Session(m_bfd);

    Table stateTable(&m_state_db, STATE_BFD_SESSION_TABLE_NAME);
    stateTable.set("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400", {{"state", "Up"}});

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
    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("78:12:83:58:08:00"));
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
    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("00:11:22:33:44:55"));
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
    ASSERT_EQ(keys[0], "default:Ethernet0:10.0.0.2");
}

TEST_F(BfdSyncdTest, Ipv4LinkLocalRejected)
{
    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("00:11:22:33:44:55"));
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("00:22:33:44:55:66"));

    unsigned char buf[BFD_WIRE_MSG_LEN];
    buildIpv4SessionWire(buf, sizeof(buf), DP_ADD_SESSION,
                         "10.0.0.1", "169.254.1.1", 5, "Ethernet0",
                         300000, 300000, 3);
    memcpy(m_bfd.m_messageBuffer, buf, BFD_WIRE_MSG_LEN);
    m_bfd.handleBfdDpMessage(0);

    shared_ptr<swss::DBConnector> app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    Table app_bfd_session_table(app_db.get(), APP_BFD_SESSION_TABLE_NAME);
    vector<string> keys;
    app_bfd_session_table.getKeys(keys);
    ASSERT_EQ(keys.size(), 0u);
}

TEST_F(BfdSyncdTest, BfdStateUpdateFromStateDb)
{
    ON_CALL(m_bfd, get_intf_mac(_)).WillByDefault(Return("78:12:83:58:08:00"));
    ON_CALL(m_bfd, exec(_)).WillByDefault(Return("78:12:83:58:08:01"));
    addDefaultIpv6Session(m_bfd);

    Table stateTable(&m_state_db, STATE_BFD_SESSION_TABLE_NAME);
    stateTable.set("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400", {{"state", "Up"}});

    EXPECT_CALL(m_bfd, sendmsg(STATE_UPDATE_MSG_LEN)).Times(1);
    m_bfd.bfdStateUpdate("default|Ethernet1_1|fe80::7a11:8ff:fe55:d400");
}

