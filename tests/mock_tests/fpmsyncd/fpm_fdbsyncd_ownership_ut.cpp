// Exercise both actual producers against the same mock APPL_DB. This is a
// decoder/ownership regression, not a privileged kernel or full DVS execution.
#include "redisutility.h"
#include "mock_table.h"
#include <gtest/gtest.h>
#include <linux/nexthop.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <array>
#include <map>
#include <memory>
#include <cstdio>
#include <cstring>
#include "fdbsyncd/fdbsync.h"
#include "fpmsyncd/routesync.h"
#include "fpmsyncd/fpm/fpm.h"
#include "lib/fpm_protection_ids.h"

using namespace swss;

namespace {
class OwnershipRouteSync : public RouteSync
{
public:
    using RouteSync::RouteSync;
    bool getIfName(int index, char *name, size_t size) override
    {
        if (index != 40) return false;
        snprintf(name, size, "%s", "PortChannel0");
        return true;
    }
};

struct KernelFrame
{
    alignas(nlmsghdr) std::array<unsigned char, 256> bytes{};
    explicit KernelFrame(unsigned short operation)
    {
        header()->nlmsg_type = operation;
        header()->nlmsg_len = NLMSG_LENGTH(sizeof(nhmsg));
        header()->nlmsg_flags = NLM_F_REQUEST;
    }
    nlmsghdr *header() { return reinterpret_cast<nlmsghdr *>(bytes.data()); }
    void attribute(unsigned short type, const void *value, size_t size)
    {
        auto *h = header();
        auto offset = NLMSG_ALIGN(h->nlmsg_len);
        auto *attr = reinterpret_cast<rtattr *>(bytes.data() + offset);
        attr->rta_type = type; attr->rta_len = static_cast<uint16_t>(RTA_LENGTH(size));
        if (size) memcpy(RTA_DATA(attr), value, size);
        h->nlmsg_len = offset + RTA_ALIGN(attr->rta_len);
    }
};

class FpmFdbOwnershipTest : public ::testing::Test
{
protected:
    DBConnector app{"APPL_DB", 0}, state{"STATE_DB", 0}, config{"CONFIG_DB", 0};
    RedisPipeline pipeline{&app};
    std::unique_ptr<FdbSync> kernel;
    std::unique_ptr<OwnershipRouteSync> fpm;
    Table objects{&app, APP_L2_NEXTHOP_GROUP_TABLE_NAME};
    using Rows = std::map<std::string, std::vector<FieldValueTuple>>;
    Rows legacy;
    const uint32_t fpmMember = 0x40001001u, fpmGroup = 0x80001001u;
    void SetUp() override
    {
        testing_db::reset();
        kernel = std::make_unique<FdbSync>(&pipeline, &state, &config);
        kernel->m_isEvpnNvoExist = true;
        fpm = std::make_unique<OwnershipRouteSync>(&pipeline);
    }
    void TearDown() override
    {
        fpm.reset(); kernel.reset(); testing_db::reset();
    }
    void kernelMember(uint32_t id, const char *address)
    {
        KernelFrame frame{RTM_NEWNEXTHOP};
        auto *message = reinterpret_cast<nhmsg *>(NLMSG_DATA(frame.header()));
        message->nh_family = strchr(address, ':') ? AF_INET6 : AF_INET;
        unsigned char ip[16]{};
        ASSERT_EQ(inet_pton(message->nh_family, address, ip), 1);
        frame.attribute(NHA_ID, &id, sizeof(id));
        frame.attribute(NHA_GATEWAY, ip, message->nh_family == AF_INET ? 4 : 16);
        frame.attribute(NHA_FDB, nullptr, 0);
        kernel->onMsgRaw(frame.header()); pipeline.flush();
        std::vector<FieldValueTuple> row;
        ASSERT_TRUE(objects.get(std::to_string(id), row));
        ASSERT_EQ(row, (std::vector<FieldValueTuple>{{"remote_vtep", address}}));
        legacy[std::to_string(id)] = row;
    }
    void kernelGroup(uint32_t id, uint32_t first, uint32_t second)
    {
        KernelFrame frame{RTM_NEWNEXTHOP};
        nexthop_grp members[2]{}; members[0].id = first; members[1].id = second;
        frame.attribute(NHA_ID, &id, sizeof(id));
        frame.attribute(NHA_GROUP, members, sizeof(members));
        frame.attribute(NHA_FDB, nullptr, 0);
        kernel->onMsgRaw(frame.header()); pipeline.flush();
        std::vector<FieldValueTuple> row;
        ASSERT_TRUE(objects.get(std::to_string(id), row));
        ASSERT_EQ(row, (std::vector<FieldValueTuple>{{"nexthop_group", std::to_string(first)+","+std::to_string(second)}}));
        legacy[std::to_string(id)] = row;
    }
    void kernelDelete(uint32_t id)
    {
        KernelFrame frame{RTM_DELNEXTHOP}; frame.attribute(NHA_ID, &id, sizeof(id));
        kernel->onMsgRaw(frame.header()); pipeline.flush();
    }
    void seedLegacy()
    {
        kernelMember(100, "10.0.0.1"); kernelMember(101, "2001:db8::1");
        kernelGroup(200, 100, 101);
        // Numeric type tags are NOT proof of FPM ownership.
        kernelMember(0xfffffffeu, "10.0.0.2"); kernelMember(0xffffffffu, "2001:db8::2");
        kernelGroup(0x80000001u, 0xfffffffeu, 0xffffffffu);
    }
    void member(uint32_t id, bool deleting = false)
    {
        struct { nlmsghdr header; l2_nhg_member_msg body; } frame{};
        frame.header.nlmsg_type = deleting ? RTM_FPM_DEL_L2_NEXTHOP : RTM_FPM_ADD_L2_NEXTHOP;
        frame.header.nlmsg_len = NLMSG_LENGTH(sizeof(frame.body));
        frame.body.l2nm_nh_id = id; frame.body.l2nm_type = 1; frame.body.l2nm_family = AF_INET;
        inet_pton(AF_INET, "10.0.0.3", &frame.body.l2nm_remote_vtep);
        frame.body.l2nm_router_mac[0] = 2; frame.body.l2nm_router_mac[5] = 3;
        fpm->onMsgRaw(&frame.header); pipeline.flush();
    }
    void group(uint32_t id, bool deleting = false)
    {
        l2_nhg_group_msg body{}; body.l2ng_nhg_id = id; body.l2ng_count = 1;
        l2_nhg_member_msg member{}; member.l2nm_nh_id = fpmMember;
        member.l2nm_type = 1; member.l2nm_family = AF_INET;
        inet_pton(AF_INET, "10.0.0.3", &member.l2nm_remote_vtep);
        member.l2nm_router_mac[0] = 2; member.l2nm_router_mac[5] = 3;
        alignas(nlmsghdr) unsigned char bytes[NLMSG_SPACE(sizeof(body)+sizeof(member))]{};
        auto *header = reinterpret_cast<nlmsghdr *>(bytes);
        header->nlmsg_type = deleting ? RTM_FPM_DEL_L2_NEXTHOP_GROUP : RTM_FPM_ADD_L2_NEXTHOP_GROUP;
        header->nlmsg_len = NLMSG_LENGTH(sizeof(body)+sizeof(member));
        memcpy(NLMSG_DATA(header), &body, sizeof(body));
        memcpy(bytes + NLMSG_HDRLEN + sizeof(body), &member, sizeof(member));
        fpm->onMsgRaw(header); pipeline.flush();
    }
    void seedFpm()
    {
        member(fpmMember); group(fpmGroup);
        struct { nlmsghdr header; evpn_backup_nhg_msg body; } binding{};
        binding.header.nlmsg_type = RTM_FPM_ADD_EVPN_ES_BACKUP_NHG;
        binding.header.nlmsg_len = NLMSG_LENGTH(sizeof(binding.body));
        binding.body = {40, fpmGroup}; fpm->onMsgRaw(&binding.header); pipeline.flush();
        std::vector<FieldValueTuple> row;
        ASSERT_TRUE(objects.get(std::to_string(fpmMember), row));
        EXPECT_EQ(row, (std::vector<FieldValueTuple>{{"remote_vtep","10.0.0.3"},{"type","l3"},{"router_mac","02:00:00:00:00:03"}}));
        ASSERT_TRUE(objects.get(std::to_string(fpmGroup), row));
        EXPECT_EQ(row, (std::vector<FieldValueTuple>{{"nexthop_group",std::to_string(fpmMember)}}));
        Table ids{&app, FPM_PROTECTION_ID_TABLE};
        EXPECT_TRUE(hasFpmProtectionId(ids, std::to_string(fpmMember)));
        EXPECT_TRUE(hasFpmProtectionId(ids, std::to_string(fpmGroup)));
    }
    void preserved()
    {
        for (const auto &expected : legacy)
        {
            std::vector<FieldValueTuple> actual;
            ASSERT_TRUE(objects.get(expected.first, actual)) << expected.first;
            EXPECT_EQ(actual, expected.second) << expected.first;
        }
    }
    void flushed()
    {
        std::vector<FieldValueTuple> row;
        EXPECT_FALSE(objects.get(std::to_string(fpmMember), row));
        EXPECT_FALSE(objects.get(std::to_string(fpmGroup), row));
        Table bindings{&app, "EVPN_ES_BACKUP_NHG_TABLE"};
        EXPECT_FALSE(bindings.get("PortChannel0", row));
        preserved();
    }
};
}

