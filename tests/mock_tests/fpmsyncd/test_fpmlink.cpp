#include "fpmsyncd/fpmlink.h"

#include <swss/netdispatcher.h>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace swss;

using ::testing::_;

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


static volatile sig_atomic_t gSigpipeRaised = 0;

static void onSigpipe(int)
{
    gSigpipeRaised = 1;
}

/*
 * A zebra that disconnects leaves fpmsyncd writing to a closed connection. That has to
 * be a failed send, not a SIGPIPE that stops the daemon.
 */
TEST_F(FpmLinkTest, SendToClosedPeerFailsWithoutSigpipe)
{
    struct sigaction catcher{}, saved{};
    catcher.sa_handler = onSigpipe;
    ASSERT_EQ(sigaction(SIGPIPE, &catcher, &saved), 0);
    gSigpipeRaised = 0;

    int client = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FPM_DEFAULT_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connect(client, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)), 0);
    m_fpm.accept();
    close(client);

    struct nlmsghdr nl_hdr{};
    nl_hdr.nlmsg_len = NLMSG_LENGTH(0);

    /* The first send can still be queued, and the first one after the peer's reset fails
     * with ECONNRESET; the sends after that are the ones that meet EPIPE. */
    int failures = 0;
    for (int i = 0; i < 100 && failures < 3; i++)
    {
        if (!m_fpm.send(&nl_hdr))
        {
            failures++;
        }
        else
        {
            usleep(10000);
        }
    }

    sigaction(SIGPIPE, &saved, nullptr);
    EXPECT_EQ(failures, 3);
    EXPECT_EQ(gSigpipeRaised, 0);
}
