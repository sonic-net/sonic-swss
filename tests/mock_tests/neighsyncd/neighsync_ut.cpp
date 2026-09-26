#include "gtest/gtest.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <linux/netlink.h>
#include <linux/neighbour.h>
#include <memory>
#include <net/if.h>
#include <netlink/addr.h>
#include <netlink/attr.h>
#include <netlink/errno.h>
#include <netlink/handlers.h>
#include <netlink/msg.h>
#include <netlink/route/neighbour.h>
#include <poll.h>
#include <string>
#include <vector>

#include "../mock_table.h"
#include "neighsyncd/neighsync.h"
#include "redisutility.h"

using namespace swss;

namespace
{

enum class DumpResponse
{
    Done,
    Interrupted,
    KernelError,
};

struct CapturedDumpRequest
{
    int type = 0;
    int flags = 0;
    int family = 0;
    int headerIfindex = 0;
    bool hasIfindexAttribute = false;
    uint32_t ifindexAttribute = 0;
};

bool mockInterfaceLookup = false;
unsigned int mockIfindex = 42;
int mockSendResult = 1;
int mockPollResult = 1;
int mockReceiveResult = 0;
uint32_t mockSequence = 1234;
DumpResponse mockDumpResponse = DumpResponse::Done;
CapturedDumpRequest capturedDumpRequest;
int dumpRequestCount = 0;
nl_recvmsg_msg_cb_t validCallback = nullptr;
void *validCallbackArg = nullptr;
nl_recvmsg_msg_cb_t finishCallback = nullptr;
void *finishCallbackArg = nullptr;
nl_recvmsg_err_cb_t errorCallback = nullptr;
void *errorCallbackArg = nullptr;

void resetDumpMocks()
{
    mockInterfaceLookup = false;
    mockIfindex = 42;
    mockSendResult = 1;
    mockPollResult = 1;
    mockReceiveResult = 0;
    mockSequence = 1234;
    mockDumpResponse = DumpResponse::Done;
    capturedDumpRequest = {};
    dumpRequestCount = 0;
    validCallback = nullptr;
    validCallbackArg = nullptr;
    finishCallback = nullptr;
    finishCallbackArg = nullptr;
    errorCallback = nullptr;
    errorCallbackArg = nullptr;
}

}

extern "C"
{

unsigned int __real_if_nametoindex(const char *ifname);
int __real_nl_cb_set(
    struct nl_cb *cb,
    enum nl_cb_type type,
    enum nl_cb_kind kind,
    nl_recvmsg_msg_cb_t func,
    void *arg);
int __real_nl_cb_err(
    struct nl_cb *cb,
    enum nl_cb_kind kind,
    nl_recvmsg_err_cb_t func,
    void *arg);

unsigned int __wrap_if_nametoindex(const char *ifname)
{
    return mockInterfaceLookup ? mockIfindex : __real_if_nametoindex(ifname);
}

int __wrap_nl_send_auto(struct nl_sock *, struct nl_msg *message)
{
    ++dumpRequestCount;
    auto *header = nlmsg_hdr(message);
    auto *neighbor = static_cast<struct ndmsg *>(NLMSG_DATA(header));
    auto *ifindex = nlmsg_find_attr(header, sizeof(*neighbor), NDA_IFINDEX);
    capturedDumpRequest.type = header->nlmsg_type;
    capturedDumpRequest.flags = header->nlmsg_flags;
    capturedDumpRequest.family = neighbor->ndm_family;
    capturedDumpRequest.headerIfindex = neighbor->ndm_ifindex;
    capturedDumpRequest.hasIfindexAttribute = ifindex != nullptr;
    if (ifindex)
    {
        capturedDumpRequest.ifindexAttribute = nla_get_u32(ifindex);
    }
    header->nlmsg_seq = mockSequence;
    return mockSendResult;
}

int __wrap_nl_cb_set(
    struct nl_cb *cb,
    enum nl_cb_type type,
    enum nl_cb_kind kind,
    nl_recvmsg_msg_cb_t func,
    void *arg)
{
    if (type == NL_CB_VALID)
    {
        validCallback = func;
        validCallbackArg = arg;
    }
    else if (type == NL_CB_FINISH)
    {
        finishCallback = func;
        finishCallbackArg = arg;
    }
    return __real_nl_cb_set(cb, type, kind, func, arg);
}

int __wrap_nl_cb_err(
    struct nl_cb *cb,
    enum nl_cb_kind kind,
    nl_recvmsg_err_cb_t func,
    void *arg)
{
    errorCallback = func;
    errorCallbackArg = arg;
    return __real_nl_cb_err(cb, kind, func, arg);
}

int __wrap_poll(struct pollfd *, nfds_t, int)
{
    return mockPollResult;
}

int __wrap_nl_recvmsgs(struct nl_sock *, struct nl_cb *)
{
    if (mockReceiveResult < 0)
    {
        return mockReceiveResult;
    }

    if (mockDumpResponse == DumpResponse::KernelError)
    {
        struct sockaddr_nl address = {};
        struct nlmsgerr error = {};
        error.error = -EINVAL;
        return errorCallback(&address, &error, errorCallbackArg);
    }

    std::unique_ptr<struct nl_msg, decltype(&nlmsg_free)> message(nlmsg_alloc(), nlmsg_free);
    int flags = NLM_F_MULTI;
    if (mockDumpResponse == DumpResponse::Interrupted)
    {
        flags |= NLM_F_DUMP_INTR;
    }
    auto *header = nlmsg_put(
        message.get(), NL_AUTO_PORT, mockSequence, NLMSG_DONE, 0, flags);
    if (!header)
    {
        return -NLE_NOMEM;
    }
    return finishCallback(message.get(), finishCallbackArg);
}

}

