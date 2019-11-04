#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include "swssnet.h"
#include "cfgmgr/shellcmd.h"

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define DEFAULT_VLAN_ID     "1"

TEST(quoted, copy1_v6)
{
    ostringstream cmd;
    string key = "Ethernet0";
    cmd << "ip link set " << shellquote(key) << " down";
    EXPECT_EQ(cmd.str(), "ip link set \"Ethernet0\" down");

    ostringstream cmd2;
    key = "; rm -rf /; echo '\"'";
    cmd2 << "ip link set " << shellquote(key) << " down";
    EXPECT_EQ(cmd2.str(), "ip link set \"; rm -rf /; echo '\\\"'\" down");

    ostringstream cmds, inner;
    string port_alias = "etp1";
    string tagging_cmd = "pvid untagged";
    int vlan_id = 111;
    inner << IP_CMD " link set " << shellquote(port_alias) << " master " DOT1Q_BRIDGE_NAME " && "
      BRIDGE_CMD " vlan del vid " DEFAULT_VLAN_ID " dev " << shellquote(port_alias) << " && "
      BRIDGE_CMD " vlan add vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " " + tagging_cmd;
    cmds << BASH_CMD " -c " << shellquote(inner.str());
    EXPECT_EQ(cmds.str(), "/bin/bash -c \"/sbin/ip link set \\\"etp1\\\" master Bridge && /sbin/bridge vlan del vid 1 dev \\\"etp1\\\" && /sbin/bridge vlan add vid 111 dev \\\"etp1\\\" pvid untagged\"");

    ostringstream cmd4;
    key = "$(echo hi)";
    cmd4 << "cat /sys/class/net/" << shellquote(key) << "/operstate";
    EXPECT_EQ(cmd4.str(), "cat /sys/class/net/\"\\$(echo hi)\"/operstate");
}
