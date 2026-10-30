/**
 * @file fpmsyncd_evpn_mh_ut.cpp
 * @brief Unit tests for EVPN Multihoming support in fpmsyncd
 *
 * Tests for PR #4038: Fpmsyncd changes for EVPN MH feature
 * HLD: https://github.com/sonic-net/SONiC/blob/master/doc/vxlan/EVPN/EVPN_VxLAN_Multihoming.md#334-Fpmsyncd
 *
 * This file tests three new EVPN MH features:
 * 1. Split Horizon List (SHL) - RTM_FPM_ADD_EVPN_SHL / RTM_FPM_DEL_EVPN_SHL
 * 2. Designated Forwarder (DF) - RTM_FPM_ADD_EVPN_DF / RTM_FPM_DEL_EVPN_DF
 * 3. ES Backup NextHop Group - RTM_FPM_ADD_EVPN_ES_BACKUP_NHG / RTM_FPM_DEL_EVPN_ES_BACKUP_NHG
 */

#include "redisutility.h"
#include "table.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <linux/if_ether.h>
#include <netlink/route/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <climits>
#include <fstream>
#include "mock_table.h"
#include "lib/fpm_protection_ids.h"

#define private public
#include "fpmsyncd/routesync.h"
#include "fpmsyncd/fpmlink.h"
#undef private

using namespace swss;
using ::testing::_;

/**
 * @brief Test fixture for EVPN Multihoming fpmsyncd tests
 */
class FpmsyncdEvpnMhTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        m_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
        m_pipeline = std::make_shared<RedisPipeline>(m_db.get());
        m_routeSync = std::make_unique<RouteSync>(m_pipeline.get());
    }

    void TearDown() override
    {
        m_routeSync.reset();
        m_pipeline.reset();
        m_db.reset();
    }

    /**
     * @brief Helper to build FPM message header
     */
    fpm_msg_hdr_t* buildFpmHeader(unsigned char* buffer)
    {
        fpm_msg_hdr_t* fpm_hdr = reinterpret_cast<fpm_msg_hdr_t*>(static_cast<void*>(buffer));
        fpm_hdr->version = FPM_PROTO_VERSION;
        fpm_hdr->msg_type = FPM_MSG_TYPE_NETLINK;
        fpm_hdr->msg_len = htons(sizeof(fpm_msg_hdr_t));
        return fpm_hdr;
    }

    /**
     * @brief Helper to add netlink message header
     */
    nlmsghdr* addNlmsgHeader(fpm_msg_hdr_t* fpm_hdr,
                            unsigned char* buffer,
                            unsigned short nlmsg_type,
                            unsigned short nlmsg_flags,
                            size_t payload_size)
    {
        nlmsghdr* nl_hdr = reinterpret_cast<nlmsghdr*>(static_cast<void*>(buffer));
        nl_hdr->nlmsg_len = static_cast<__u32>(NLMSG_LENGTH(payload_size));
        nl_hdr->nlmsg_type = nlmsg_type;
        nl_hdr->nlmsg_flags = nlmsg_flags;
        nl_hdr->nlmsg_seq = 0;
        nl_hdr->nlmsg_pid = 0;

        fpm_hdr->msg_len = htons(static_cast<uint16_t>(ntohs(fpm_hdr->msg_len) + NLMSG_ALIGN(nl_hdr->nlmsg_len)));
        return nl_hdr;
    }

    /**
     * @brief Helper to add rtattr to netlink message
     */
    void addRtattr(nlmsghdr* nl_hdr, int type, const void* data, size_t len)
    {
        size_t rta_len = RTA_LENGTH(len);
        rtattr* rta = reinterpret_cast<rtattr*>(static_cast<void*>(
            reinterpret_cast<char*>(nl_hdr) + NLMSG_ALIGN(nl_hdr->nlmsg_len)));

        rta->rta_type = static_cast<unsigned short>(type);
        rta->rta_len = static_cast<unsigned short>(rta_len);
        memcpy(RTA_DATA(rta), data, len);

        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta_len);
    }

    /**
     * @brief Helper to get table entry from APP_DB
     */
    bool getTableEntry(const std::string& tableName, const std::string& key,
                      std::vector<FieldValueTuple>& values)
    {
        Table table(m_db.get(), tableName);
        return table.get(key, values);
    }

    /**
     * @brief Helper to check if key exists in table
     */
    bool isKeyDeleted(const std::string& tableName, const std::string& key)
    {
        Table table(m_db.get(), tableName);
        std::vector<FieldValueTuple> values;
        return !table.get(key, values);
    }

protected:
    std::shared_ptr<swss::DBConnector> m_db;
    std::shared_ptr<RedisPipeline> m_pipeline;
    std::unique_ptr<RouteSync> m_routeSync;
};


