#pragma once

#include <deque>
#include <string>
#include <unordered_map>

#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "response_publisher_interface.h"
#include "return_code.h"
#include "swss/ipaddress.h"
#include "swss/macaddress.h"
#include "swss/rediscommand.h"
extern "C"
{
#include "sai.h"
}

#define MIRROR_SESSION_DEFAULT_IP_HDR_VER 4
#define GRE_PROTOCOL_ERSPAN 0x88be
#define MIRROR_SESSION_DEFAULT_VLAN_TPID 0x8100

namespace p4orch
{
namespace test
{
class MirrorSessionManagerTest;
} // namespace test

struct P4MirrorSessionEntry
{
    P4MirrorSessionEntry(const std::string &mirror_session_key, sai_object_id_t mirror_session_oid,
                         const std::string &mirror_session_id,
                         const std::string& action, const std::string& port,
                         const swss::IpAddress& src_ip,
                         const swss::IpAddress &dst_ip, const swss::MacAddress &src_mac,
                         const swss::MacAddress &dst_mac, uint8_t ttl,
                         uint8_t tos, const std::string& failover_port,
                         const uint16_t vlan_id, const uint16_t udp_src_port,
                         const uint16_t udp_dst_port)
        : mirror_session_key(mirror_session_key), mirror_session_oid(mirror_session_oid),
          mirror_session_id(mirror_session_id),
          action(action),
          port(port), src_ip(src_ip), dst_ip(dst_ip), src_mac(src_mac),
          dst_mac(dst_mac), ttl(ttl),
          tos(tos),
          failover_port(failover_port),
          vlan_id(vlan_id),
          udp_src_port(udp_src_port),
          udp_dst_port(udp_dst_port) {}

    P4MirrorSessionEntry(const P4MirrorSessionEntry &) = default;

    bool operator==(const P4MirrorSessionEntry &entry) const
    {
        return mirror_session_key == entry.mirror_session_key && mirror_session_oid == entry.mirror_session_oid &&
               mirror_session_id == entry.mirror_session_id && port == entry.port && src_ip == entry.src_ip &&
               dst_ip == entry.dst_ip && src_mac == entry.src_mac && dst_mac == entry.dst_mac &&
               ttl == entry.ttl && tos == entry.tos &&
               failover_port == entry.failover_port && vlan_id == entry.vlan_id &&
               udp_src_port == entry.udp_src_port &&
               udp_dst_port == entry.udp_dst_port;
    }

    std::string mirror_session_key;

    // SAI OID associated with this entry.
    sai_object_id_t mirror_session_oid = 0;

    // Match field in table
    std::string mirror_session_id;
    std::string action;
    // Action parameters
    std::string port;
    swss::IpAddress src_ip;
    swss::IpAddress dst_ip;
    swss::MacAddress src_mac;
    swss::MacAddress dst_mac;
    uint8_t ttl = 0;
    uint8_t tos = 0;
    // New fields for mirror_with_vlan_tag_and_ipfix_encapsulation
    std::string failover_port;
    uint16_t vlan_id = 0;
    uint16_t udp_src_port = 0;
    uint16_t udp_dst_port = 0;
};

// MirrorSessionManager is responsible for programming mirror session intents in
// APPL_DB:FIXED_MIRROR_SESSION_TABLE to ASIC_DB.
class MirrorSessionManager : public ObjectManagerInterface
{
  public:
    MirrorSessionManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;

    ReturnCode drain() override;

    void drainWithNotExecuted() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;

    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

  private:
    ReturnCodeOr<P4MirrorSessionAppDbEntry> deserializeP4MirrorSessionAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    P4MirrorSessionEntry *getMirrorSessionEntry(const std::string &mirror_session_key);

    ReturnCode validateMirrorSessionEntry(
      const P4MirrorSessionAppDbEntry& mirror_session_entry,
      const std::string& operation);
    ReturnCode validateSetMirrorSessionEntry(
      const P4MirrorSessionAppDbEntry& mirror_session_entry);
    ReturnCode validateDelMirrorSessionEntry(
      const P4MirrorSessionAppDbEntry& mirror_session_entry);

    ReturnCode processAddRequest(const P4MirrorSessionAppDbEntry &app_db_entry);
    ReturnCode createMirrorSession(P4MirrorSessionEntry mirror_session_entry);

    ReturnCode processUpdateRequest(const P4MirrorSessionAppDbEntry &app_db_entry,
                                    P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setPort(const std::string &new_port, P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setSrcIp(const swss::IpAddress &new_src_ip, P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setDstIp(const swss::IpAddress &new_dst_ip, P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setSrcMac(const swss::MacAddress &new_src_mac, P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setDstMac(const swss::MacAddress &new_dst_mac, P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setTtl(uint8_t new_ttl, P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setTos(uint8_t new_tos, P4MirrorSessionEntry *existing_mirror_session_entry);
    ReturnCode setVlanId(uint16_t new_vlan_id,
                         P4MirrorSessionEntry* existing_mirror_session_entry);
    ReturnCode setUdpSrcPort(uint16_t new_udp_src_port,
                             P4MirrorSessionEntry* existing_mirror_session_entry);
    ReturnCode setUdpDstPort(uint16_t new_udp_dst_port,
                             P4MirrorSessionEntry* existing_mirror_session_entry);
    ReturnCode setMirrorSessionEntry(const P4MirrorSessionEntry &intent_mirror_session_entry,
                                     P4MirrorSessionEntry *existing_mirror_session_entry);

    ReturnCode processDeleteRequest(const std::string &mirror_session_key);

    // state verification DB helper functions. Return err string or empty string.
    std::string verifyStateCache(const P4MirrorSessionAppDbEntry &app_db_entry,
                                 const P4MirrorSessionEntry *mirror_session_entry);
    std::string verifyStateAsicDb(const P4MirrorSessionEntry *mirror_session_entry);

    std::unordered_map<std::string, P4MirrorSessionEntry> m_mirrorSessionTable;

    // Owners of pointers below must outlive this class's instance.
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    // For test purpose only
    friend class p4orch::test::MirrorSessionManagerTest;
};

} // namespace p4orch
