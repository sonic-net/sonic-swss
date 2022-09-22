#!/usr/bin/env python

""""
Description: bgp_eoiu_marker.py -- populating bgp eoiu marker flags in stateDB during warm reboot.
    The script is started by supervisord in bgp docker when the docker is started.
    It does not do anything in case neither system nor bgp warm restart is enabled.

    The script check bgp neighbor state via vtysh cli interface periodically (every 1 second).
    It looks for explicit EOR and implicit EOR (keep alive after established) in the json output of show ip bgp neighbors A.B.C.D json

    Once the script has collected all needed EORs, it set a EOIU flags in stateDB.

    fpmsyncd may hold a few seconds (2~5 seconds) after getting the flag before starting routing reconciliation.
    2-5 seconds should be enough for all the route to be synced to fpmsyncd from bgp. If not, the system probably is already in wrong state.

    For any reason the script failed to set EOIU flag in stateDB, the current warm_restart bgp_timer will kick in later.
"""

import sys
import time
import syslog
import traceback
import commands
import json
from swsscommon import swsscommon
import errno
from time import gmtime, strftime

class BgpStateCheck():
    # timeout the restore process in 120 seconds if not finished
    # This is in consistent with the default timerout for bgp warm restart set in fpmsyncd

    DEF_TIME_OUT = 120

    # every 1 seconds to check bgp neighbors state
    CHECK_INTERVAL = 1
    def __init__(self):
        self.ipv4_neighbors = []
        self.ipv4_neigh_eor_status = {}
        self.ipv6_neighbors = []
        self.ipv6_neigh_eor_status = {}
        self.keepalivesRecvCnt = {}
        self.bgp_ipv4_eoiu = False
        self.bgp_ipv6_eoiu = False
        self.get_peers_wt = self.DEF_TIME_OUT

    def get_all_peers(self):
        while self.get_peers_wt >= 0:
            try:
                cmd = "vtysh -c 'show bgp summary json'"
                output = commands.getoutput(cmd)
                peer_info = json.loads(output)
                if "ipv4Unicast" in peer_info and "peers" in peer_info["ipv4Unicast"]:
                    self.ipv4_neighbors = peer_info["ipv4Unicast"]["peers"].keys()

                if "ipv6Unicast" in peer_info and "peers" in peer_info["ipv6Unicast"]:
                    self.ipv6_neighbors = peer_info["ipv6Unicast"]["peers"].keys()

                syslog.syslog('BGP ipv4 neighbors: {}'.format(self.ipv4_neighbors))
                syslog.syslog('BGP ipv4 neighbors: {}'.format(self.ipv6_neighbors))
                return

            except Exception:
                syslog.syslog(syslog.LOG_ERR, "*ERROR* get_all_peers Exception: %s" % (traceback.format_exc()))
                time.sleep(5)
                self.get_peers_wt -= 5
                self.get_all_peers()
        syslog.syslog(syslog.LOG_ERR, "Failed to get bgp neighbor info in {} seconds, exiting".format(self.DEF_TIME_OUT));
        sys.exit(1)

    def init_peers_eor_status(self):
        # init neigh eor status to unknown
        for neigh in self.ipv4_neighbors:
            self.ipv4_neigh_eor_status[neigh] = "unknown"
        for neigh in self.ipv6_neighbors:
            self.ipv6_neigh_eor_status[neigh] = "unknown"

    # Set the statedb "BGP_STATE_TABLE|eoiu", so fpmsyncd can get the bgp eoiu signal
    # Only two families: 'ipv4' and 'ipv6'
    # state is "unknown" / "reached" / "consumed"
    def set_bgp_eoiu_marker(self, family, state):
        db = swsscommon.SonicV2Connector(host='127.0.0.1')
        db.connect(db.STATE_DB, False)
        key = "BGP_STATE_TABLE|%s|eoiu" % family
        db.set(db.STATE_DB, key, 'state', state)
        timesamp = strftime("%Y-%m-%d %H:%M:%S", gmtime())
        db.set(db.STATE_DB, key, 'timestamp', timesamp)
        db.close(db.STATE_DB)
        return

    def clean_bgp_eoiu_marker(self):
        db = swsscommon.SonicV2Connector(host='127.0.0.1')
        db.connect(db.STATE_DB, False)
        db.delete(db.STATE_DB, "BGP_STATE_TABLE|IPv4|eoiu")
        db.delete(db.STATE_DB, "BGP_STATE_TABLE|IPv6|eoiu")
        db.close(db.STATE_DB)
        syslog.syslog('Cleaned ipv4 and ipv6 eoiu marker flags')
        return

    def bgp_eor_received(self, neigh, is_ipv4):
        try:
            neighstr = "%s" % neigh
            eor_received = False
            cmd = "vtysh -c 'show bgp neighbors %s json'" % neighstr
            output = commands.getoutput(cmd)
            neig_status = json.loads(output)
            if neighstr in neig_status:
                if "gracefulRestartInfo" in neig_status[neighstr]:
                    if "endOfRibRecv" in neig_status[neighstr]["gracefulRestartInfo"]:
                        eor_info = neig_status[neighstr]["gracefulRestartInfo"]["endOfRibRecv"]
                        if is_ipv4 and "IPv4 Unicast" in eor_info and eor_info["IPv4 Unicast"] == True:
                            eor_received = True
                        elif not is_ipv4 and "IPv6 Unicast" in eor_info and eor_info["IPv6 Unicast"] == True:
                            eor_received = True
                if eor_received:
                    syslog.syslog('BGP eor received for neighbors: {}'.format(neigh))

                # No explict eor, try implicit eor
                if eor_received == False and "bgpState" in neig_status[neighstr] and neig_status[neighstr]["bgpState"] == "Established":
                    # if "messageStats" in neig_status and "keepalivesRecv" in neig_status["messageStats"]:
                    # it looks we need to record the keepalivesRecv count for detecting count change
                    if neighstr not in self.keepalivesRecvCnt:
                        self.keepalivesRecvCnt[neighstr] = neig_status[neighstr]["messageStats"]["keepalivesRecv"]
                    else:
                        eor_received = (self.keepalivesRecvCnt[neighstr] is not neig_status[neighstr]["messageStats"]["keepalivesRecv"])
                        if eor_received:
                            syslog.syslog('BGP implicit eor received for neighbors: {}'.format(neigh))

            return eor_received

        except Exception:
            syslog.syslog(syslog.LOG_ERR, "*ERROR* bgp_eor_received Exception: %s" % (traceback.format_exc()))


    # This function is to collect eor state based on the saved ipv4_neigh_eor_status and ipv6_neigh_eor_status dictionaries
    # It iterates through the dictionary, and check whether the specific neighbor has EOR received.
    # EOR may be explicit EOR (End-Of-RIB) or an implicit-EOR.
    # The first keep-alive after BGP has reached Established is considered an implicit-EOR.
    #
    # ipv4 and ipv6 neighbors are processed separately.
    # Once all ipv4 neighbors have EOR received, bgp_ipv4_eoiu becomes True.
    # Once all ipv6 neighbors have EOR received, bgp_ipv6_eoiu becomes True.

    # The neighbor EoR states were checked in a loop with an interval (CHECK_INTERVAL)
    # The function will timeout in case eoiu states never meet the condition
    # after some time (DEF_TIME_OUT).
    def wait_for_bgp_eoiu(self):
        wait_time = self.DEF_TIME_OUT
        while wait_time >= 0:
            if not self.bgp_ipv4_eoiu:
                for neigh, eor_status in self.ipv4_neigh_eor_status.items():
                    if eor_status == "unknown" and self.bgp_eor_received(neigh, True):
                        self.ipv4_neigh_eor_status[neigh] = "rcvd"
                if "unknown" not in self.ipv4_neigh_eor_status.values():
                    self.bgp_ipv4_eoiu = True
                    syslog.syslog("BGP ipv4 eoiu reached")

            if not self.bgp_ipv6_eoiu:
                for neigh, eor_status in self.ipv6_neigh_eor_status.items():
                    if eor_status == "unknown" and self.bgp_eor_received(neigh, False):
                        self.ipv6_neigh_eor_status[neigh] = "rcvd"
                if "unknown" not in self.ipv6_neigh_eor_status.values():
                    self.bgp_ipv6_eoiu = True
                    syslog.syslog('BGP ipv6 eoiu reached')

            if self.bgp_ipv6_eoiu and self.bgp_ipv4_eoiu:
                break;
            time.sleep(self.CHECK_INTERVAL)
            wait_time -= self.CHECK_INTERVAL

        if not self.bgp_ipv6_eoiu:
            syslog.syslog(syslog.LOG_ERR, "BGP ipv6 eoiu not reached: {}".format(self.ipv6_neigh_eor_status));

        if not self.bgp_ipv4_eoiu:
            syslog.syslog(syslog.LOG_ERR, "BGP ipv4 eoiu not reached: {}".format(self.ipv4_neigh_eor_status));