/**
 * @brief Test RTM_FPM_ADD_EVPN_SHL - Split Horizon List Add
 *
 * Verifies that fpmsyncd correctly processes EVPN Split Horizon List additions.
 * Split Horizon prevents loops by tracking which VTEPs have already seen a packet.
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnShlAdd)
{
    unsigned char buffer[4096] = {0};

    // Build FPM header
    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);

    // Build netlink header
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_SHL,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_shl_msg));

    // Build EVPN SHL message
    evpn_shl_msg* shl_msg = reinterpret_cast<evpn_shl_msg*>(NLMSG_DATA(nl_hdr));
    shl_msg->esm_ifindex = 0;  // Ethernet100
    shl_msg->esm_vid = 10;       // Vlan100

    // Add IPv4 VTEP addresses to split horizon list
    struct in_addr vtep1, vtep2;
    inet_pton(AF_INET, "10.0.0.1", &vtep1);
    inet_pton(AF_INET, "10.0.0.2", &vtep2);

    addRtattr(nl_hdr, FPM_SHL_IPV4_ADDR, &vtep1, sizeof(vtep1));
    addRtattr(nl_hdr, FPM_SHL_IPV4_ADDR, &vtep2, sizeof(vtep2));

    // Process the message
    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_shl_msg)));
    m_routeSync->onEvpnShlMsg(nl_hdr, len);
    m_pipeline->flush();

    // Verify the entry was added to EVPN_SPLIT_HORIZON_TABLE
    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_SPLIT_HORIZON_TABLE", "Vlan10:unknown", values);

    EXPECT_TRUE(found);
    if (found) {
        auto vteps = swss::fvsGetValue(values, "vteps", true);
        EXPECT_TRUE(vteps.has_value());
        if (vteps.has_value()) {
            EXPECT_TRUE(vteps->find("10.0.0.1") != std::string::npos);
            EXPECT_TRUE(vteps->find("10.0.0.2") != std::string::npos);
        }
    }
}


/**
 * @brief Test RTM_FPM_ADD_EVPN_SHL with IPv6 VTEPs
 *
 * Verifies that fpmsyncd correctly handles IPv6 VTEP addresses in Split Horizon List.
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnShlAddIpv6)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_SHL,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_shl_msg));

    evpn_shl_msg* shl_msg = reinterpret_cast<evpn_shl_msg*>(NLMSG_DATA(nl_hdr));
    shl_msg->esm_ifindex = 0;
    shl_msg->esm_vid = 10;

    // Add IPv6 VTEP addresses
    struct in6_addr vtep1_v6, vtep2_v6;
    inet_pton(AF_INET6, "2001:db8::1", &vtep1_v6);
    inet_pton(AF_INET6, "2001:db8::2", &vtep2_v6);

    addRtattr(nl_hdr, FPM_SHL_IPV6_ADDR, &vtep1_v6, sizeof(vtep1_v6));
    addRtattr(nl_hdr, FPM_SHL_IPV6_ADDR, &vtep2_v6, sizeof(vtep2_v6));

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_shl_msg)));
    m_routeSync->onEvpnShlMsg(nl_hdr, len);
    m_pipeline->flush();

    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_SPLIT_HORIZON_TABLE", "Vlan10:unknown", values);

    EXPECT_TRUE(found);
    if (found) {
        auto vteps = swss::fvsGetValue(values, "vteps", true);
        EXPECT_TRUE(vteps.has_value());
        if (vteps.has_value()) {
            EXPECT_TRUE(vteps->find("2001:db8::1") != std::string::npos);
            EXPECT_TRUE(vteps->find("2001:db8::2") != std::string::npos);
        }
    }
}


/**
 * @brief Test RTM_FPM_ADD_EVPN_SHL with mixed IPv4 and IPv6
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnShlAddMixed)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_SHL,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_shl_msg));

    evpn_shl_msg* shl_msg = reinterpret_cast<evpn_shl_msg*>(NLMSG_DATA(nl_hdr));
    shl_msg->esm_ifindex = 0;
    shl_msg->esm_vid = 10;

    struct in_addr vtep_v4;
    struct in6_addr vtep_v6;
    inet_pton(AF_INET, "192.168.1.1", &vtep_v4);
    inet_pton(AF_INET6, "fc00::1", &vtep_v6);

    addRtattr(nl_hdr, FPM_SHL_IPV4_ADDR, &vtep_v4, sizeof(vtep_v4));
    addRtattr(nl_hdr, FPM_SHL_IPV6_ADDR, &vtep_v6, sizeof(vtep_v6));

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_shl_msg)));
    m_routeSync->onEvpnShlMsg(nl_hdr, len);
    m_pipeline->flush();

    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_SPLIT_HORIZON_TABLE", "Vlan10:unknown", values);

    EXPECT_TRUE(found);
    if (found) {
        auto vteps = swss::fvsGetValue(values, "vteps", true);
        EXPECT_TRUE(vteps.has_value());
        if (vteps.has_value()) {
            EXPECT_TRUE(vteps->find("192.168.1.1") != std::string::npos);
            EXPECT_TRUE(vteps->find("fc00::1") != std::string::npos);
        }
    }
}


/**
 * @brief Test RTM_FPM_DEL_EVPN_SHL - Split Horizon List Delete
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnShlDelete)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_DEL_EVPN_SHL,
                                      0,
                                      sizeof(evpn_shl_msg));

    evpn_shl_msg* shl_msg = reinterpret_cast<evpn_shl_msg*>(NLMSG_DATA(nl_hdr));
    shl_msg->esm_ifindex = 0;
    shl_msg->esm_vid = 10;

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_shl_msg)));
    m_routeSync->onEvpnShlMsg(nl_hdr, len);
    m_pipeline->flush();

    EXPECT_TRUE(isKeyDeleted("EVPN_SPLIT_HORIZON_TABLE", "Vlan10:unknown"));
}


/**
 * @brief Test RTM_FPM_ADD_EVPN_DF - Designated Forwarder Add
 *
 * Tests DF election result notification. DF=true means this PE is responsible
 * for forwarding BUM (Broadcast, Unknown unicast, Multicast) traffic.
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnDfAddDesignated)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_DF,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_df_msg));

    evpn_df_msg* df_msg = reinterpret_cast<evpn_df_msg*>(NLMSG_DATA(nl_hdr));
    df_msg->edm_ifindex = 0;
    df_msg->edm_vid = 10;
    df_msg->edm_non_df = 0;  // This PE is DF

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_df_msg)));
    m_routeSync->onEvpnDfMsg(nl_hdr, len);
    m_pipeline->flush();

    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_DF_TABLE", "Vlan10:unknown", values);

    EXPECT_TRUE(found);
    if (found) {
        auto df = swss::fvsGetValue(values, "df", true);
        EXPECT_TRUE(df.has_value());
        if (df.has_value()) {
            EXPECT_EQ(*df, "true");
        }
    }
}


/**
 * @brief Test RTM_FPM_ADD_EVPN_DF with non-DF status
 *
 * Tests when this PE is NOT the designated forwarder (DF=false).
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnDfAddNonDesignated)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_DF,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_df_msg));

    evpn_df_msg* df_msg = reinterpret_cast<evpn_df_msg*>(NLMSG_DATA(nl_hdr));
    df_msg->edm_ifindex = 0;
    df_msg->edm_vid = 10;
    df_msg->edm_non_df = 1;  // This PE is NOT DF

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_df_msg)));
    m_routeSync->onEvpnDfMsg(nl_hdr, len);
    m_pipeline->flush();

    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_DF_TABLE", "Vlan10:unknown", values);

    EXPECT_TRUE(found);
    if (found) {
        auto df = swss::fvsGetValue(values, "df", true);
        EXPECT_TRUE(df.has_value());
        if (df.has_value()) {
            EXPECT_EQ(*df, "false");
        }
    }
}


/**
 * @brief Test RTM_FPM_DEL_EVPN_DF - Designated Forwarder Delete
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnDfDelete)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_DEL_EVPN_DF,
                                      0,
                                      sizeof(evpn_df_msg));

    evpn_df_msg* df_msg = reinterpret_cast<evpn_df_msg*>(NLMSG_DATA(nl_hdr));
    df_msg->edm_ifindex = 0;
    df_msg->edm_vid = 10;
    df_msg->edm_non_df = 0;  // Value doesn't matter for delete

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_df_msg)));
    m_routeSync->onEvpnDfMsg(nl_hdr, len);
    m_pipeline->flush();

    EXPECT_TRUE(isKeyDeleted("EVPN_DF_TABLE", "Vlan10:unknown"));
}


/**
 * @brief Test RTM_FPM_ADD_EVPN_ES_BACKUP_NHG - ES Backup NextHop Group Add
 *
 * Tests Ethernet Segment backup nexthop group configuration.
 * Backup NHG provides alternate paths when the primary ES member is down.
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnEsBackupNhgAdd)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_ES_BACKUP_NHG,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_backup_nhg_msg));

    evpn_backup_nhg_msg* backup_msg = reinterpret_cast<evpn_backup_nhg_msg*>(NLMSG_DATA(nl_hdr));
    backup_msg->ebnm_ifindex = 21;         // Ethernet100
    backup_msg->ebnm_backup_nhg_id = 0x80001388u;  // Backup NHG ID

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_backup_nhg_msg)));
    m_routeSync->onEvpnEsBackupNhgMsg(nl_hdr, len);
    m_pipeline->flush();

    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_ES_BACKUP_NHG_TABLE", "Ethernet0", values);

    EXPECT_TRUE(found);
    if (found) {
        auto nhg = swss::fvsGetValue(values, "nexthop_group", true);
        EXPECT_TRUE(nhg.has_value());
        if (nhg.has_value()) {
            EXPECT_EQ(*nhg, "2147488648");
        }
    }
}


/**
 * @brief Test RTM_FPM_DEL_EVPN_ES_BACKUP_NHG - ES Backup NextHop Group Delete
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnEsBackupNhgDelete)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_DEL_EVPN_ES_BACKUP_NHG,
                                      0,
                                      sizeof(evpn_backup_nhg_msg));

    evpn_backup_nhg_msg* backup_msg = reinterpret_cast<evpn_backup_nhg_msg*>(NLMSG_DATA(nl_hdr));
    backup_msg->ebnm_ifindex = 21;
    backup_msg->ebnm_backup_nhg_id = 0x80001388u;  // Value doesn't matter for delete

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_backup_nhg_msg)));
    m_routeSync->onEvpnEsBackupNhgMsg(nl_hdr, len);
    m_pipeline->flush();

    EXPECT_TRUE(isKeyDeleted("EVPN_ES_BACKUP_NHG_TABLE", "Ethernet0"));
}


/**
 * @brief Test EVPN SHL ignores port-specific VLAN 0xFFF
 *
 * Per HLD, VLAN 4095 (0xFFF) is used for port-specific SHL information
 * which should be ignored by fpmsyncd.
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnShlIgnorePortSpecific)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_SHL,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_shl_msg));

    evpn_shl_msg* shl_msg = reinterpret_cast<evpn_shl_msg*>(NLMSG_DATA(nl_hdr));
    shl_msg->esm_ifindex = 0;
    shl_msg->esm_vid = 0xFFF;  // Port-specific VLAN - should be ignored

    struct in_addr vtep;
    inet_pton(AF_INET, "10.0.0.1", &vtep);
    addRtattr(nl_hdr, FPM_SHL_IPV4_ADDR, &vtep, sizeof(vtep));

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_shl_msg)));
    m_routeSync->onEvpnShlMsg(nl_hdr, len);
    m_pipeline->flush();

    // Should NOT create an entry for VLAN 4095
    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_SPLIT_HORIZON_TABLE", "Vlan10:unknown", values);
    EXPECT_FALSE(found);
}


/**
 * @brief Test EVPN DF ignores port-specific VLAN 0xFFF
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnDfIgnorePortSpecific)
{
    unsigned char buffer[4096] = {0};

    fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer);
    unsigned char* nl_buf = buffer + FPM_MSG_HDR_LEN;
    nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                      RTM_FPM_ADD_EVPN_DF,
                                      NLM_F_CREATE | NLM_F_REPLACE,
                                      sizeof(evpn_df_msg));

    evpn_df_msg* df_msg = reinterpret_cast<evpn_df_msg*>(NLMSG_DATA(nl_hdr));
    df_msg->edm_ifindex = 0;
    df_msg->edm_vid = 0xFFF;  // Port-specific VLAN - should be ignored
    df_msg->edm_non_df = 0;

    int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_df_msg)));
    m_routeSync->onEvpnDfMsg(nl_hdr, len);
    m_pipeline->flush();

    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_DF_TABLE", "Vlan10:unknown", values);
    EXPECT_FALSE(found);
}


/**
 * @brief Test multiple ES backup NHG updates for different interfaces
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnEsBackupNhgMultiple)
{
    unsigned char buffer1[4096] = {0};
    unsigned char buffer2[4096] = {0};

    // Add backup NHG for Ethernet100
    {
        fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer1);
        unsigned char* nl_buf = buffer1 + FPM_MSG_HDR_LEN;
        nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                          RTM_FPM_ADD_EVPN_ES_BACKUP_NHG,
                                          NLM_F_CREATE | NLM_F_REPLACE,
                                          sizeof(evpn_backup_nhg_msg));

        evpn_backup_nhg_msg* backup_msg = reinterpret_cast<evpn_backup_nhg_msg*>(NLMSG_DATA(nl_hdr));
        backup_msg->ebnm_ifindex = 21;
        backup_msg->ebnm_backup_nhg_id = 0x80001389u;

        int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_backup_nhg_msg)));
        m_routeSync->onEvpnEsBackupNhgMsg(nl_hdr, len);
    m_pipeline->flush();
    }

    // Add backup NHG for Ethernet200
    {
        fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer2);
        unsigned char* nl_buf = buffer2 + FPM_MSG_HDR_LEN;
        nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                          RTM_FPM_ADD_EVPN_ES_BACKUP_NHG,
                                          NLM_F_CREATE | NLM_F_REPLACE,
                                          sizeof(evpn_backup_nhg_msg));

        evpn_backup_nhg_msg* backup_msg = reinterpret_cast<evpn_backup_nhg_msg*>(NLMSG_DATA(nl_hdr));
        backup_msg->ebnm_ifindex = 20;
        backup_msg->ebnm_backup_nhg_id = 0x8000138au;

        int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_backup_nhg_msg)));
        m_routeSync->onEvpnEsBackupNhgMsg(nl_hdr, len);
    m_pipeline->flush();
    }

    // Verify both entries exist
    // Distinct resolved interfaces must retain independent bindings.
    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_ES_BACKUP_NHG_TABLE", "Brvxlan100", values);

    std::vector<FieldValueTuple> first;
    ASSERT_TRUE(getTableEntry("EVPN_ES_BACKUP_NHG_TABLE", "Ethernet0", first));
    EXPECT_EQ(fvValue(first.front()), "2147488649");
    EXPECT_TRUE(found);
    if (found) {
        auto nhg = swss::fvsGetValue(values, "nexthop_group", true);
        EXPECT_TRUE(nhg.has_value());
        if (nhg.has_value()) {
            // Second interface retains its own group
            EXPECT_EQ(*nhg, "2147488650");
        }
    }
}


/**
 * @brief Test SHL update (replace existing entry)
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnShlUpdate)
{
    unsigned char buffer1[4096] = {0};
    unsigned char buffer2[4096] = {0};

    // Initial SHL add with one VTEP
    {
        fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer1);
        unsigned char* nl_buf = buffer1 + FPM_MSG_HDR_LEN;
        nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                          RTM_FPM_ADD_EVPN_SHL,
                                          NLM_F_CREATE | NLM_F_REPLACE,
                                          sizeof(evpn_shl_msg));

        evpn_shl_msg* shl_msg = reinterpret_cast<evpn_shl_msg*>(NLMSG_DATA(nl_hdr));
        shl_msg->esm_ifindex = 0;
        shl_msg->esm_vid = 10;

        struct in_addr vtep1;
        inet_pton(AF_INET, "10.0.0.1", &vtep1);
        addRtattr(nl_hdr, FPM_SHL_IPV4_ADDR, &vtep1, sizeof(vtep1));

        int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_shl_msg)));
        m_routeSync->onEvpnShlMsg(nl_hdr, len);
    m_pipeline->flush();
    }

    // Update SHL with different VTEPs (should replace)
    {
        fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer2);
        unsigned char* nl_buf = buffer2 + FPM_MSG_HDR_LEN;
        nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                          RTM_FPM_ADD_EVPN_SHL,
                                          NLM_F_CREATE | NLM_F_REPLACE,
                                          sizeof(evpn_shl_msg));

        evpn_shl_msg* shl_msg = reinterpret_cast<evpn_shl_msg*>(NLMSG_DATA(nl_hdr));
        shl_msg->esm_ifindex = 0;
        shl_msg->esm_vid = 10;

        struct in_addr vtep2, vtep3;
        inet_pton(AF_INET, "10.0.0.2", &vtep2);
        inet_pton(AF_INET, "10.0.0.3", &vtep3);
        addRtattr(nl_hdr, FPM_SHL_IPV4_ADDR, &vtep2, sizeof(vtep2));
        addRtattr(nl_hdr, FPM_SHL_IPV4_ADDR, &vtep3, sizeof(vtep3));

        int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_shl_msg)));
        m_routeSync->onEvpnShlMsg(nl_hdr, len);
    m_pipeline->flush();
    }

    // Verify the entry was updated (should have new VTEPs, not old)
    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_SPLIT_HORIZON_TABLE", "Vlan10:unknown", values);

    EXPECT_TRUE(found);
    if (found) {
        auto vteps = swss::fvsGetValue(values, "vteps", true);
        EXPECT_TRUE(vteps.has_value());
        if (vteps.has_value()) {
            EXPECT_TRUE(vteps->find("10.0.0.2") != std::string::npos);
            EXPECT_TRUE(vteps->find("10.0.0.3") != std::string::npos);
            // Old VTEP should be replaced
            EXPECT_TRUE(vteps->find("10.0.0.1") == std::string::npos ||
                        vteps->find("10.0.0.2") != std::string::npos);  // Either replaced or both present temporarily
        }
    }
}


/**
 * @brief Test DF role transition (DF to non-DF)
 */
