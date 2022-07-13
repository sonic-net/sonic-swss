#!/usr/bin/env python3

""""
Description: restore_neighbors.py -- restoring neighbor table into kernel during system warm reboot.
    The script is started by supervisord in swss docker when the docker is started.
    It does not do anything in case neither system nor swss warm restart is enabled.
    In case swss warm restart enabled only, it sets the stateDB flag so neighsyncd can continue
    the reconciation process.
    In case system warm reboot is enabled, it will try to restore the neighbor table into kernel
    through netlink API calls and update the neighbor table in kernel by sending arp/ns requests
    to all neighbor entries, then it sets the stateDB flag for neighsyncd to continue the
    reconciliation process.
"""

import sys
import netifaces
import time
from pyroute2 import IPRoute, NetlinkError
from pyroute2.netlink.rtnl import ndmsg
from socket import AF_INET,AF_INET6
import logging
logging.getLogger("scapy.runtime").setLevel(logging.ERROR)
from scapy.all import conf, in6_getnsma, inet_pton, inet_ntop, in6_getnsmac, get_if_hwaddr, Ether, ARP, IPv6, ICMPv6ND_NS, ICMPv6NDOptSrcLLAddr
from swsscommon import swsscommon
import errno
import syslog

logger = logging.getLogger(__name__)
logger.setLevel(logging.WARNING)
logger.addHandler(logging.NullHandler())

SYSLOG_IDENTIFIER = 'restore_neighbor'

def log_info(msg):
    syslog.openlog(SYSLOG_IDENTIFIER)
    syslog.syslog(syslog.LOG_INFO, msg)
    syslog.closelog()

def log_warning(msg):
    syslog.openlog(SYSLOG_IDENTIFIER)
    syslog.syslog(syslog.LOG_WARNING, msg)
    syslog.closelog()

def log_error(msg):
    syslog.openlog(SYSLOG_IDENTIFIER)
    syslog.syslog(syslog.LOG_ERR, msg)
    syslog.closelog()

# timeout the restore process in 110 seconds if not finished
# This is mostly to wait for interfaces to be created and up after system warm-reboot
# and this process is started by supervisord in swss docker.
# There had been devices taking close to 70 seconds to complete restoration, setting
# default timeout to 110 seconds.
DEF_TIME_OUT = 110

# every 5 seconds to check interfaces states
CHECK_INTERVAL = 5

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
        log_info('Error: {}'.format(str(e)))
        return False
    if state == '1':
        return True
    return False

def is_intf_up(intf, db):
    if not is_intf_oper_state_up(intf):
         return False
    if 'Vlan' in intf:
        table_name = 'VLAN_MEMBER_TABLE|{}|*'.format(intf)
        key = db.keys(db.STATE_DB, table_name)
        if key is None:
            log_info ("Vlan member is not yet created")
            return False
        if is_intf_up.counter == 0:
            time.sleep(3*CHECK_INTERVAL)
            is_intf_up.counter = 1
        log_info ("intf {} is up".format(intf))
    return True

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
    db = swsscommon.SonicV2Connector(host='127.0.0.1')
    db.connect(db.APPL_DB, False)

    intf_neigh_map = {}
    # Key format: "NEIGH_TABLE:intf-name:ipv4/ipv6", examples below:
    # "NEIGH_TABLE:Ethernet122:100.1.1.200"
    # "NEIGH_TABLE:Ethernet122:fe80::2e0:ecff:fe3b:d6ac"
    # Value format:
    # 1) "neigh"
    # 2) "00:22:33:44:55:cc"
    # 3) "family"
    # 4) "IPv4" or "IPv6"
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

        # build map like this:
        #       { intf1 -> { { family1 -> [[ip1, mac1], [ip2, mac2] ...] }
        #                    { family2 -> [[ipM, macM], [ipN, macN] ...] } },
        #         intfX -> {...}
        #       }
        ip_mac_pair = []
        ip_mac_pair.append(dst_ip)
        ip_mac_pair.append(dmac)

        intf_neigh_map.setdefault(intf_name, {}).setdefault(family, []).append(ip_mac_pair)
    db.close(db.APPL_DB)
    return intf_neigh_map