def main():

    print "bgp_eoiu_marker service is started"

    try:
        bgp_state_check = BgpStateCheck()
    except Exception, e:
        syslog.syslog(syslog.LOG_ERR, "{}: error exit 1, reason {}".format(THIS_MODULE, str(e)))
        exit(1)

    # Always clean the eoiu marker in stateDB first
    bgp_state_check.clean_bgp_eoiu_marker()

    # Use warmstart python binding to check warmstart information
    warmstart = swsscommon.WarmStart()
    warmstart.initialize("bgp", "bgp")
    warmstart.checkWarmStart("bgp", "bgp", False)

    # if bgp or system warm reboot not enabled, don't run
    if not warmstart.isWarmStart():
        print "bgp_eoiu_marker service is skipped as warm restart not enabled"
        return

    bgp_state_check.set_bgp_eoiu_marker("IPv4", "unknown")
    bgp_state_check.set_bgp_eoiu_marker("IPv6", "unknown")
    bgp_state_check.get_all_peers()
    bgp_state_check.init_peers_eor_status()
    try:
        bgp_state_check.wait_for_bgp_eoiu()
    except Exception as e:
        syslog.syslog(syslog.LOG_ERR, str(e))
        sys.exit(1)

    # set statedb to signal other processes like fpmsynd
    if bgp_state_check.bgp_ipv4_eoiu:
        bgp_state_check.set_bgp_eoiu_marker("IPv4", "reached")
    if bgp_state_check.bgp_ipv6_eoiu:
        bgp_state_check.set_bgp_eoiu_marker("IPv6", "reached")

    print "bgp_eoiu_marker service is done"
    return

if __name__ == '__main__':
    main()
