#!/usr/bin/env python

""""
Description: bgp_eoiu_marker.py -- populating bgp eoiu marker flags in stateDB during warm reboot.
    The script is started by supervisord in bgp docker when the docker is started.
    It does not do anything in case neither system nor bgp warm restart is enabled.

    The script check bgp neighbor state via vtysh cli interface periodically (every 3 second).
    It looks for the explicit EOR in the json output of show bgp neighbors A.B.C.D json.
    FRR only reports an explicit EOR for a neighbor which negotiated the graceful restart
    capability, so for the neighbors with graceful restart disabled on either end the first
    Keepalive received after the session reached Established is used as an implicit EOR instead.

    Once the script has collected all needed EORs, it sets EOIU flags in stateDB.

    fpmsyncd may hold upto 'eoiu_hold_timer` (default 3 seconds) after getting the flag before starting routing reconciliation.
    For any reason the script fails to set EOIU flag in stateDB, the current warm_restart bgp_timer will kick in later.
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

    # every 3 seconds to check bgp neighbors state
    CHECK_INTERVAL = 3

    def __init__(self):
        self.ipv4_neigh_count = 0
        self.ipv4_neighbors = []
        self.ipv4_neigh_eor_status = {}
        self.ipv6_neigh_count = 0
        self.ipv6_neighbors = []
        self.ipv6_neigh_eor_status = {}
        self.keepalivesRecvCnt = {}
        self.bgp_ipv4_eoiu = False
        self.bgp_ipv6_eoiu = False
        self.get_peers_wt = self.DEF_TIME_OUT

    def get_all_peers(self):
        empty_logged = False
        while self.get_peers_wt >= 0:
            ipv4_neigh_count = 0
            ipv6_neigh_count = 0

            try:
                cmd = "vtysh -c 'show bgp summary json'"
                output = commands.getoutput(cmd)
                peer_info = json.loads(output)
                if "ipv4Unicast" in peer_info and "peers" in peer_info["ipv4Unicast"]:
                    self.ipv4_neighbors = peer_info["ipv4Unicast"]["peers"].keys()
                    ipv4_neigh_count = len(self.ipv4_neighbors)

                if "ipv6Unicast" in peer_info and "peers" in peer_info["ipv6Unicast"]:
                    self.ipv6_neighbors = peer_info["ipv6Unicast"]["peers"].keys()
                    ipv6_neigh_count = len(self.ipv6_neighbors)

                # bgp answers with an empty object until it has instantiated its
                # neighbors, and that is a successful reply rather than an error.
                # Returning with an empty/partial neighbor list can make eoiu check
                # wrongly succeed and signal eoiu before all routes were replayed.
                # So keep polling until the neighbor count is stable.
                if ipv4_neigh_count > 0 or ipv6_neigh_count > 0:
                    if ipv4_neigh_count == self.ipv4_neigh_count and ipv6_neigh_count == self.ipv6_neigh_count:
                        syslog.syslog('BGP ipv4 neighbors ({}): {}'.format(self.ipv4_neigh_count, self.ipv4_neighbors))
                        syslog.syslog('BGP ipv6 neighbors ({}): {}'.format(self.ipv6_neigh_count, self.ipv6_neighbors))
                        return
                else:
                    if not empty_logged:
                        syslog.syslog('BGP neighbor list is empty, waiting for bgp to instantiate its neighbors')
                        empty_logged = True

                self.ipv4_neigh_count = ipv4_neigh_count
                self.ipv6_neigh_count = ipv6_neigh_count

                time.sleep(5)
                self.get_peers_wt -= 5
                continue

            except Exception:
                syslog.syslog(syslog.LOG_ERR, "*ERROR* get_all_peers Exception: %s" % (traceback.format_exc()))
                time.sleep(5)
                self.get_peers_wt -= 5
                self.get_all_peers()
        syslog.syslog(syslog.LOG_ERR, "Failed to get a non-empty stable bgp neighbor list in {} seconds, exiting".format(self.DEF_TIME_OUT));
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
        db = swsscommon.SonicV2Connector(use_unix_socket_path=True)
        db.connect(db.STATE_DB, False)
        key = "BGP_STATE_TABLE|%s|eoiu" % family
        db.set(db.STATE_DB, key, 'state', state)
        timesamp = strftime("%Y-%m-%d %H:%M:%S", gmtime())
        db.set(db.STATE_DB, key, 'timestamp', timesamp)
        db.close(db.STATE_DB)
        return

    def clean_bgp_eoiu_marker(self):
        db = swsscommon.SonicV2Connector(use_unix_socket_path=True)
        db.connect(db.STATE_DB, False)
        db.delete(db.STATE_DB, "BGP_STATE_TABLE|IPv4|eoiu")
        db.delete(db.STATE_DB, "BGP_STATE_TABLE|IPv6|eoiu")
        db.close(db.STATE_DB)
        syslog.syslog('Cleaned ipv4 and ipv6 eoiu marker flags')
        return

    # FRR only reports an explicit End-of-RIB (gracefulRestartInfo.endOfRibRecv) for a
    # neighbor which negotiated the graceful restart capability. When graceful restart is
    # disabled by the peer (remoteGrMode "Disable") or on our side (then we never send the
    # capability, so remoteGrMode reads "NotApplicable" and localGrMode is "Disable", or
    # "Disable*" when the neighbor inherits the global mode), no explicit EOR will ever
    # arrive and the implicit EOR is the only signal available.
    def bgp_gr_disabled(self, gr_info):
        if "remoteGrMode" in gr_info and gr_info["remoteGrMode"] == "Disable":
            return True
        if "localGrMode" in gr_info and gr_info["localGrMode"] in ("Disable", "Disable*"):
            return True
        return False

    def bgp_eor_received(self, neigh, is_ipv4):
        try:
            neighstr = "%s" % neigh
            eor_received = False
            gr_info = {}
            cmd = "vtysh -c 'show bgp neighbors %s json'" % neighstr
            output = commands.getoutput(cmd)
            neig_status = json.loads(output)
            if neighstr in neig_status:
                if "gracefulRestartInfo" in neig_status[neighstr]:
                    gr_info = neig_status[neighstr]["gracefulRestartInfo"]
                    if "endOfRibRecv" in gr_info:
                        eor_info = gr_info["endOfRibRecv"]
                        if is_ipv4 and "ipv4Unicast" in eor_info and eor_info["ipv4Unicast"] == True:
                            eor_received = True
                        elif not is_ipv4 and "ipv6Unicast" in eor_info and eor_info["ipv6Unicast"] == True:
                            eor_received = True
                if eor_received:
                    syslog.syslog('BGP eor received for neighbors: {}'.format(neigh))

                # No explict eor. Only neighbors with graceful restart disabled on either
                # end can use the implicit eor, everyone else must send a real EOR.
                if eor_received == False and self.bgp_gr_disabled(gr_info) and \
                   "bgpState" in neig_status[neighstr] and neig_status[neighstr]["bgpState"] == "Established":
                    # record the keepalivesRecv count on the first poll, then look for a change
                    if neighstr not in self.keepalivesRecvCnt:
                        syslog.syslog('BGP graceful restart not negotiated for neighbor {}, using implicit eor'.format(neigh))
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
    # EOR is normally the explicit EOR (End-Of-RIB) reported by FRR in gracefulRestartInfo.
    # FRR only reports it for the neighbors which negotiated the graceful restart capability,
    # so for the neighbors with graceful restart disabled on either end (remoteGrMode
    # "Disable", or localGrMode "Disable"/"Disable*" when it is off on our side) the first
    # keep-alive after BGP has reached Established is considered an implicit-EOR instead.
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
