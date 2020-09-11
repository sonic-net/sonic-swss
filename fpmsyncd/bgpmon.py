#!/usr/bin/env python2

""""
Description: bgpmon.py -- populating bgp related information in stateDB.
    script is started by supervisord in bgp docker when the docker is started.

    Initial creation of this daemon is to assist SNMP agent in obtaining the 
    BGP related information for its MIB support. The MIB that this daemon is
    assiting is for the CiscoBgp4MIB (Neighbor state only). If there are other
    BGP related items that needs to be updated in a periodic manner in the 
    future, then more can be added into this process.

    The script check if there are any bgp activities by monitoring the bgp
    frr.log file timestamp.  If activity is detected, then it will request bgp
    neighbor state via vtysh cli interface. This bgp activity monitoring is 
    done periodically (every 15 second). When triggered, it looks specifically
    for the neighbor state in the json output of show ip bgp neighbors json
    and update the state DB for each neighbor accordingly.
    In order to not disturb and hold on to the State DB access too long and
    removal of the stale neighbors (neighbors that was there previously on 
    previous get request but no longer there in the current get request), a
    "previous" neighbor dictionary will be kept and used to determine if there
    is a need to perform update or the peer is stale to be removed from the
    state DB
"""

import commands
import json
import os
import syslog
import swsssdk
import time
import traceback

class BgpStateGet():
    def __init__(self):
        # list peer_l stores the Neighbor peer Ip address
        # dic peer_state stores the Neighbor peer state entries
        # list new_peer_l stores the new snapshot of Neighbor peer ip address
        # dic new_peer_state stores the new snapshot of Neighbor peer states 
        self.peer_l = []
        self.peer_state = {}
        self.new_peer_l = []
        self.new_peer_state = {}
        self.cached_timestamp = 0
        self.db = swsssdk.SonicV2Connector()
        self.db.connect(self.db.STATE_DB, False)
        self.db.delete_all_by_pattern(self.db.STATE_DB, "NEIGH_STATE_TABLE|*" )

    # A quick way to check if there are anything happening within BGP is to
    # check its log file has any activities. This is by checking its modified
    # timestamp against the cached timestamp that we keep and if there is a
    # difference, there is activity detected. In case the log file got wiped
    # out, it will default back to constant pulling every 15 seconds
    def bgp_activity_detected(self):
        try:
            timestamp = os.stat("/var/log/frr/frr.log").st_mtime
            if timestamp != self.cached_timestamp:
                self.cached_timestamp = timestamp
                return True
            else:
                return False
        except Exception, e:
            return True

    # Get a new snapshot of BGP neighbors and store them in the "new" location
    def get_all_neigh_states(self):
        ipv6_peer_l = []
        try:
            cmd = "vtysh -c 'show bgp summary json'"
            output = commands.getoutput(cmd)
            peer_info = json.loads(output)
            # no exception, safe to Clean the "new" lists/dic for new sanpshot
            del self.new_peer_l[:]
            self.new_peer_state.clear()
            if "ipv4Unicast" in peer_info and "peers" in peer_info["ipv4Unicast"]:
                self.new_peer_l = peer_info["ipv4Unicast"]["peers"].keys()
                for i in range (0, len(self.new_peer_l)):
                    self.new_peer_state[self.new_peer_l[i]] = \
                    peer_info["ipv4Unicast"]["peers"][self.new_peer_l[i]]["state"]

            if "ipv6Unicast" in peer_info and "peers" in peer_info["ipv6Unicast"]:
                ipv6_peer_l = peer_info["ipv6Unicast"]["peers"].keys()
                self.new_peer_l.extend(ipv6_peer_l)
                for i in range (0, len(ipv6_peer_l)):
                    self.new_peer_state[ipv6_peer_l[i]] = \
                    peer_info["ipv6Unicast"]["peers"][ipv6_peer_l[i]]["state"]

        except Exception:
            syslog.syslog(syslog.LOG_ERR, "*ERROR* get_all_neigh_states Exception: %s"
                    % (traceback.format_exc()))

    def update_neigh_states(self):
        for i in range (0, len(self.new_peer_l)):
            peer = self.new_peer_l[i]
            key = "NEIGH_STATE_TABLE|%s" % peer
            if peer in self.peer_l:
                # only update the entry if state changed
                if self.peer_state[peer] != self.new_peer_state[peer]:
                    # state changed. Update state DB for this entry
                    state = self.new_peer_state[peer]
                    self.db.set(self.db.STATE_DB, key, 'state', state)
                    self.peer_state[peer] = state
                # remove this neighbor from old list since it is accounted for
                self.peer_l.remove(peer)
            else:
                # New neighbor found case. Add to dictionary and state DB
                state = self.new_peer_state[peer]
                self.db.set(self.db.STATE_DB, key, 'state', state)
                self.peer_state[peer] = state
        # Check for stale state entries to be cleaned up
        while len(self.peer_l) > 0:
            # remove this from the stateDB and the current nighbor state entry
            peer = self.peer_l.pop(0)
            del_key = "NEIGH_STATE_TABLE|%s" % peer
            self.db.delete(self.db.STATE_DB, del_key)
            del self.peer_state[peer]
        # Save the new List
        self.peer_l = self.new_peer_l[:]

def main():

    print "bgpmon service started"

    try:
        bgp_state_get = BgpStateGet()
    except Exception, e:
        syslog.syslog(syslog.LOG_ERR, "{}: error exit 1, reason {}".format(THIS_MODULE, str(e)))
        exit(1)

    # periodically obtain the new neighbor infomraton and update if necessary
    while True:
        time.sleep(15)
        if bgp_state_get.bgp_activity_detected():
            bgp_state_get.get_all_neigh_states()
            bgp_state_get.update_neigh_states()

if __name__ == '__main__':
    main()