namespace
{

struct RtnlNeighDeleter
{
    void operator()(struct rtnl_neigh *neigh) const
    {
        rtnl_neigh_put(neigh);
    }
};

using RtnlNeighPtr = std::unique_ptr<struct rtnl_neigh, RtnlNeighDeleter>;

struct NlMsgDeleter
{
    void operator()(struct nl_msg *msg) const
    {
        nlmsg_free(msg);
    }
};

using NlMsgPtr = std::unique_ptr<struct nl_msg, NlMsgDeleter>;

RtnlNeighPtr createNeighbor(int family, const std::string& ip, int state)
{
    NlMsgPtr msg(nlmsg_alloc());
    if (!msg)
    {
        return nullptr;
    }

    struct nlmsghdr *hdr = nlmsg_put(
        msg.get(), NL_AUTO_PORT, NL_AUTO_SEQ, RTM_NEWNEIGH, sizeof(struct ndmsg), NLM_F_REQUEST);
    if (!hdr)
    {
        return nullptr;
    }

    struct ndmsg *nd = static_cast<struct ndmsg *>(NLMSG_DATA(hdr));
    memset(nd, 0, sizeof(*nd));
    nd->ndm_family = static_cast<unsigned char>(family);
    nd->ndm_ifindex = static_cast<int>(if_nametoindex("lo"));
    nd->ndm_state = static_cast<unsigned short>(state);
    nd->ndm_type = RTN_UNICAST;

    size_t addressLength = family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr);
    struct rtattr *dst = static_cast<struct rtattr *>(
        nlmsg_reserve(msg.get(), RTA_LENGTH(addressLength), NLMSG_ALIGNTO));
    if (!dst)
    {
        return nullptr;
    }

    dst->rta_type = NDA_DST;
    dst->rta_len = static_cast<unsigned short>(RTA_LENGTH(addressLength));
    if (inet_pton(family, ip.c_str(), RTA_DATA(dst)) != 1)
    {
        return nullptr;
    }

    const unsigned char mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    struct rtattr *lladdr = static_cast<struct rtattr *>(
        nlmsg_reserve(msg.get(), RTA_LENGTH(sizeof(mac)), NLMSG_ALIGNTO));
    if (!lladdr)
    {
        return nullptr;
    }

    lladdr->rta_type = NDA_LLADDR;
    lladdr->rta_len = static_cast<unsigned short>(RTA_LENGTH(sizeof(mac)));
    memcpy(RTA_DATA(lladdr), mac, sizeof(mac));

    struct rtnl_neigh *neigh = nullptr;
    if (rtnl_neigh_parse(nlmsg_hdr(msg.get()), &neigh) < 0)
    {
        return nullptr;
    }

    return RtnlNeighPtr(neigh);
}

class NeighSyncTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        resetDumpMocks();
        testing_db::reset();
        m_appDb = std::make_shared<DBConnector>("APPL_DB", 0);
        m_stateDb = std::make_shared<DBConnector>("STATE_DB", 0);
        m_configDb = std::make_shared<DBConnector>("CONFIG_DB", 0);
        m_pipeline = std::make_shared<RedisPipeline>(m_appDb.get());
        m_sync = std::make_unique<NeighSync>(
            m_pipeline.get(), m_stateDb.get(), m_configDb.get(), m_appDb.get());
    }

    void enableDualTor()
    {
        Table peerSwitchTable(m_configDb.get(), CFG_PEER_SWITCH_TABLE_NAME);
        peerSwitchTable.set("peer_switch_hostname", {{"address_ipv4", "10.0.0.1"}});
    }

    bool failedNeighborExists(const std::string& ip, std::vector<FieldValueTuple>* fields = nullptr)
    {
        Table failedNeighborTable(m_appDb.get(), APP_NEIGH_FAILED_TABLE_NAME);
        std::vector<std::string> keys;
        failedNeighborTable.getKeys(keys);
        const std::string suffix = ":" + ip;

        for (const auto& key : keys)
        {
            std::vector<FieldValueTuple> values;
            if (key.size() >= suffix.size() &&
                key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                failedNeighborTable.get(key, values))
            {
                if (fields)
                {
                    *fields = values;
                }
                return true;
            }
        }

        return false;
    }

    std::shared_ptr<DBConnector> m_appDb;
    std::shared_ptr<DBConnector> m_stateDb;
    std::shared_ptr<DBConnector> m_configDb;
    std::shared_ptr<RedisPipeline> m_pipeline;
    std::unique_ptr<NeighSync> m_sync;
};

TEST_F(NeighSyncTest, PublishesDualTorFailedIpv6Neighbor)
{
    enableDualTor();
    auto neigh = createNeighbor(AF_INET6, "2001:db8::1", NUD_FAILED);
    ASSERT_TRUE(neigh.get() != nullptr);
    EXPECT_EQ(rtnl_neigh_get_family(neigh.get()), AF_INET6);
    EXPECT_EQ(rtnl_neigh_get_state(neigh.get()), NUD_FAILED);

    Table peerSwitchTable(m_configDb.get(), CFG_PEER_SWITCH_TABLE_NAME);
    std::vector<std::string> peerSwitchKeys;
    peerSwitchTable.getKeys(peerSwitchKeys);
    ASSERT_EQ(peerSwitchKeys.size(), 1u);

    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(neigh.get()));

    std::vector<FieldValueTuple> fields;
    ASSERT_TRUE(failedNeighborExists("2001:db8::1", &fields));
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fvField(fields[0]), "NULL");
    EXPECT_EQ(fvValue(fields[0]), "NULL");
}

TEST_F(NeighSyncTest, FiltersUnsupportedFailedNeighborEvents)
{
    enableDualTor();

    auto ipv4 = createNeighbor(AF_INET, "192.0.2.1", NUD_FAILED);
    ASSERT_TRUE(ipv4.get() != nullptr);
    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(ipv4.get()));

    auto incomplete = createNeighbor(AF_INET6, "2001:db8::2", NUD_INCOMPLETE);
    ASSERT_TRUE(incomplete.get() != nullptr);
    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(incomplete.get()));

    EXPECT_FALSE(failedNeighborExists("192.0.2.1"));
    EXPECT_FALSE(failedNeighborExists("2001:db8::2"));
}

TEST_F(NeighSyncTest, KeepsFailedNeighborEntryWhileIncomplete)
{
    enableDualTor();
    auto failed = createNeighbor(AF_INET6, "2001:db8::3", NUD_FAILED);
    ASSERT_TRUE(failed.get() != nullptr);
    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(failed.get()));
    ASSERT_TRUE(failedNeighborExists("2001:db8::3"));

    auto incomplete = createNeighbor(AF_INET6, "2001:db8::3", NUD_INCOMPLETE);
    ASSERT_TRUE(incomplete.get() != nullptr);
    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(incomplete.get()));

    EXPECT_TRUE(failedNeighborExists("2001:db8::3"));
}