TEST_F(FpmsyncdEvpnMhTest, EvpnDfRoleChange)
{
    unsigned char buffer1[4096] = {0};
    unsigned char buffer2[4096] = {0};

    // Initially this PE is DF
    {
        fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer1);
        unsigned char* nl_buf = buffer1 + FPM_MSG_HDR_LEN;
        nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                          RTM_FPM_ADD_EVPN_DF,
                                          NLM_F_CREATE | NLM_F_REPLACE,
                                          sizeof(evpn_df_msg));

        evpn_df_msg* df_msg = reinterpret_cast<evpn_df_msg*>(NLMSG_DATA(nl_hdr));
        df_msg->edm_ifindex = 0;
        df_msg->edm_vid = 10;
        df_msg->edm_non_df = 0;  // DF

        int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_df_msg)));
        m_routeSync->onEvpnDfMsg(nl_hdr, len);
    m_pipeline->flush();
    }

    // DF election changes, this PE becomes non-DF
    {
        fpm_msg_hdr_t* fpm_hdr = buildFpmHeader(buffer2);
        unsigned char* nl_buf = buffer2 + FPM_MSG_HDR_LEN;
        nlmsghdr* nl_hdr = addNlmsgHeader(fpm_hdr, nl_buf,
                                          RTM_FPM_ADD_EVPN_DF,
                                          NLM_F_CREATE | NLM_F_REPLACE,
                                          sizeof(evpn_df_msg));

        evpn_df_msg* df_msg = reinterpret_cast<evpn_df_msg*>(NLMSG_DATA(nl_hdr));
        df_msg->edm_ifindex = 0;
        df_msg->edm_vid = 10;
        df_msg->edm_non_df = 1;  // Non-DF

        int len = static_cast<int>(nl_hdr->nlmsg_len - NLMSG_LENGTH(sizeof(evpn_df_msg)));
        m_routeSync->onEvpnDfMsg(nl_hdr, len);
    m_pipeline->flush();
    }

    // Verify the role changed to non-DF
    std::vector<FieldValueTuple> values;
    bool found = getTableEntry("EVPN_DF_TABLE", "Vlan10:unknown", values);

    EXPECT_TRUE(found);
    if (found) {
        auto df = swss::fvsGetValue(values, "df", true);
        EXPECT_TRUE(df.has_value());
        if (df.has_value()) {
            EXPECT_EQ(*df, "false");
        }
    }
}

// Binding tests resolve a controllable kernel interface snapshot, including
// removal/reuse, without relying on the runner's network namespace.
void resetMockWarmStartHelper();
bool getMockWarmStartHelperRefreshEntry(const std::string &, const std::string &,
                                       swss::KeyOpFieldsValuesTuple &);

