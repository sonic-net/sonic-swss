#include "fpmsyncd/fpmlink.h"

#include <swss/netdispatcher.h>

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <fcntl.h>

using namespace swss;

using ::testing::_;

class FpmLinkLifecycleTest : public ::testing::Test
{
public:
    void TearDown() override
    {
        if (m_client >= 0)
            close(m_client);
    }

    int connectClient()
    {
        m_client = socket(AF_INET, SOCK_STREAM, 0);
        if (m_client < 0)
            return -1;

        // Bound peer reads so a missing close fails instead of hanging the test.
        timeval timeout{1, 0};
        if (setsockopt(m_client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
            return -1;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(FPM_DEFAULT_PORT);
        return connect(m_client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }

    DBConnector m_db{"APPL_DB", 0};
    RedisPipeline m_pipeline{&m_db, 1};
    RouteSync m_routeSync{&m_pipeline};
    int m_client = -1;
};

TEST_F(FpmLinkLifecycleTest, TeardownClosesAcceptedSocketAcrossReconnects)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        int accepted;
        {
            FpmLink fpm(&m_routeSync);
            ASSERT_EQ(connectClient(), 0);
            fpm.accept();
            accepted = fpm.getFd();
            ASSERT_GE(fcntl(accepted, F_GETFD), 0);
        }

        errno = 0;
        EXPECT_EQ(fcntl(accepted, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
        char byte;
        EXPECT_EQ(recv(m_client, &byte, sizeof(byte), 0), 0);
        EXPECT_EQ(close(m_client), 0);
        m_client = -1;
    }
}

TEST_F(FpmLinkLifecycleTest, PeerEofTeardownClosesAcceptedSocket)
{
    int accepted;
    {
        FpmLink fpm(&m_routeSync);
        ASSERT_EQ(connectClient(), 0);
        fpm.accept();
        accepted = fpm.getFd();
        ASSERT_EQ(shutdown(m_client, SHUT_WR), 0);
        EXPECT_THROW(fpm.readData(), FpmLink::FpmConnectionClosedException);
    }

    errno = 0;
    EXPECT_EQ(fcntl(accepted, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    char byte;
    EXPECT_EQ(recv(m_client, &byte, sizeof(byte), 0), 0);
}

class MockMsgHandler : public NetMsg
{
public:
    MOCK_METHOD2(onMsg, void(int, nl_object*));
};

class FpmLinkTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &m_mock);
        NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &m_mock);
    }

    void TearDown() override
    {
        NetDispatcher::getInstance().unregisterMessageHandler(RTM_NEWROUTE);
        NetDispatcher::getInstance().unregisterMessageHandler(RTM_DELROUTE);
    }

    DBConnector m_db{"APPL_DB", 0};
    RedisPipeline m_pipeline{&m_db, 1};
    RouteSync m_routeSync{&m_pipeline};
    FpmLink m_fpm{&m_routeSync};
    MockMsgHandler m_mock;
};

TEST_F(FpmLinkTest, SingleNlMessageInFpmMessage)
{
    // Single FPM message containing single RTM_NEWROUTE
    alignas(fpm_msg_hdr_t) unsigned char fpmMsgBuffer[] = {
        0x01, 0x01, 0x00, 0x40, 0x3C, 0x00, 0x00, 0x00, 0x18, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0xE0,
        0x12, 0x6F, 0xC4, 0x02, 0x18, 0x00, 0x00, 0xFE, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
        0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x06, 0x00, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x05,
        0x00, 0xAC, 0x1E, 0x38, 0xA6, 0x08, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00
    };

    EXPECT_CALL(m_mock, onMsg(_, _)).Times(1);

    m_fpm.processFpmMessage(reinterpret_cast<fpm_msg_hdr_t*>(static_cast<void*>(fpmMsgBuffer)));
}

TEST_F(FpmLinkTest, TwoNlMessagesInFpmMessage)
{
    // Single FPM message containing RTM_DELROUTE and RTM_NEWROUTE
    alignas(fpm_msg_hdr_t) unsigned char fpmMsgBuffer[] = {
        0x01, 0x01, 0x00, 0x6C, 0x2C, 0x00, 0x00, 0x00, 0x19, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x12,
        0x6F, 0xC4, 0x02, 0x18, 0x00, 0x00, 0xFE, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00,
        0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x06, 0x00, 0x14, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x18, 0x00,
        0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x12, 0x6F, 0xC4, 0x02, 0x18, 0x00, 0x00, 0xFE, 0x02, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x06, 0x00, 0x14, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x05, 0x00, 0xAC, 0x1E, 0x38, 0xA7, 0x08, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00
    };

    EXPECT_CALL(m_mock, onMsg(_, _)).Times(2);

    m_fpm.processFpmMessage(reinterpret_cast<fpm_msg_hdr_t*>(static_cast<void*>(fpmMsgBuffer)));
}

