#!/usr/bin/env python

""""
Description: restore_neighbors.py -- restoring neighbor table into kernel during system warm reboot.
    The script is started by supervisord in swss docker when the docker is started.
    If does not do anything in case warm restart is not enabled.
    In case system warm reboot is enabled, it will try to restore the neighbor table into kernel
    through netlink API calls and update the neigh table by sending arp/ns requests to all neighbor
    entries, then it sets the stateDB flag for neighsyncd to continue the reconciliation process.
    In case docker restart enabled only, it sets the stateDB flag so neighsyncd can follow
    the same logic.
"""

import sys
import swsssdk
import netifaces
import time
import monotonic
from pyroute2 import IPRoute, NetlinkError
from pyroute2.netlink.rtnl import ndmsg
from socket import AF_INET,AF_INET6
import logging
logging.getLogger("scapy.runtime").setLevel(logging.ERROR)
from scapy.all import conf, in6_getnsma, inet_pton, inet_ntop, in6_getnsmac, get_if_hwaddr, Ether, ARP, IPv6, ICMPv6ND_NS, ICMPv6NDOptSrcLLAddr
from swsscommon import swsscommon
import errno

logger = logging.getLogger(__name__)
logger.setLevel(logging.WARNING)
logger.addHandler(logging.NullHandler())

TIME_OUT = 300      # timeout the restore process in 5 mins if not finished
CHECK_INTERVAL = 5  # every 5 seconds to check interfaces state

ip_family = {"IPv4": AF_INET, "IPv6": AF_INET6}

# return the first ipv4/ipv6 address assigned on intf
def first_ip_on_intf(intf, family):
    if intf in netifaces.interfaces():
        ipaddresses = netifaces.ifaddresses(intf)
        if ip_family[family] in ipaddresses:
            # cover link local address as well
            return ipaddresses[ip_family[family]][0]['addr'].split("%")[0]
    return None

# check if the intf is operational up
def is_intf_oper_state_up(intf):
    oper_file = '/sys/class/net/{0}/carrier'
    try:
        state_file = open(oper_file.format(intf), 'r')
        state = state_file.readline().rstrip()
    except Exception as e:
        logger.info('Error: {}'.format(str(e)))
        return False
    if state == '1':
        return True
    return False

# read the neigh table from AppDB to memory, format as below
# build map as below, this can efficiently access intf and family groups later
#       { intf1 -> { { family1 -> [[ip1, mac1], [ip2, mac2] ...] }
#                    { family2 -> [[ipM, macM], [ipN, macN] ...] } },
#        ...
#         intfA -> { { family1 -> [[ipW, macW], [ipX, macX] ...] }
#                    { family2 -> [[ipY, macY], [ipZ, macZ] ...] } }
#       }
#
# Alternatively:
#  1, we can build:
#       { intf1 ->  [[family1, ip1, mac1], [family2, ip2, mac2] ...]},
#       ...
#       { intfA ->  [[family1, ipX, macX], [family2, ipY, macY] ...]}
#
#  2, Or simply build two maps based on families
# These alternative solutions would have worse performance because:
#  1, need iterate the whole list if only one family is up.
#  2, need check interface state twice due to the split map

def read_neigh_table_to_maps():
    db = swsssdk.SonicV2Connector(host='127.0.0.1')
    db.connect(db.APPL_DB, False)

    intf_neigh_map = {}

    keys = db.keys(db.APPL_DB, 'NEIGH_TABLE:*')
    keys = [] if keys is None else keys
    for key in keys:
        key_split = key.split(':', 2)
        intf_name = key_split[1]
        if intf_name == 'lo':
            continue
        dst_ip = key_split[2]
        value = db.get_all(db.APPL_DB, key)
        if 'neigh' in value and 'family' in value:
            dmac = value['neigh']
            family = value['family']
        else:
            raise RuntimeError('Neigh table format is incorrect')

        if family not in ip_family:
            raise RuntimeError('Neigh table format is incorrect')

        ip_mac_pair = []
        ip_mac_pair.append(dst_ip)
        ip_mac_pair.append(dmac)

        intf_neigh_map.setdefault(intf_name, {}).setdefault(family, []).append(ip_mac_pair)
    db.close(db.APPL_DB)
    return intf_neigh_map