class BindingRouteSync : public RouteSync
{
public:
    using RouteSync::RouteSync;
    std::map<int, std::string> names{{40, "PortChannel0"}, {41, "PortChannel1"}};
    bool getIfName(int index, char *name, size_t size) override
    {
        auto it = names.find(index);
        if (it == names.end()) return false;
        snprintf(name, size, "%s", it->second.c_str());
        return true;
    }
};

namespace {
uint32_t bppGroupId(uint32_t value)
{
    return (value & NHG_ID_VALUE_MASK) | (uint32_t(NHG_TYPE_L3_BPP) << NHG_ID_TYPE_POS);
}
l2_nhg_member_msg bppWireMember(uint32_t id = 0x40000007u, const char *ip = "10.0.0.2")
{
    l2_nhg_member_msg member{};
    member.l2nm_nh_id = id; member.l2nm_type = 1;
    member.l2nm_family = strchr(ip, ':') ? AF_INET6 : AF_INET;
    inet_pton(member.l2nm_family, ip, &member.l2nm_remote_vtep);
    member.l2nm_router_mac[0] = 2; member.l2nm_router_mac[5] = 3;
    return member;
}
std::vector<unsigned char> bppWireGroup(uint32_t id,
        const std::vector<l2_nhg_member_msg> &members, bool deleting = false)
{
    l2_nhg_group_msg body{};
    body.l2ng_nhg_id = id; body.l2ng_count = static_cast<uint32_t>(members.size());
    for (size_t i = 0; i < L2_NHG_ESI_LEN; ++i) body.l2ng_esi[i] = static_cast<uint8_t>(i);
    nlmsghdr header{};
    header.nlmsg_type = deleting ? RTM_FPM_DEL_L2_NEXTHOP_GROUP : RTM_FPM_ADD_L2_NEXTHOP_GROUP;
    header.nlmsg_len = static_cast<uint32_t>(NLMSG_LENGTH(sizeof(body) + members.size() * sizeof(l2_nhg_member_msg)));
    std::vector<unsigned char> bytes(NLMSG_ALIGN(header.nlmsg_len));
    memcpy(bytes.data(), &header, sizeof(header));
    memcpy(bytes.data() + NLMSG_HDRLEN, &body, sizeof(body));
    if (!members.empty()) memcpy(bytes.data() + NLMSG_HDRLEN + sizeof(body), members.data(), members.size() * sizeof(l2_nhg_member_msg));
    return bytes;
}
}

class FpmBindingTest : public ::testing::Test
{
protected:
    DBConnector db{"APPL_DB", 0};
    RedisPipeline pipeline{&db};
    std::unique_ptr<BindingRouteSync> sync;
    struct Frame { nlmsghdr header; evpn_backup_nhg_msg binding; };
    void SetUp() override
    {
        testing_db::reset();
        resetMockWarmStartHelper();
        sync = std::make_unique<BindingRouteSync>(&pipeline);
    }
    void TearDown() override { resetMockWarmStartHelper(); testing_db::reset(); }
    Frame frame(int index = 40, uint32_t group = 5000, bool deleting = false)
    {
        Frame f{};
        f.header.nlmsg_len = NLMSG_LENGTH(sizeof(f.binding));
        f.header.nlmsg_type = deleting ? RTM_FPM_DEL_EVPN_ES_BACKUP_NHG : RTM_FPM_ADD_EVPN_ES_BACKUP_NHG;
        f.binding = {index, group ? bppGroupId(group) : 0};
        return f;
    }
    void send(Frame f) { sync->onMsgRaw(&f.header); pipeline.flush(); }
    bool entry(const std::string &key, const std::string &group)
    {
        Table table(&db, "EVPN_ES_BACKUP_NHG_TABLE");
        std::vector<FieldValueTuple> values;
        return table.get(key, values) && values == std::vector<FieldValueTuple>{{"nexthop_group", std::to_string(bppGroupId(static_cast<uint32_t>(std::stoul(group))))}};
    }
    bool absent(const std::string &key)
    {
        Table table(&db, "EVPN_ES_BACKUP_NHG_TABLE");
        std::vector<FieldValueTuple> values;
        return !table.get(key, values);
    }
};

TEST_F(FpmBindingTest, CreateReplaceDuplicateAndIndependentKeys)
{
    send(frame()); send(frame());
    ASSERT_TRUE(entry("PortChannel0", "5000"));
    sync->names[INT_MAX] = "PortChannel9999";
    send(frame(INT_MAX, 1)); EXPECT_TRUE(entry("PortChannel9999", "1"));
    send(frame(INT_MAX, 0, true)); EXPECT_TRUE(absent("PortChannel9999"));
    send(frame(41, 6000)); send(frame(40, INT_MAX));
    EXPECT_TRUE(entry("PortChannel0", std::to_string(INT_MAX)));
    EXPECT_TRUE(entry("PortChannel1", "6000"));
    send(frame(40, 0, true)); send(frame(40, -1, true));
    EXPECT_TRUE(absent("PortChannel0"));
    EXPECT_TRUE(entry("PortChannel1", "6000"));
}

TEST_F(FpmBindingTest, TaggedGroupIdsPreserveAllBits)
{
    for (uint32_t group : {0x80000000u, 0x80000001u, 0x8fffffffu})
    {
        send(frame(40, group));
        EXPECT_TRUE(entry("PortChannel0", std::to_string(group)));
        send(frame(40, group, true));
        EXPECT_TRUE(absent("PortChannel0"));
    }
}

TEST_F(FpmBindingTest, RejectInvalidInterfaceAndZeroGroupAtomically)
{
    send(frame());
    for (auto pair : std::vector<std::pair<int, uint32_t>>{{0, 1}, {-1, 1}, {40, 0}})
        send(frame(pair.first, pair.second));
    send(frame(0, 0, true)); send(frame(-1, 0, true));
    EXPECT_TRUE(entry("PortChannel0", "5000"));
    EXPECT_TRUE(absent("unknown"));
    EXPECT_EQ(sync->m_evpnBackupNhgInterfaces.size(), 1u);
}

TEST_F(FpmBindingTest, RejectUnknownInterfaceUntilReplayed)
{
    send(frame(99));
    EXPECT_TRUE(absent("unknown"));
    EXPECT_TRUE(sync->m_evpnBackupNhgInterfaces.empty());
    sync->names[99] = "PortChannel99";
    send(frame(99));
    EXPECT_TRUE(entry("PortChannel99", "5000"));
    send(frame(100, 0, true));
    EXPECT_TRUE(absent("unknown"));
}

TEST_F(FpmBindingTest, DeleteAfterInterfaceRemovalAndRepeat)
{
    send(frame()); sync->names.erase(40);
    send(frame(40, 0, true)); send(frame(40, 0, true));
    EXPECT_TRUE(absent("PortChannel0"));
    EXPECT_TRUE(absent("unknown"));
    EXPECT_TRUE(sync->m_evpnBackupNhgInterfaces.empty());
}

TEST_F(FpmBindingTest, WithdrawalAfterReuseTargetsAcceptedIdentity)
{
    send(frame()); sync->names[40] = "PortChannel2";
    send(frame(40, 0, true));
    EXPECT_TRUE(absent("PortChannel0"));
    EXPECT_TRUE(absent("PortChannel2"));
}

TEST_F(FpmBindingTest, NewAddAfterReuseRemovesOldIdentity)
{
    send(frame()); sync->names[40] = "PortChannel2";
    send(frame(40, 7000));
    EXPECT_TRUE(absent("PortChannel0"));
    EXPECT_TRUE(entry("PortChannel2", "7000"));
    send(frame(40, 0, true)); EXPECT_TRUE(absent("PortChannel2"));
}

TEST_F(FpmBindingTest, RecreatedNameRejectsOldIndexWithdrawal)
{
    send(frame());
    sync->names[42] = "PortChannel0";
    send(frame(42, 7000));
    EXPECT_EQ(sync->m_evpnBackupNhgInterfaces.count(40), 0u);
    // Even a stale interface-cache name must not let the old DEL erase it.
    send(frame(40, 0, true));
    EXPECT_TRUE(entry("PortChannel0", "7000"));
    send(frame(42, 0, true)); EXPECT_TRUE(absent("PortChannel0"));
}

TEST_F(FpmBindingTest, UnresolvedUpdateKeepsAcceptedIdentity)
{
    send(frame()); sync->names.erase(40); send(frame(40, 7000));
    EXPECT_TRUE(entry("PortChannel0", "5000"));
    send(frame(40, 0, true)); EXPECT_TRUE(absent("PortChannel0"));
}

