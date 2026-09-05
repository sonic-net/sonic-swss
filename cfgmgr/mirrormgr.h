#ifndef __MIRRORMGR__
#define __MIRRORMGR__

#include "dbconnector.h"
#include "orch.h"

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

/* Session types */
#define MIRROR_SESSION_TYPE_SPAN           "SPAN"
#define MIRROR_SESSION_TYPE_ERSPAN         "ERSPAN"

namespace swss {

/*
 * MirrorMgr — switchdev mirror-session manager.
 *
 * Reads CONFIG_DB MIRROR_SESSION and programs the kernel directly via tc:
 *   - SPAN   (local):  `tc filter ... matchall action mirred egress mirror dev <dst>`
 *   - ERSPAN (remote): `tc filter ... matchall action tunnel_key set ... action
 *                      mirred egress mirror tunnel_key dev <monitor>`
 *
 * Status is written back to STATE_DB MIRROR_SESSION_TABLE. There is no APP_DB
 * or orchagent hand-off; the kernel is the dataplane (switchdev mode).
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

    /* Internal tracking: session_name -> set of tc filter handles/prios */
    std::set<std::string> m_programmedSessions;
    std::map<std::string, std::string> m_sessionSrcPort;   // session -> src port(s)

    void doTask(Consumer &consumer);
    void doMirrorSessionTask(Consumer &consumer);

    /* Kernel programming helpers */
    bool addSpanSession(const std::string &srcPort,
                        const std::string &dstPort);
    bool addErspanSession(const std::string &srcPort,
                          const std::string &srcIp,
                          const std::string &dstIp,
                          const std::string &dstPort);
    bool removeMirrorSession(const std::string &srcPort);
};

} // namespace swss

#endif /* __MIRRORMGR__ */
