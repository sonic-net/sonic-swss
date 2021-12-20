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

namespace p4orch
{
namespace test
{
class MirrorSessionManagerTest;
} // namespace test

struct P4MirrorSessionEntry
{
    P4MirrorSessionEntry(const std::string &mirror_session_key, sai_object_id_t mirror_session_oid,
                         const std::string &mirror_session_id, const std::string &port, const swss::IpAddress &src_ip,
                         const swss::IpAddress &dst_ip, const swss::MacAddress &src_mac,
                         const swss::MacAddress &dst_mac, uint8_t ttl, uint8_t tos)
        : mirror_session_key(mirror_session_key), mirror_session_oid(mirror_session_oid),
          mirror_session_id(mirror_session_id), port(port), src_ip(src_ip), dst_ip(dst_ip), src_mac(src_mac),
          dst_mac(dst_mac), ttl(ttl), tos(tos)
    {
    }

    P4MirrorSessionEntry(const P4MirrorSessionEntry &) = default;

    bool operator==(const P4MirrorSessionEntry &entry) const
    {
        return mirror_session_key == entry.mirror_session_key && mirror_session_oid == entry.mirror_session_oid &&
               mirror_session_id == entry.mirror_session_id && port == entry.port && src_ip == entry.src_ip &&
               dst_ip == entry.dst_ip && src_mac == entry.src_mac && dst_mac == entry.dst_mac && ttl == entry.ttl &&
               tos == entry.tos;
    }

    std::string mirror_session_key;

    // SAI OID associated with this entry.
    sai_object_id_t mirror_session_oid = 0;

    // Match field in table
    std::string mirror_session_id;
    // Action parameters
    std::string port;
    swss::IpAddress src_ip;
    swss::IpAddress dst_ip;
    swss::MacAddress src_mac;
    swss::MacAddress dst_mac;
    uint8_t ttl = 0;
    uint8_t tos = 0;
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

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;

    void drain() override;

  private:
    ReturnCodeOr<P4MirrorSessionAppDbEntry> deserializeP4MirrorSessionAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    P4MirrorSessionEntry *getMirrorSessionEntry(const std::string &mirror_session_key);

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
    ReturnCode setMirrorSessionEntry(const P4MirrorSessionEntry &intent_mirror_session_entry,
                                     P4MirrorSessionEntry *existing_mirror_session_entry);

    ReturnCode processDeleteRequest(const std::string &mirror_session_key);

    std::unordered_map<std::string, P4MirrorSessionEntry> m_mirrorSessionTable;

    // Owners of pointers below must outlive this class's instance.
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    // For test purpose only
    friend class p4orch::test::MirrorSessionManagerTest;
};

} // namespace p4orch
