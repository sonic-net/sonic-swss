#include "gtest/gtest.h"

#include <arpa/inet.h>
#include <cstring>
#include <linux/neighbour.h>
#include <memory>
#include <net/if.h>
#include <netlink/addr.h>
#include <netlink/route/neighbour.h>
#include <string>
#include <vector>

#include "../mock_table.h"
#include "neighsyncd/neighsync.h"
#include "redisutility.h"

using namespace swss;

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

RtnlNeighPtr createNeighbor(int family, const std::string& ip, int state)
{
    char buffer[512] = {};
    struct nlmsghdr *hdr = reinterpret_cast<struct nlmsghdr *>(buffer);
    hdr->nlmsg_type = RTM_NEWNEIGH;
    hdr->nlmsg_flags = NLM_F_REQUEST;
    hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));

    struct ndmsg *nd = static_cast<struct ndmsg *>(NLMSG_DATA(hdr));
    nd->ndm_family = static_cast<unsigned char>(family);
    nd->ndm_ifindex = static_cast<int>(if_nametoindex("lo"));
    nd->ndm_state = static_cast<unsigned short>(state);
    nd->ndm_type = RTN_UNICAST;

    struct rtattr *dst = reinterpret_cast<struct rtattr *>(
        buffer + NLMSG_ALIGN(hdr->nlmsg_len));
    size_t addressLength = family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr);
    dst->rta_type = NDA_DST;
    dst->rta_len = static_cast<unsigned short>(RTA_LENGTH(addressLength));
    if (inet_pton(family, ip.c_str(), RTA_DATA(dst)) != 1)
    {
        return nullptr;
    }
    hdr->nlmsg_len = static_cast<unsigned int>(
        NLMSG_ALIGN(hdr->nlmsg_len) + RTA_ALIGN(dst->rta_len));

    const unsigned char mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    struct rtattr *lladdr = reinterpret_cast<struct rtattr *>(
        buffer + NLMSG_ALIGN(hdr->nlmsg_len));
    lladdr->rta_type = NDA_LLADDR;
    lladdr->rta_len = static_cast<unsigned short>(RTA_LENGTH(sizeof(mac)));
    memcpy(RTA_DATA(lladdr), mac, sizeof(mac));
    hdr->nlmsg_len = static_cast<unsigned int>(
        NLMSG_ALIGN(hdr->nlmsg_len) + RTA_ALIGN(lladdr->rta_len));

    struct rtnl_neigh *neigh = nullptr;
    if (rtnl_neigh_parse(hdr, &neigh) < 0)
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

} // namespace