TEST_F(NeighSyncTest, RemovesFailedNeighborEntryWhenResolved)
{
    enableDualTor();
    auto failed = createNeighbor(AF_INET6, "2001:db8::4", NUD_FAILED);
    ASSERT_TRUE(failed.get() != nullptr);
    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(failed.get()));
    ASSERT_TRUE(failedNeighborExists("2001:db8::4"));

    auto reachable = createNeighbor(AF_INET6, "2001:db8::4", NUD_REACHABLE);
    ASSERT_TRUE(reachable.get() != nullptr);
    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(reachable.get()));

    EXPECT_FALSE(failedNeighborExists("2001:db8::4"));
}

TEST_F(NeighSyncTest, RemovesFailedNeighborEntryWhenDeleted)
{
    enableDualTor();
    auto failed = createNeighbor(AF_INET6, "2001:db8::5", NUD_FAILED);
    ASSERT_TRUE(failed.get() != nullptr);
    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(failed.get()));
    ASSERT_TRUE(failedNeighborExists("2001:db8::5"));

    auto deleted = createNeighbor(AF_INET6, "2001:db8::5", NUD_FAILED);
    ASSERT_TRUE(deleted.get() != nullptr);
    m_sync->onMsg(RTM_DELNEIGH, reinterpret_cast<struct nl_object *>(deleted.get()));

    EXPECT_FALSE(failedNeighborExists("2001:db8::5"));
}

TEST_F(NeighSyncTest, DoesNotPublishFailedIpv6NeighborWithoutDualTor)
{
    auto neigh = createNeighbor(AF_INET6, "2001:db8::6", NUD_FAILED);
    ASSERT_TRUE(neigh.get() != nullptr);

    m_sync->onMsg(RTM_NEWNEIGH, reinterpret_cast<struct nl_object *>(neigh.get()));

    EXPECT_FALSE(failedNeighborExists("2001:db8::6"));
}

TEST_F(NeighSyncTest, SendsInterfaceFilteredIpv6NeighborDump)
{
    mockInterfaceLookup = true;

    EXPECT_TRUE(m_sync->resyncLinkLocalNeighbors("Ethernet0"));

    EXPECT_EQ(dumpRequestCount, 1);
    EXPECT_EQ(capturedDumpRequest.type, RTM_GETNEIGH);
    EXPECT_EQ(capturedDumpRequest.flags & (NLM_F_REQUEST | NLM_F_DUMP),
              NLM_F_REQUEST | NLM_F_DUMP);
    EXPECT_EQ(capturedDumpRequest.family, AF_INET6);
    EXPECT_EQ(capturedDumpRequest.headerIfindex, 0);
    EXPECT_TRUE(capturedDumpRequest.hasIfindexAttribute);
    EXPECT_EQ(capturedDumpRequest.ifindexAttribute, mockIfindex);
}

TEST_F(NeighSyncTest, DoesNotSendDumpForUnknownInterface)
{
    mockInterfaceLookup = true;
    mockIfindex = 0;

    EXPECT_FALSE(m_sync->resyncLinkLocalNeighbors("Ethernet0"));
    EXPECT_EQ(dumpRequestCount, 0);
}

TEST_F(NeighSyncTest, FailsWhenDumpSendFails)
{
    mockInterfaceLookup = true;
    mockSendResult = -NLE_FAILURE;

    EXPECT_FALSE(m_sync->resyncLinkLocalNeighbors("Ethernet0"));
}

TEST_F(NeighSyncTest, FailsWhenDumpReceiveFails)
{
    mockInterfaceLookup = true;
    mockReceiveResult = -NLE_AGAIN;

    EXPECT_FALSE(m_sync->resyncLinkLocalNeighbors("Ethernet0"));
}

TEST_F(NeighSyncTest, FailsWhenDumpReceiveTimesOut)
{
    mockInterfaceLookup = true;
    mockPollResult = 0;

    EXPECT_FALSE(m_sync->resyncLinkLocalNeighbors("Ethernet0"));
}

TEST_F(NeighSyncTest, FailsWhenDumpIsInterrupted)
{
    mockInterfaceLookup = true;
    mockDumpResponse = DumpResponse::Interrupted;

    EXPECT_FALSE(m_sync->resyncLinkLocalNeighbors("Ethernet0"));
}

TEST_F(NeighSyncTest, FailsWhenKernelRejectsDump)
{
    mockInterfaceLookup = true;
    mockDumpResponse = DumpResponse::KernelError;

    EXPECT_FALSE(m_sync->resyncLinkLocalNeighbors("Ethernet0"));
}

} // namespace