TEST_F(FpmFdbOwnershipTest, ReconnectResetPreservesActualKernelMembersGroupsAndCache)
{
    seedLegacy(); seedFpm();
    fpm->onFpmDisconnected();
    preserved();
    testing_db::resetOperationCounters();
    fpm->resetFpmProtectionState(app); pipeline.flush(); flushed();
    EXPECT_EQ(testing_db::getProducerDelCount(APP_L2_NEXTHOP_GROUP_TABLE_NAME), 2u);
    fpm->resetFpmProtectionState(app); pipeline.flush(); flushed();
    EXPECT_EQ(testing_db::getProducerDelCount(APP_L2_NEXTHOP_GROUP_TABLE_NAME), 2u);
    // fdbsyncd's own cache and downstream member/group maintenance still work.
    kernelDelete(100);
    std::vector<FieldValueTuple> row;
    EXPECT_FALSE(objects.get("100", row));
    ASSERT_TRUE(objects.get("200", row));
    EXPECT_EQ(row, (std::vector<FieldValueTuple>{{"nexthop_group", "101"}}));
    kernelDelete(101); EXPECT_FALSE(objects.get("200", row));
    EXPECT_TRUE(objects.get("2147483649", row));
}

TEST_F(FpmFdbOwnershipTest, FirstAcceptAfterRestartPreservesLegacyAndRejectsFpmCollisions)
{
    seedLegacy(); seedFpm();
    // Recreate only FPM: cleanup must consult persisted provenance, not caches.
    fpm = std::make_unique<OwnershipRouteSync>(&pipeline);
    testing_db::resetOperationCounters();
    fpm->resetFpmProtectionState(app); pipeline.flush(); flushed();
    EXPECT_EQ(testing_db::getProducerDelCount(APP_L2_NEXTHOP_GROUP_TABLE_NAME), 2u);
    for (const auto &row : legacy)
    {
        auto id = static_cast<uint32_t>(std::stoul(row.first));
        member(id); member(id, true); group(id); group(id, true);
        preserved();
    }
    // No collision may register a legacy row, delete it or queue a later reset DEL.
    EXPECT_EQ(testing_db::getProducerDelCount(APP_L2_NEXTHOP_GROUP_TABLE_NAME), 2u);
    fpm->resetFpmProtectionState(app); pipeline.flush(); preserved();
    EXPECT_EQ(testing_db::getProducerDelCount(APP_L2_NEXTHOP_GROUP_TABLE_NAME), 2u);
    seedFpm(); fpm->resetFpmProtectionState(app); pipeline.flush(); flushed();
}
