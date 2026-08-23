#ifndef __KERNUTIL__
#define __KERNUTIL__

#include <map>
#include <string>
#include <vector>

namespace swss {
class DBConnector;
}

/*
 * kernutil — shared kernel (tc/ip) helpers for the switchdev cfgmgr daemons
 * (mirrormgrd, aclmgrd, policermgrd). Each daemon programs the kernel directly
 * instead of going through SAI/orchagent; these helpers keep the tc command
 * construction consistent across daemons.
 */

namespace kernutil {

/*
 * resolveInterface — return the kernel interface name for a SONiC port name.
 * In switchdev mode the docker-sonic-vs container renames the front-panel
 * veths (eth1 -> Ethernet0, ...) at startup, so the kernel names already match
 * the SONiC names. PortChannel/bond expansion is done separately by
 * resolveSrcPorts().
 */
std::string resolveInterface(const std::string &sonicName);

/*
 * normalizeDirection — map the MIRROR_SESSION direction field to the canonical
 * RX/TX/BOTH values. Accepts upstream RX/TX/BOTH and the legacy ingress/egress/
 * both spellings, defaulting to RX (ingress) when absent or unknown.
 */
std::string normalizeDirection(const std::string &dir);

/*
 * dscpToTos — convert a DSCP value (6 bits) to the full TOS byte (dscp << 2)
 * that tc's ip_tos / tunnel metadata expects. Returns "" for empty/invalid.
 */
std::string dscpToTos(const std::string &dscp);

/*
 * packetActionToTc — map an ACL PACKET_ACTION (FORWARD/DROP) to a tc action
 * token (ok/drop). Defaults to drop.
 */
std::string packetActionToTc(const std::string &action);

/*
 * policerToTcPolice — build a tc "action police ..." string from a POLICER
 * config (field names match policermgr.h defines). Returns "" if the mandatory
 * CIR/CBS are missing (caller should mark the referencing session inactive).
 *
 * The police CONTROL is emitted as:
 *   conform-exceed <exceed>/<conform>
 * where "exceed" (red) defaults to drop and "conform" (green) defaults to pipe,
 * so that a mirror/ACL filter rate-limits: conforming packets continue to the
 * next action (mirror/drop), exceeding packets are dropped.
 */
std::string policerToTcPolice(const std::map<std::string, std::string> &policer);

/*
 * resolveSrcPorts — expand a comma-separated src_port list into kernel
 * interface names. PortChannel* entries are expanded to their member (lower_*)
 * interfaces. Returns false if a PortChannel has no members (empty LAG), so the
 * caller can mark the session inactive.
 */
bool resolveSrcPorts(const std::string &srcPortList, std::vector<std::string> &ifaces);

/*
 * resolveNextHopInterface — resolve the egress interface toward dstIp via
 * "ip route get <dstIp>". Returns "" on failure.
 */
std::string resolveNextHopInterface(const std::string &dstIp);

/*
 * maskToPrefixLen — convert a dotted-quad IPv4 netmask (e.g. 255.255.255.0)
 * to a prefix-length string "/24". Returns "" for empty/invalid.
 */
std::string maskToPrefixLen(const std::string &mask);

/*
 * matchIpTypeToTc — map an ACL IP_TYPE (ANY/IP/IPV4ANY/IPV6ANY/ARP/...) to a
 * tc flower ether_type token (e.g. "0x0800"). Returns "" when no match is
 * needed (ANY) or the type is unsupported.
 */
std::string matchIpTypeToTc(const std::string &ipType);

/*
 * peditSetDscpToTc — build a tc pedit action that rewrites the IPv4 TOS byte
 * to the given DSCP value. Returns "" for empty/invalid DSCP.
 */
std::string peditSetDscpToTc(const std::string &dscp);

/*
 * resolveMirrorMonitorPort — resolve a mirror session's monitor interface for
 * an ACL MIRROR_ACTION. Reads STATE_DB MIRROR_SESSION_TABLE monitor_port first,
 * then falls back to CONFIG_DB MIRROR_SESSION dst_port. Returns "" if unknown.
 */
std::string resolveMirrorMonitorPort(swss::DBConnector *cfgDb,
                                     swss::DBConnector *stateDb,
                                     const std::string &sessionName);

} // namespace kernutil

#endif /* __KERNUTIL__ */