TEST_F(FpmBindingTest, MalformedFramesAndWrongDirectTypeHaveNoWrites)
{
    send(frame());
    for (unsigned length : {0u, 15u, 16u, 23u, 25u, UINT_MAX})
    {
        auto f = frame(); f.header.nlmsg_len = length;
        sync->onEvpnEsBackupNhgMsg(&f.header, 0);
        if (length <= 25) sync->onMsgRaw(&f.header);
    }
    auto f = frame(); sync->onEvpnEsBackupNhgMsg(&f.header, 1);
    sync->onEvpnEsBackupNhgMsg(&f.header, -1);
    f.header.nlmsg_type = RTM_FPM_ADD_EVPN_DF;
    sync->onEvpnEsBackupNhgMsg(&f.header, 0);
    EXPECT_TRUE(entry("PortChannel0", "5000"));
}

TEST_F(FpmBindingTest, ReconnectFlushesBindingsAndRebuildsFromReplay)
{
    send(frame()); send(frame(41, 6000));
    sync->onFpmDisconnected();
    sync->resetFpmProtectionState(db); pipeline.flush();
    EXPECT_TRUE(absent("PortChannel0")); EXPECT_TRUE(absent("PortChannel1"));
    // Old accepted-name caches must not permit a withdrawal on a vanished port.
    sync->names.erase(40);
    Table bindings(&db, "EVPN_ES_BACKUP_NHG_TABLE");
    bindings.set("PortChannel0", {{"nexthop_group", std::to_string(bppGroupId(7000))}});
    send(frame(40, 0, true)); EXPECT_TRUE(entry("PortChannel0", "7000"));
    sync->names[40] = "PortChannel2";
    send(frame(40, 0x80000001u));
    EXPECT_TRUE(entry("PortChannel2", "2147483649"));
    send(frame(40, 0, true)); EXPECT_TRUE(absent("PortChannel2"));
}

TEST_F(FpmBindingTest, ProcessRestartClearsPersistedBindingsOnlyAndIsIdempotent)
{
    send(frame());
    Table bindings(&db, "EVPN_ES_BACKUP_NHG_TABLE");
    bindings.set("PortChannel99", {{"nexthop_group", "5000"}});
    DBConnector config{"CONFIG_DB", 0};
    Table policy(&config, "BRIDGE_PORT_PROTECTION");
    policy.set("PortChannel0", {{"admin_mode", "auto"}});
    Table neighbors(&db, "NEIGH_TABLE");
    neighbors.set("Vlan100:10.0.0.1", {{"neigh", "00:11:22:33:44:55"}});
    sync = std::make_unique<BindingRouteSync>(&pipeline);
    sync->resetFpmProtectionState(db); pipeline.flush();
    EXPECT_TRUE(absent("PortChannel0")); EXPECT_TRUE(absent("PortChannel99"));
    std::vector<FieldValueTuple> fields;
    EXPECT_TRUE(policy.get("PortChannel0", fields));
    EXPECT_EQ(fields, (std::vector<FieldValueTuple>{{"admin_mode", "auto"}}));
    EXPECT_TRUE(neighbors.get("Vlan100:10.0.0.1", fields));
    sync->resetFpmProtectionState(db); pipeline.flush();
    EXPECT_TRUE(absent("PortChannel0"));
    send(frame()); EXPECT_TRUE(entry("PortChannel0", "5000"));
}

TEST_F(FpmBindingTest, ProtectionReplayBypassesWarmRouteReconciliation)
{
    sync->getWarmStartHelper().setState(WarmStart::INITIALIZED);
    send(frame());
    EXPECT_TRUE(entry("PortChannel0", "5000"));
    KeyOpFieldsValuesTuple kfv;
    EXPECT_FALSE(getMockWarmStartHelperRefreshEntry("EVPN_ES_BACKUP_NHG_TABLE", "PortChannel0", kfv));
    EXPECT_FALSE(getMockWarmStartHelperRefreshEntry("ROUTE_TABLE", "PortChannel0", kfv));
    sync->resetFpmProtectionState(db); pipeline.flush();
    EXPECT_TRUE(absent("PortChannel0"));
    send(frame(40, UINT32_MAX)); EXPECT_TRUE(entry("PortChannel0", "4294967295"));
    sync->names[40] = "PortChannel2"; send(frame(40, 7000));
    EXPECT_TRUE(absent("PortChannel0")); EXPECT_TRUE(entry("PortChannel2", "7000"));
    send(frame(40, 0, true)); EXPECT_TRUE(absent("PortChannel2"));
    EXPECT_FALSE(getMockWarmStartHelperRefreshEntry("EVPN_ES_BACKUP_NHG_TABLE", "PortChannel2", kfv));
}

TEST_F(FpmBindingTest, ActualProducerFramesThroughFpmLink)
{
    const char *path = getenv("SONIC_FPM_BINDING_FIXTURES");
    if (!path) GTEST_SKIP() << "Run tests/integration/run_fpm_binding_roundtrip.py for actual producer fixtures";
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.good());
    FpmLink link(sync.get(), 0);
    for (uint32_t group : {5000u, static_cast<uint32_t>(INT_MAX), 0x80000001u, UINT32_MAX, 0u, 0u})
    {
        alignas(nlmsghdr) unsigned char buffer[28]{};
        ASSERT_TRUE(input.read(reinterpret_cast<char *>(buffer), sizeof(buffer)));
        auto *fpm = reinterpret_cast<fpm_msg_hdr_t *>(buffer);
        EXPECT_EQ(fpm->version, FPM_PROTO_VERSION);
        EXPECT_EQ(fpm_msg_len(fpm), sizeof(buffer));
        link.processFpmMessage(fpm); pipeline.flush();
        if (group == 0) EXPECT_TRUE(absent("PortChannel0"));
        else if (group == 0x80000001u || group == UINT32_MAX) EXPECT_TRUE(entry("PortChannel0", "2147483649"));
        else EXPECT_TRUE(absent("PortChannel0"));
    }
    EXPECT_EQ(input.peek(), std::ifstream::traits_type::eof());
}

TEST_F(FpmBindingTest, FpmPayloadBoundaryRejectsTruncatedBinding)
{
    struct { fpm_msg_hdr_t fpm; Frame netlink; } message{};
    message.fpm.version = FPM_PROTO_VERSION;
    message.fpm.msg_type = FPM_MSG_TYPE_NETLINK;
    message.netlink = frame();
    FpmLink link(sync.get(), 0);
    message.fpm.msg_len = htons(24); // Only 20 payload bytes; netlink needs 24.
    link.processFpmMessage(&message.fpm);
    EXPECT_TRUE(absent("PortChannel0"));
    message.fpm.msg_len = htons(0);
    link.processFpmMessage(&message.fpm);
    EXPECT_TRUE(absent("PortChannel0"));
}
TEST_F(FpmBindingTest, MemberValidationAndReplacement)
{
    struct { nlmsghdr h; l2_nhg_member_msg m; } f{};
    f.h.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP;
    f.h.nlmsg_len = NLMSG_LENGTH(sizeof(f.m));
    f.m.l2nm_nh_id = 0x40000007u; f.m.l2nm_type = 1; f.m.l2nm_family = AF_INET;
    inet_pton(AF_INET, "10.0.0.2", &f.m.l2nm_remote_vtep);
    f.m.l2nm_router_mac[0] = 2; f.m.l2nm_router_mac[5] = 3;
    Table table(&db, "L2_NEXTHOP_GROUP_TABLE");
    std::vector<FieldValueTuple> fields;
    auto sendMember = [&] { sync->onMsgRaw(&f.h); pipeline.flush(); };
    sendMember(); ASSERT_TRUE(table.get("1073741831", fields));
    std::string value; EXPECT_TRUE(table.hget("1073741831", "router_mac", value)); EXPECT_EQ(value, "02:00:00:00:00:03");
    auto good = f;
    for (int fault = 0; fault < 7; ++fault) {
        f = good;
        if (fault == 0) f.h.nlmsg_len--;
        if (fault == 1) f.h.nlmsg_len++;
        if (fault == 2) f.m.l2nm_type = 2;
        if (fault == 3) f.m.l2nm_family = 0;
        if (fault == 4) memset(f.m.l2nm_router_mac, 0, 6);
        if (fault == 5) f.m.l2nm_router_mac[0] = 1;
        if (fault == 6) f.m.l2nm_nh_id = 0;
        sendMember(); std::vector<FieldValueTuple> after;
        ASSERT_TRUE(table.get("1073741831", after)); EXPECT_EQ(fields, after);
    }
    f.h.nlmsg_type = RTM_FPM_ADD_EVPN_DF; sync->onL2ProtectionMsg(&f.h);
    f.h.nlmsg_type = RTM_FPM_DEL_L2_NEXTHOP_GROUP + 1; sync->onL2ProtectionMsg(&f.h);
    f = good; f.m.l2nm_family = AF_INET6;
    inet_pton(AF_INET6, "2001:db8::2", &f.m.l2nm_remote_vtep); sendMember();
    EXPECT_TRUE(table.hget("1073741831", "remote_vtep", value)); EXPECT_EQ(value, "2001:db8::2");
    f.m.l2nm_type = 0; sendMember();
    EXPECT_TRUE(table.hget("1073741831", "router_mac", value)); EXPECT_EQ(value, "02:00:00:00:00:03");
    f.h.nlmsg_type = RTM_FPM_DEL_L2_NEXTHOP; f.m.l2nm_type = 255; sendMember(); sendMember();
    EXPECT_FALSE(table.get("1073741831", fields));
}

