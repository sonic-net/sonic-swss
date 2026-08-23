#include <arpa/inet.h>
#include <dirent.h>
#include <sstream>

#include "kernutil.h"
#include "logger.h"
#include "tokenize.h"
#include "exec.h"
#include "dbconnector.h"
#include "table.h"
#include "converter.h"
#include "schema.h"

using namespace std;
using namespace swss;

#define IP_CMD "/sbin/ip"

namespace kernutil {

static string getField(const map<string, string> &fields, const string &key)
{
    auto it = fields.find(key);
    return (it != fields.end()) ? it->second : "";
}

string resolveInterface(const string &sonicName)
{
    return sonicName;
}

string normalizeDirection(const string &dir)
{
    if (dir == "RX" || dir == "rx" || dir == "INGRESS" || dir == "ingress")
        return "RX";
    if (dir == "TX" || dir == "tx" || dir == "EGRESS" || dir == "egress")
        return "TX";
    if (dir == "BOTH" || dir == "both")
        return "BOTH";
    if (dir.empty())
        return "RX";

    SWSS_LOG_WARN("Unknown mirror direction '%s', defaulting to RX", dir.c_str());
    return "RX";
}

string dscpToTos(const string &dscp)
{
    if (dscp.empty())
        return "";

    try
    {
        uint8_t d = (uint8_t)stoi(dscp);
        return to_string((uint32_t)d << 2);
    }
    catch (...)
    {
        SWSS_LOG_WARN("Invalid DSCP value: %s", dscp.c_str());
        return "";
    }
}

string packetActionToTc(const string &action)
{
    if (action == "FORWARD")
        return "ok";
    if (action == "DROP")
        return "drop";
    if (!action.empty())
        SWSS_LOG_WARN("Unknown PACKET_ACTION '%s', defaulting to drop", action.c_str());
    return "drop";
}

/*
 * policeActionControl — map a SONiC policer packet action to a tc police
 * CONTROL token. "forward"/"copy"/empty keep the packet flowing (pipe to the
 * next action); "drop" drops it.
 */
static string policeActionControl(const string &action)
{
    if (action == "drop")
        return "drop";
    if (action == "forward" || action == "copy" || action.empty())
        return "pipe";

    SWSS_LOG_WARN("Unknown policer action '%s', defaulting to pipe", action.c_str());
    return "pipe";
}

string policerToTcPolice(const map<string, string> &policer)
{
    string cir = getField(policer, "cir");
    string cbs = getField(policer, "cbs");
    if (cir.empty() || cbs.empty())
        return "";

    ostringstream os;
    os << "action police rate " << cir << " burst " << cbs;

    /* Two-rate (tr_tcm) policers add a peak rate + peak burst (mtu). */
    if (getField(policer, "mode") == "tr_tcm")
    {
        string pir = getField(policer, "pir");
        string pbs = getField(policer, "pbs");
        if (!pir.empty() && !pbs.empty())
            os << " peakrate " << pir << " mtu " << pbs;
    }

    /* conform-exceed <exceed>/<conform>: red (exceed) first, green (conform)
     * second. Green defaults to pipe (continue to mirror), red to drop. */
    string red = policeActionControl(getField(policer, "red_packet_action"));
    string green = policeActionControl(getField(policer, "green_packet_action"));

    /* policeActionControl returns "pipe" for empty, but empty green means the
     * default (pipe) and empty red means the default (drop) — fix red's default. */
    if (getField(policer, "red_packet_action").empty())
        red = "drop";

    os << " conform-exceed " << red << "/" << green;

    return os.str();
}

/*
 * getLagMembers — enumerate a bond/team master's member interfaces via its
 * /sys/class/net/<master>/lower_* symlinks. Returns false if the device does
 * not exist; an empty member list means an empty (unusable) LAG.
 */
static bool getLagMembers(const string &lag, vector<string> &members)
{
    string path = "/sys/class/net/" + lag;
    DIR *dir = opendir(path.c_str());
    if (!dir)
        return false;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        string name = ent->d_name;
        if (name.rfind("lower_", 0) == 0)
            members.push_back(name.substr(6));
    }
    closedir(dir);

    return true;
}

bool resolveSrcPorts(const string &srcPortList, vector<string> &ifaces)
{
    for (auto &port : swss::tokenize(srcPortList, ','))
    {
        if (port.rfind("PortChannel", 0) == 0)
        {
            vector<string> members;
            if (!getLagMembers(port, members) || members.empty())
                return false;
            ifaces.insert(ifaces.end(), members.begin(), members.end());
        }
        else
        {
            ifaces.push_back(port);
        }
    }

    return true;
}

string resolveNextHopInterface(const string &dstIp)
{
    ostringstream cmd;
    cmd << IP_CMD << " route get " << dstIp;

    string res;
    if (swss::exec(cmd.str(), res) != 0)
        return "";

    istringstream iss(res);
    string tok;
    while (iss >> tok)
    {
        if (tok == "dev")
        {
            string iface;
            iss >> iface;
            return iface;
        }
    }

    return "";
}

string maskToPrefixLen(const string &mask)
{
    if (mask.empty())
        return "";

    struct in_addr addr;
    if (inet_pton(AF_INET, mask.c_str(), &addr) != 1)
    {
        SWSS_LOG_WARN("Invalid IPv4 netmask '%s'", mask.c_str());
        return "";
    }

    uint32_t m = ntohl(addr.s_addr);
    uint32_t bits = 0;
    while (m & 0x80000000)
    {
        bits++;
        m <<= 1;
    }

    return "/" + to_string(bits);
}

string matchIpTypeToTc(const string &ipType)
{
    if (ipType == "IP" || ipType == "IPV4ANY")
        return "0x0800";
    if (ipType == "IPV6ANY")
        return "0x86dd";
    if (ipType == "ARP")
        return "0x0806";
    if (ipType == "ANY" || ipType.empty())
        return "";

    SWSS_LOG_WARN("Unsupported IP_TYPE '%s', ignoring", ipType.c_str());
    return "";
}

string peditSetDscpToTc(const string &dscp)
{
    string tos = dscpToTos(dscp);
    if (tos.empty())
        return "";

    return "action pedit ex munge ip tos set " + tos;
}

string resolveMirrorMonitorPort(DBConnector *cfgDb, DBConnector *stateDb,
                                const string &sessionName)
{
    vector<FieldValueTuple> fvs;

    /* STATE_DB MIRROR_SESSION_TABLE monitor_port (resolved by mirrormgrd). */
    Table stateTable(stateDb, "MIRROR_SESSION_TABLE");
    if (stateTable.get(sessionName, fvs))
    {
        for (auto &fv : fvs)
            if (fvField(fv) == "monitor_port" && !fvValue(fv).empty())
                return fvValue(fv);
    }

    /* Fall back to CONFIG_DB MIRROR_SESSION dst_port (SPAN). */
    fvs.clear();
    Table cfgTable(cfgDb, "MIRROR_SESSION");
    if (cfgTable.get(sessionName, fvs))
    {
        for (auto &fv : fvs)
            if (fvField(fv) == "dst_port" && !fvValue(fv).empty())
                return fvValue(fv);
    }

    return "";
}

} // namespace kernutil