# Use netlink to set neigh table into kernel, not overwrite the existing ones
def set_neigh_in_kernel(ipclass, family, intf_idx, dst_ip, dmac):
    log_info('Add neighbor entries: family: {}, intf_idx: {}, ip: {}, mac: {}'.format(
    family, intf_idx, dst_ip, dmac))

    if family not in ip_family:
        return

    family_af_inet = ip_family[family]
    # Add neighbor to kernel with "stale" state, we will send arp/ns packet later
    # so if the neighbor is active, it will become "reachable", otherwise, it will
    # stay at "stale" state and get aged out by kernel.
    try :
        ipclass.neigh('add',
            family=family_af_inet,
            dst=dst_ip,
            lladdr=dmac,
            ifindex=intf_idx,
            state=ndmsg.states['stale'])

    # If neigh exists, log it but no exception raise, other exceptions, raise
    except NetlinkError as e:
        if e.code == errno.EEXIST:
            log_warning('Neigh exists in kernel with family: {}, intf_idx: {}, ip: {}, mac: {}'.format(
            family, intf_idx, dst_ip, dmac))
        else:
            raise

# build ARP or NS packets depending on family
def build_arp_ns_pkt(family, smac, src_ip, dst_ip):
    if family == 'IPv4':
        eth = Ether(src=smac, dst='ff:ff:ff:ff:ff:ff')
        pkt = eth/ARP(op='who-has', pdst=dst_ip, psrc=src_ip, hwsrc=smac)
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
    db = swsscommon.SonicV2Connector(host='127.0.0.1')
    db.connect(db.STATE_DB, False)
    db.set(db.STATE_DB, 'NEIGH_RESTORE_TABLE|Flags', 'restored', 'true')
    db.close(db.STATE_DB)
    return

# This function is to restore the kernel neighbors based on the saved neighbor map
# It iterates through the map, and work on interface by interface basis.
# If the interface is operational up and has IP configured per IP family,
# it will restore the neighbors per family.
# The restoring process is done by setting the neighbors in kernel from saved entries
# first, then sending arp/nd packets to update the neighbors.
# Once all the entries are restored, this function is returned.
# The interfaces' states were checked in a loop with an interval (CHECK_INTERVAL)
# The function will timeout in case interfaces' states never meet the condition
# after some time (DEF_TIME_OUT).
def restore_update_kernel_neighbors(intf_neigh_map, timeout=DEF_TIME_OUT):
    # create object for netlink calls to kernel
    ipclass = IPRoute()
    start_time = time.monotonic()
    is_intf_up.counter = 0
    db = swsscommon.SonicV2Connector(host='127.0.0.1')
    db.connect(db.STATE_DB, False)
    while (time.monotonic() - start_time) < timeout:
        for intf, family_neigh_map in list(intf_neigh_map.items()):
            # only try to restore to kernel when link is up
            if is_intf_up(intf, db):
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

                            log_info('Sending Neigh with family: {}, intf_idx: {}, ip: {}, mac: {}'.format(
                            family, intf_idx, dst_ip, dmac))
                            # sending arp/ns packet to update kernel neigh info
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
    db.close(db.STATE_DB)


def main():

    log_info ("restore_neighbors service is started")
    # Use warmstart python binding to check warmstart information
    warmstart = swsscommon.WarmStart()
    warmstart.initialize("neighsyncd", "swss")
    warmstart.checkWarmStart("neighsyncd", "swss", False)

    # if swss or system warm reboot not enabled, don't run
    if not warmstart.isWarmStart():
        log_info ("restore_neighbors service is skipped as warm restart not enabled")
        return

    # swss restart not system warm reboot, set statedb directly
    if not warmstart.isSystemWarmRebootEnabled():
        set_statedb_neigh_restore_done()
        log_info ("restore_neighbors service is done as system warm reboot not enabled")
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
    log_info ("restore_neighbor service is done for system warmreboot")
    return

if __name__ == '__main__':
    main()