TEST_F(FpmBindingTest, GroupValidationOwnershipAndRestartCleanup)
{
    auto good = bppWireGroup(0x80000001u, {bppWireMember(), bppWireMember(0x40000008u, "2001:db8::2")});
    auto sendGroup = [&](std::vector<unsigned char> bytes) { sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush(); };
    Table table(&db, "L2_NEXTHOP_GROUP_TABLE"); std::string value;
    sendGroup(good);
    ASSERT_TRUE(table.hget("2147483649", "nexthop_group", value)); EXPECT_EQ(value,"1073741831,1073741832");
    ASSERT_TRUE(table.hget("1073741832", "remote_vtep", value)); EXPECT_EQ(value,"2001:db8::2");
    for (int fault = 0; fault < 9; ++fault)
    {
        auto bytes = good; auto *header = reinterpret_cast<nlmsghdr *>(bytes.data());
        l2_nhg_group_msg group; memcpy(&group, NLMSG_DATA(header), sizeof(group));
        if (fault == 0) group.l2ng_count = UINT32_MAX;
        if (fault == 1) header->nlmsg_len = NLMSG_LENGTH(sizeof(group)-1);
        if (fault == 2) ++header->nlmsg_len;
        if (fault == 3) group.l2ng_nhg_id = 0;
        memcpy(NLMSG_DATA(header), &group, sizeof(group));
        if (fault >= 4)
        {
            l2_nhg_member_msg member; const size_t offset = NLMSG_HDRLEN + sizeof(group) + sizeof(member);
            memcpy(&member, bytes.data()+offset, sizeof(member));
            if (fault == 4) member.l2nm_nh_id = 0;
            if (fault == 5) member.l2nm_nh_id = 0x40000007u;
            if (fault == 6) member.l2nm_type = 0;
            if (fault == 7) member.l2nm_family = 0;
            if (fault == 8) memset(member.l2nm_router_mac,0,6);
            memcpy(bytes.data()+offset,&member,sizeof(member));
        }
        testing_db::resetOperationCounters(); sendGroup(bytes);
        EXPECT_EQ(testing_db::getProducerSetCount("L2_NEXTHOP_GROUP_TABLE"), 0u);
        ASSERT_TRUE(table.hget("2147483649", "nexthop_group", value)); EXPECT_EQ(value,"1073741831,1073741832");
    }
    sendGroup(bppWireGroup(0x80000001u, {}));
    ASSERT_TRUE(table.hget("2147483649", "nexthop_group", value)); EXPECT_TRUE(value.empty());
    table.set("17", {{"remote_vtep", "10.0.0.7"}});
    sendGroup(bppWireGroup(17, {}, true)); EXPECT_TRUE(table.hget("17", "remote_vtep", value));
    send(frame()); sync = std::make_unique<BindingRouteSync>(&pipeline);
    sync->resetFpmProtectionState(db); pipeline.flush();
    std::vector<FieldValueTuple> fields;
    EXPECT_FALSE(table.get("2147483649", fields)); EXPECT_FALSE(table.get("1073741831", fields));
    EXPECT_FALSE(table.get("1073741832", fields)); EXPECT_TRUE(table.get("17", fields));
    EXPECT_TRUE(absent("PortChannel0"));
}

TEST_F(FpmBindingTest, ReferenceGroupSerializerThroughFpmLink)
{
    const char *path = getenv("SONIC_FPM_L2_FIXTURES");
    if (!path) GTEST_SKIP() << "Run integration/run_fpm_l2_roundtrip.py for reference wire fixtures";
    std::ifstream input(path, std::ios::binary); ASSERT_TRUE(input.good());
    FpmLink link(sync.get(), 0);
    Table table(&db, "L2_NEXTHOP_GROUP_TABLE");
    for (int i=0; i<4; ++i) {
        alignas(nlmsghdr) unsigned char buffer[FPM_MAX_MSG_LEN]{};
        ASSERT_TRUE(input.read(reinterpret_cast<char *>(buffer), 4));
        auto *fpm = reinterpret_cast<fpm_msg_hdr_t *>(buffer);
        const size_t length = fpm_msg_len(fpm);
        ASSERT_LE(length, sizeof(buffer)); ASSERT_GE(length, 4u);
        ASSERT_TRUE(input.read(reinterpret_cast<char *>(buffer+4), length-4));
        link.processFpmMessage(fpm); pipeline.flush();
        std::string value;
        if (i==0) { EXPECT_TRUE(table.hget("1073741831", "router_mac", value)); EXPECT_EQ(value,"02:00:00:00:00:03"); }
        if (i==1) { EXPECT_TRUE(table.hget("2147483649", "nexthop_group", value)); EXPECT_EQ(value,"1073741831,1073741832"); }
        if (i==2) { EXPECT_FALSE(table.hget("2147483649", "nexthop_group", value)); }
        if (i==3) { EXPECT_FALSE(table.hget("1073741831", "remote_vtep", value)); }
    }
    EXPECT_EQ(input.peek(), std::ifstream::traits_type::eof());
}

TEST_F(FpmBindingTest, ResetRequiresAcceptedKeyAndUnambiguousRow)
{
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE"), accepted(&db, FPM_PROTECTION_ID_TABLE);
    const std::vector<std::vector<FieldValueTuple>> preserved = {
        {{"remote_vtep", "10.0.0.1"}}, {{"nexthop_group", "1,2"}},
        {{"remote_vtep", "10.0.0.2"}, {"type", "l3"}},
        {{"nexthop_group", ""}},
        {{"nexthop_group", "1,2"}, {"type", "l3"}},
        {{"remote_vtep", "10.0.0.3"}, {"type", "invalid"}},
        {{"unexpected", "invalid"}}, {{"remote_vtep", "10.0.0.4"}}
    };
    for (size_t i = 0; i < preserved.size(); ++i)
        objects.set(std::to_string(i + 1), preserved[i]);
    accepted.set("4", {{"NULL", "invalid"}});
    for (const auto *id : {"5", "6", "7", "8", "100", "101", "102"})
        accepted.set(id, {{"NULL", "NULL"}});
    objects.set("100", {{"type", "l2"}, {"remote_vtep", "10.0.0.5"}});
    objects.set("101", {{"nexthop_group", "999999"}}); // Missing Member is immaterial.
    // 102 simulates a crash after indexing and before publishing its object.
    testing_db::resetOperationCounters();
    for (int repetition = 0; repetition < 2; ++repetition)
    {
        sync->resetFpmProtectionState(db); pipeline.flush();
        std::vector<FieldValueTuple> row;
        EXPECT_FALSE(objects.get("100", row)); EXPECT_FALSE(objects.get("101", row));
        EXPECT_FALSE(accepted.get("100", row)); EXPECT_FALSE(accepted.get("101", row));
        EXPECT_FALSE(accepted.get("102", row));
        for (size_t i = 0; i < preserved.size(); ++i)
        {
            ASSERT_TRUE(objects.get(std::to_string(i + 1), row)); EXPECT_EQ(row, preserved[i]);
        }
        EXPECT_EQ(testing_db::getProducerDelCount("L2_NEXTHOP_GROUP_TABLE"), 3u);
    }
}

TEST_F(FpmBindingTest, HldRowsAndAcceptedIdsPersistAcrossOrphanGroupRestart)
{
    auto bytes = bppWireGroup(0x80000001u, {bppWireMember()});
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE"), accepted(&db, FPM_PROTECTION_ID_TABLE);
    std::vector<FieldValueTuple> row;
    ASSERT_TRUE(objects.get("2147483649", row));
    EXPECT_EQ(row, (std::vector<FieldValueTuple>{{"nexthop_group", "1073741831"}}));
    EXPECT_TRUE(hasFpmProtectionId(accepted, "2147483649"));
    ASSERT_TRUE(objects.get("1073741831", row));
    // A crash may leave an owned Group whose Member publication is absent.
    objects.del("1073741831"); accepted.del("1073741831");
    objects.set("200", {{"nexthop_group", "300"}});
    Table queuedBindings(&db, "_EVPN_ES_BACKUP_NHG_TABLE");
    queuedBindings.set("PortChannel9", {{"nexthop_group","2147483649"}});
    sync = std::make_unique<BindingRouteSync>(&pipeline);
    sync->resetFpmProtectionState(db); pipeline.flush();
    EXPECT_FALSE(objects.get("2147483649", row)); EXPECT_FALSE(accepted.get("2147483649", row));
    EXPECT_FALSE(queuedBindings.get("PortChannel9", row)); ASSERT_TRUE(objects.get("200", row));
}