# Use netlink to set neigh table into kernel, not overwrite the existing ones
def set_neigh_in_kernel(ipclass, family, intf_idx, dst_ip, dmac):
    logging.info('Add neighbor entries: family: {}, intf_idx: {}, ip: {}, mac: {}'.format(
    family, intf_idx, dst_ip, dmac))

    if family not in ip_family:
        return

    family_af_inet = ip_family[family]
    try :
        ipclass.neigh('add',
            family=family_af_inet,
            dst=dst_ip,
            lladdr=dmac,
            ifindex=intf_idx,
            state=ndmsg.states['reachable'])
    # If neigh exists, log it but no exception raise, other exceptions, raise
    except NetlinkError as e:
        if e[0] == errno.EEXIST:
            logger.warning('Neigh exists in kernel with family: {}, intf_idx: {}, ip: {}, mac: {}'.format(
            family, intf_idx, dst_ip, dmac))
        else:
            raise

# build ARP or NS packets depending on family
def build_arp_ns_pkt(family, smac, src_ip, dst_ip):
    if family == 'IPv4':
        eth = Ether(src=smac, dst='ff:ff:ff:ff:ff:ff')
        pkt = eth/ARP(op=ARP.who_has, pdst=dst_ip)
    elif family == 'IPv6':
        nsma = in6_getnsma(inet_pton(AF_INET6, dst_ip))
        mcast_dst_ip = inet_ntop(AF_INET6, nsma)
        dmac = in6_getnsmac(nsma)
        eth = Ether(src=smac,dst=dmac)
        ipv6 = IPv6(src=src_ip, dst=mcast_dst_ip)
        ns = ICMPv6ND_NS(tgt=dst_ip)
        ns_opt = ICMPv6NDOptSrcLLAddr(lladdr=smac)
        pkt = eth/ipv6/ns/ns_opt
    return pkt

# Set the statedb "NEIGH_RESTORE_TABLE|Flags", so neighsyncd can start reconciliation
def set_statedb_neigh_restore_done():
    db = swsssdk.SonicV2Connector(host='127.0.0.1')
    db.connect(db.STATE_DB, False)
    db.set(db.STATE_DB, 'NEIGH_RESTORE_TABLE|Flags', 'restored', 'true')
    db.close(db.STATE_DB)
    return

def restore_update_kernel_neighbors(intf_neigh_map):
    # create object for netlink calls to kernel
    ipclass = IPRoute()
    mtime = monotonic.time.time
    start_time = mtime()
    while (mtime() - start_time) < TIME_OUT:
        for intf, family_neigh_map in intf_neigh_map.items():
            # only try to restore to kernel when link is up
            if is_intf_oper_state_up(intf):
                src_mac = get_if_hwaddr(intf)
                intf_idx = ipclass.link_lookup(ifname=intf)[0]
                # create socket per intf to send packets
                s = conf.L2socket(iface=intf)

                # Only two families: 'IPv4' and 'IPv6'
                for family in ip_family.keys():
                    # if ip address assigned and if we have neighs in this family, restore them
                    src_ip = first_ip_on_intf(intf, family)
                    if src_ip and (family in family_neigh_map):
                        neigh_list = family_neigh_map[family]
                        for dst_ip, dmac in neigh_list:
                            # use netlink to set neighbor entries
                            set_neigh_in_kernel(ipclass, family, intf_idx, dst_ip, dmac)

                            # best effort to update kernel neigh info
                            # this will be updated by arp_update later too
                            s.send(build_arp_ns_pkt(family, src_mac, src_ip, dst_ip))
                        # delete this family on the intf
                        del intf_neigh_map[intf][family]
                # close the pkt socket
                s.close()

                # if all families are deleted, remove the key
                if len(intf_neigh_map[intf]) == 0:
                    del intf_neigh_map[intf]
        # map is empty, all neigh entries are restored
        if not intf_neigh_map:
            break
        time.sleep(CHECK_INTERVAL)


def main():

    print "restore_neighbors service is started"

    # Use warmstart python binding
    warmstart = swsscommon.WarmStart()
    warmstart.initialize("neighsyncd", "swss")
    warmstart.checkWarmStart("neighsyncd", "swss", False)

    # if swss or system warm reboot not enabled, don't run
    if not warmstart.isWarmStart():
        print "restore_neighbors service is skipped as warm restart not enabled"
        return

    # swss restart not system warm reboot
    if not warmstart.isSystemWarmRebootEnabled():
        set_statedb_neigh_restore_done()
        print "restore_neighbors service is done as system warm reboot not enabled"
        return

    # read the neigh table from appDB to internal map
    try:
        intf_neigh_map = read_neigh_table_to_maps()
    except RuntimeError as e:
        logger.exception(str(e))
        sys.exit(1)

    try:
        restore_update_kernel_neighbors(intf_neigh_map)
    except Exception as e:
        logger.exception(str(e))
        sys.exit(1)

    # set statedb to signal other processes like neighsyncd
    set_statedb_neigh_restore_done()
    print "restore_neighbor service is done for system warmreboot"
    return

if __name__ == '__main__':
    main()
