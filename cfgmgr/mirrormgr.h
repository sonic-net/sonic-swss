#ifndef __MIRRORMGR__
#define __MIRRORMGR__

#include "dbconnector.h"
#include "orch.h"

#include <cstdint>
#include <set>
#include <map>
#include <string>
#include <vector>

/*
 * CONFIG_DB / STATE_DB table name constants.
 * These are normally generated from YANG models during the full build, but may
 * not be available during isolated component builds. #ifndef guards let the
 * build-generated versions take precedence (same pattern as aclmgr.h).
 */
#ifndef CFG_MIRROR_SESSION_TABLE_NAME
#define CFG_MIRROR_SESSION_TABLE_NAME      "MIRROR_SESSION"
#endif
#ifndef STATE_MIRROR_SESSION_TABLE_NAME
#define STATE_MIRROR_SESSION_TABLE_NAME    "MIRROR_SESSION_TABLE"
#endif
#ifndef CFG_POLICER_TABLE_NAME
#define CFG_POLICER_TABLE_NAME             "POLICER"
#endif
#ifndef STATE_POLICER_TABLE_NAME
#define STATE_POLICER_TABLE_NAME           "POLICER_TABLE"
#endif

/* MIRROR_SESSION field names from CONFIG_DB */
#define MIRROR_SESSION_FIELD_STATUS        "status"
#define MIRROR_SESSION_FIELD_TYPE          "type"
#define MIRROR_SESSION_FIELD_SRC_IP        "src_ip"
#define MIRROR_SESSION_FIELD_DST_IP        "dst_ip"
#define MIRROR_SESSION_FIELD_GRE_TYPE      "gre_type"
#define MIRROR_SESSION_FIELD_DSCP          "dscp"
#define MIRROR_SESSION_FIELD_TTL           "ttl"
#define MIRROR_SESSION_FIELD_SRC_PORT      "src_port"
#define MIRROR_SESSION_FIELD_DST_PORT      "dst_port"
#define MIRROR_SESSION_FIELD_DIRECTION     "direction"
#define MIRROR_SESSION_FIELD_POLICER       "policer"
#define MIRROR_SESSION_FIELD_SAMPLE_RATE   "sample_rate"

/* Session types */
#define MIRROR_SESSION_TYPE_SPAN           "SPAN"
#define MIRROR_SESSION_TYPE_ERSPAN         "ERSPAN"

/* Mirror direction values (normalized; upstream SONiC uses RX/TX/BOTH) */
#define MIRROR_RX_DIRECTION     "RX"
#define MIRROR_TX_DIRECTION     "TX"
#define MIRROR_BOTH_DIRECTION   "BOTH"

/* Default GRE protocol type, matching MirrorOrch (0x88be). */
#define MIRROR_SESSION_DEFAULT_GRE_TYPE     "0x88be"

namespace swss {

/*
 * MirrorMgr — switchdev mirror-session manager.
 *
 * Reads CONFIG_DB MIRROR_SESSION and programs the kernel directly via tc:
 *   - SPAN   (local):  `tc filter ... matchall action mirred egress mirror dev <dst>`
 *   - ERSPAN (remote): `tc filter ... matchall action tunnel_key set ... action
 *                      mirred egress mirror tunnel_key dev <monitor>`
 *
 * Optional fields: policer (tc police), gre_type (ERSPAN GRE key), LAG source
 * ports (expanded to member interfaces), and ERSPAN next-hop resolution.
 * Sampled mirroring (sample_rate) is not supported and marks the session
 * inactive. Status is written back to STATE_DB MIRROR_SESSION_TABLE. There is
 * no APP_DB or orchagent hand-off; the kernel is the dataplane (switchdev).
 */
class MirrorMgr : public Orch
{
public:
    MirrorMgr(DBConnector *cfgDb, DBConnector *stateDb,
              const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    /* STATE_DB table for writing session status */
    Table m_stateMirrorSessionTable;

    /* POLICER tables for resolving a referenced policer */
    Table m_cfgPolicerTable;
    Table m_statePolicerTable;

    /* Internal tracking: session_name -> set of tc filter handles/prios */
    std::set<std::string> m_programmedSessions;
    std::map<std::string, std::string> m_sessionSrcPort;             // session -> src port(s)
    std::map<std::string, std::vector<std::string>> m_sessionIfaces; // session -> resolved ifaces
    std::map<std::string, uint32_t> m_sessionPrio;                   // session -> tc filter prio
    uint32_t m_nextPrio = 100;                                       // next prio to assign

    void doTask(Consumer &consumer);
    void doMirrorSessionTask(Consumer &consumer);

    /* Resolve a referenced policer into a tc "action police ..." string. */
    bool getPolicerPoliceAction(const std::string &policerName, std::string &policeAction);

    /* Kernel programming helpers */
    bool addSpanSession(const std::string &srcPort,
                        const std::string &dstPort,
                        const std::string &direction,
                        const std::string &policeAction,
                        uint32_t prio);
    bool addErspanSession(const std::string &srcPort,
                          const std::string &srcIp,
                          const std::string &dstIp,
                          const std::string &dstPort,
                          const std::string &greType,
                          const std::string &dscp,
                          const std::string &ttl,
                          const std::string &direction,
                          const std::string &policeAction,
                          uint32_t prio);
    bool removeMirrorSession(const std::string &srcPort,
                             uint32_t prio);

    /* Low-level tc helpers */
    void ensureClsact(const std::string &srcPort);
    bool addTcMirrorFilter(const std::string &srcPort, const std::string &hook,
                           uint32_t prio, const std::string &actionTail);
    bool removeTcMirrorFilter(const std::string &srcPort, const std::string &hook,
                              uint32_t prio);
};

} // namespace swss

#endif /* __MIRRORMGR__ */