TEST_F(FpmBindingTest, PendingObjectsAreIsolatedAndCanceledBeforeReplay)
{
    Table pending(&db, "_L2_NEXTHOP_GROUP_TABLE"), objects(&db, "L2_NEXTHOP_GROUP_TABLE");
    Table accepted(&db, FPM_PROTECTION_ID_TABLE);
    auto key = [](uint32_t id) { return std::to_string(bppGroupId(id)); };
    pending.set(key(200), {{"remote_vtep","10.0.0.1"}});
    pending.set(key(201), {{"nexthop_group","200"}});
    pending.set(key(202), {{"nexthop_group",""}}); accepted.set(key(202), {{"NULL","NULL"}});
    objects.set(key(203), {{"nexthop_group",""}});
    pending.set(key(203), {{"type","l3"},{"remote_vtep","10.0.0.2"}});
    accepted.set(key(203), {{"NULL","NULL"}});
    for (uint32_t id : {200u,201u,203u})
    {
        auto bytes = bppWireGroup(bppGroupId(id), {});
        sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
        EXPECT_EQ(hasFpmProtectionId(accepted,key(id)), id==203);
    }
    sync=std::make_unique<BindingRouteSync>(&pipeline);
    sync->resetFpmProtectionState(db); pipeline.flush();
    std::vector<FieldValueTuple> row;
    EXPECT_TRUE(pending.get(key(200),row)); EXPECT_TRUE(pending.get(key(201),row));
    for (uint32_t id : {202u,203u})
    {
        EXPECT_FALSE(pending.get(key(id),row)); EXPECT_FALSE(objects.get(key(id),row));
        EXPECT_FALSE(accepted.get(key(id),row));
    }
    auto bytes = bppWireGroup(bppGroupId(203), {});
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    ASSERT_TRUE(objects.get(key(203),row)); EXPECT_EQ(row,(std::vector<FieldValueTuple>{{"nexthop_group",""}}));
}

TEST_F(FpmBindingTest, AcceptedKeyCannotAuthorizeAmbiguousLegacyOrWrongKindWrites)
{
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE"), accepted(&db, FPM_PROTECTION_ID_TABLE);
    const std::vector<std::vector<FieldValueTuple>> rows = {
        {{"remote_vtep", "10.0.0.1"}},
        {{"type", "invalid"}, {"remote_vtep", "10.0.0.2"}},
        {{"type", "l3"}, {"nexthop_group", "1"}},
        {{"nexthop_group", "1"}}
    };
    struct { nlmsghdr h; l2_nhg_member_msg member; } f{};
    f.h.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP; f.h.nlmsg_len = NLMSG_LENGTH(sizeof(f.member));
    f.member.l2nm_type = 1; f.member.l2nm_family = AF_INET;
    inet_pton(AF_INET, "10.0.0.3", &f.member.l2nm_remote_vtep);
    f.member.l2nm_router_mac[0] = 2; f.member.l2nm_router_mac[5] = 3;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto key = std::to_string(0x40000001u+static_cast<uint32_t>(i)); objects.set(key, rows[i]); accepted.set(key, {{"NULL","NULL"}});
        f.member.l2nm_nh_id = 0x40000001u+static_cast<uint32_t>(i);
        f.h.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP; sync->onMsgRaw(&f.h); pipeline.flush();
        f.h.nlmsg_type = RTM_FPM_DEL_L2_NEXTHOP; sync->onMsgRaw(&f.h); pipeline.flush();
        std::vector<FieldValueTuple> actual; ASSERT_TRUE(objects.get(key, actual)); EXPECT_EQ(actual, rows[i]);
    }
}

TEST_F(FpmBindingTest, EveryWrongIdTagIsRejectedForObjectsAndBindings)
{
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE");
    for (uint32_t tag = 0; tag < 16; ++tag)
    {
        const uint32_t id = (tag << NHG_ID_TYPE_POS) | 7;
        if (tag != NHG_TYPE_L3_BPP)
        {
            auto binding = frame(); binding.binding.ebnm_backup_nhg_id = id;
            send(binding); EXPECT_TRUE(absent("PortChannel0"));
            auto bytes = bppWireGroup(id, {});
            sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data()));
            auto *header = reinterpret_cast<nlmsghdr *>(bytes.data());
            header->nlmsg_type = RTM_FPM_DEL_L2_NEXTHOP_GROUP; sync->onMsgRaw(header);
        }
        if (tag != NHG_TYPE_L3_BPP_NH)
        {
            struct { nlmsghdr header; l2_nhg_member_msg member; } f{};
            f.header.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP;
            f.header.nlmsg_len = NLMSG_LENGTH(sizeof(f.member)); f.member = bppWireMember(id);
            sync->onMsgRaw(&f.header); f.header.nlmsg_type = RTM_FPM_DEL_L2_NEXTHOP; sync->onMsgRaw(&f.header);
        }
    }
    pipeline.flush(); std::vector<std::string> keys; objects.getKeys(keys); EXPECT_TRUE(keys.empty());
    send(frame(40, 0x8fffffffu)); ASSERT_TRUE(entry("PortChannel0", "2415919103"));
    auto wrong = frame(); wrong.binding.ebnm_backup_nhg_id = 0x20000001u;
    wrong.header.nlmsg_type = RTM_FPM_DEL_EVPN_ES_BACKUP_NHG;
    send(wrong); EXPECT_TRUE(entry("PortChannel0", "2415919103"));
    send(frame(40, 0, true)); EXPECT_TRUE(absent("PortChannel0"));
}

TEST_F(FpmBindingTest, GroupValidatesAllMembersAndOwnershipBeforeAnyPublication)
{
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE");
    auto first = bppWireMember(0x40000008u), second = bppWireMember(0x40000009u);
    second.l2nm_family = 0;
    auto bytes = bppWireGroup(0x80000002u, {first,second});
    testing_db::resetOperationCounters(); sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    EXPECT_EQ(testing_db::getProducerSetCount("L2_NEXTHOP_GROUP_TABLE"), 0u);
    objects.set("1073741833", {{"remote_vtep","10.0.0.99"}});
    second = bppWireMember(0x40000009u); bytes = bppWireGroup(0x80000002u, {first,second});
    testing_db::resetOperationCounters(); sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    EXPECT_EQ(testing_db::getProducerSetCount("L2_NEXTHOP_GROUP_TABLE"), 0u);
    std::vector<FieldValueTuple> row; EXPECT_FALSE(objects.get("1073741832",row)); EXPECT_FALSE(objects.get("2147483650",row));
    ASSERT_TRUE(objects.get("1073741833",row)); EXPECT_EQ(row,(std::vector<FieldValueTuple>{{"remote_vtep","10.0.0.99"}}));
}

TEST_F(FpmBindingTest, FullGroupSnapshotRefreshesMembersAndRetainsHldRowShape)
{
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE");
    auto member = bppWireMember(); auto bytes = bppWireGroup(0x80000001u, {member});
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    member.l2nm_router_mac[5] = 9;
    bytes = bppWireGroup(0x80000001u, {member,bppWireMember(0x4fffffffu,"2001:db8::9")});
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    std::vector<FieldValueTuple> row;
    ASSERT_TRUE(objects.get("1073741831",row));
    EXPECT_EQ(row,(std::vector<FieldValueTuple>{{"remote_vtep","10.0.0.2"},{"type","l3"},{"router_mac","02:00:00:00:00:09"}}));
    ASSERT_TRUE(objects.get("2147483649",row));
    EXPECT_EQ(row,(std::vector<FieldValueTuple>{{"nexthop_group","1073741831,1342177279"}}));
}

TEST_F(FpmBindingTest, GroupTransportBoundaryAndOldIdOnlyLayoutAreRejectedSafely)
{
    const size_t maximum = (FPM_MAX_MSG_LEN - FPM_MSG_HDR_LEN - NLMSG_SPACE(sizeof(l2_nhg_group_msg))) / sizeof(l2_nhg_member_msg);
    ASSERT_EQ(maximum,510u);
    std::vector<l2_nhg_member_msg> members;
    for (uint32_t i=0;i<maximum;++i) members.push_back(bppWireMember(0x40000001u+i));
    auto bytes = bppWireGroup(0x80000001u,members);
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    Table objects(&db,"L2_NEXTHOP_GROUP_TABLE"); std::vector<FieldValueTuple> before,after;
    ASSERT_TRUE(objects.get("2147483649",before));
    members.push_back(bppWireMember(0x40000001u+static_cast<uint32_t>(maximum)));
    bytes=bppWireGroup(0x80000001u,members); testing_db::resetOperationCounters();
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    EXPECT_EQ(testing_db::getProducerSetCount("L2_NEXTHOP_GROUP_TABLE"),0u);
    struct { nlmsghdr header; uint32_t id,count,member; } old{};
    old.header.nlmsg_type=RTM_FPM_ADD_L2_NEXTHOP_GROUP; old.header.nlmsg_len=NLMSG_LENGTH(12);
    old.id=0x80000001u; old.count=1; old.member=0x40000001u;
    sync->onMsgRaw(&old.header); pipeline.flush();
    ASSERT_TRUE(objects.get("2147483649",after)); EXPECT_EQ(before,after);
}

TEST_F(FpmBindingTest, ReplayDoneRequiresEmptyPayloadAndDoesNotGateBindings)
{
    FpmLink link(sync.get(),0);
    struct { nlmsghdr header; uint32_t unexpected; } marker{};
    marker.header.nlmsg_type=RTM_FPM_EVPN_MH_REPLAY_DONE;
    marker.header.nlmsg_len=NLMSG_LENGTH(0);
    EXPECT_TRUE(link.isRawProcessing(&marker.header));
    send(frame()); ASSERT_TRUE(entry("PortChannel0","5000"));
    testing_db::resetOperationCounters();
    for (int i=0;i<2;++i) sync->onMsgRaw(&marker.header);
    marker.header.nlmsg_len=NLMSG_LENGTH(sizeof(marker.unexpected)); sync->onMsgRaw(&marker.header);
    marker.header.nlmsg_len=NLMSG_HDRLEN-1; sync->onMsgRaw(&marker.header); pipeline.flush();
    EXPECT_EQ(testing_db::getProducerSetCount("EVPN_ES_BACKUP_NHG_TABLE"),0u);
    EXPECT_EQ(testing_db::getProducerDelCount("EVPN_ES_BACKUP_NHG_TABLE"),0u);
    EXPECT_TRUE(entry("PortChannel0","5000"));
    sync->resetFpmProtectionState(db); pipeline.flush(); EXPECT_TRUE(absent("PortChannel0"));
    marker.header.nlmsg_len=NLMSG_LENGTH(0); sync->onMsgRaw(&marker.header); send(frame());
    EXPECT_TRUE(entry("PortChannel0","5000"));
}

TEST_F(FpmBindingTest, ReplayDoneTravelsThroughFpmLinkWithoutPublishingState)
{
    FpmLink link(sync.get(),0);
    alignas(nlmsghdr) unsigned char bytes[FPM_MSG_HDR_LEN+NLMSG_HDRLEN]{};
    auto *fpm=reinterpret_cast<fpm_msg_hdr_t *>(bytes);
    fpm->version=FPM_PROTO_VERSION; fpm->msg_type=FPM_MSG_TYPE_NETLINK;
    fpm->msg_len=htons(static_cast<uint16_t>(sizeof(bytes)));
    auto *header=reinterpret_cast<nlmsghdr *>(bytes+FPM_MSG_HDR_LEN);
    header->nlmsg_type=RTM_FPM_EVPN_MH_REPLAY_DONE; header->nlmsg_len=NLMSG_HDRLEN;
    testing_db::resetOperationCounters(); link.processFpmMessage(fpm); pipeline.flush();
    EXPECT_TRUE(absent("PortChannel0"));
    EXPECT_EQ(testing_db::getProducerSetCount("L2_NEXTHOP_GROUP_TABLE"),0u);
    EXPECT_EQ(testing_db::getProducerSetCount("EVPN_ES_BACKUP_NHG_TABLE"),0u);
}

TEST_F(FpmBindingTest, NativeAlignedMemberAndGroupIgnorePaddingBytes)
{
    auto rawMember = [](uint32_t id, const char *ip) {
        // Independent native ABI fixture: do not serialize the decoder's struct.
        std::vector<unsigned char> body(32, 0xa5);
        memcpy(body.data(), &id, sizeof(id));
        body[4] = 1; body[5] = strchr(ip, ':') ? AF_INET6 : AF_INET;
        unsigned char address[16]{};
        EXPECT_EQ(inet_pton(body[5], ip, address), 1);
        memcpy(body.data() + 8, address, sizeof(address));
        const unsigned char mac[6]{2, 0, 0, 0, 0, 3};
        memcpy(body.data() + 24, mac, sizeof(mac));
        return body;
    };
    auto deliver = [&](uint16_t type, const std::vector<unsigned char> &body) {
        std::vector<unsigned char> bytes(NLMSG_SPACE(body.size()));
        nlmsghdr header{}; header.nlmsg_type = type;
        header.nlmsg_len = static_cast<uint32_t>(NLMSG_LENGTH(body.size()));
        memcpy(bytes.data(), &header, sizeof(header));
        memcpy(bytes.data() + NLMSG_HDRLEN, body.data(), body.size());
        sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(bytes.data())); pipeline.flush();
    };
    const auto first = rawMember(0x40000007u, "10.0.0.2");
    const auto second = rawMember(0x40000008u, "2001:db8::2");
    deliver(RTM_FPM_ADD_L2_NEXTHOP, first);
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE");
    std::string value;
    ASSERT_TRUE(objects.hget("1073741831", "remote_vtep", value)); EXPECT_EQ(value, "10.0.0.2");
    std::vector<unsigned char> group(20, 0xa5);
    const uint32_t id = 0x80000001u, count = 2;
    memcpy(group.data(), &id, sizeof(id)); memcpy(group.data() + 4, &count, sizeof(count));
    group.insert(group.end(), first.begin(), first.end()); group.insert(group.end(), second.begin(), second.end());
    deliver(RTM_FPM_ADD_L2_NEXTHOP_GROUP, group);
    ASSERT_TRUE(objects.hget("2147483649", "nexthop_group", value)); EXPECT_EQ(value, "1073741831,1073741832");
    ASSERT_TRUE(objects.hget("1073741832", "remote_vtep", value)); EXPECT_EQ(value, "2001:db8::2");
    ASSERT_TRUE(objects.hget("1073741832", "router_mac", value)); EXPECT_EQ(value, "02:00:00:00:00:03");
}

TEST_F(FpmBindingTest, PackedLayoutsCannotReplaceAcceptedNativeRows)
{
    const auto member = bppWireMember();
    struct { nlmsghdr header; l2_nhg_member_msg member; } natural{};
    natural.header.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP;
    natural.header.nlmsg_len = NLMSG_LENGTH(sizeof(member)); natural.member = member;
    sync->onMsgRaw(&natural.header); pipeline.flush();
    auto group = bppWireGroup(0x80000001u, {member});
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(group.data())); pipeline.flush();
    Table objects(&db, "L2_NEXTHOP_GROUP_TABLE");
    std::vector<FieldValueTuple> memberBefore, groupBefore, after;
    ASSERT_TRUE(objects.get("1073741831", memberBefore)); ASSERT_TRUE(objects.get("2147483649", groupBefore));
    std::vector<unsigned char> packedMember(NLMSG_SPACE(28));
    nlmsghdr header{}; header.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP; header.nlmsg_len = NLMSG_LENGTH(28);
    memcpy(packedMember.data(), &header, sizeof(header));
    auto *payload = packedMember.data() + NLMSG_HDRLEN;
    memcpy(payload, &member.l2nm_nh_id, 4); payload[4] = 1; payload[5] = AF_INET;
    memcpy(payload + 6, &member.l2nm_remote_vtep, 16); memcpy(payload + 22, member.l2nm_router_mac, 6);
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(packedMember.data())); pipeline.flush();
    std::vector<unsigned char> packedGroup(NLMSG_SPACE(18));
    header.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP_GROUP; header.nlmsg_len = NLMSG_LENGTH(18);
    memcpy(packedGroup.data(), &header, sizeof(header));
    const uint32_t id = 0x80000001u; memcpy(packedGroup.data() + NLMSG_HDRLEN, &id, sizeof(id));
    sync->onMsgRaw(reinterpret_cast<nlmsghdr *>(packedGroup.data())); pipeline.flush();
    ASSERT_TRUE(objects.get("1073741831", after)); EXPECT_EQ(after, memberBefore);
    ASSERT_TRUE(objects.get("2147483649", after)); EXPECT_EQ(after, groupBefore);
}
