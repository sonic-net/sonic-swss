/*
 * SONiC-private extensions to the FPM netlink wire format consumed by
 * fpmsyncd. Mirror copy of the encoder side in
 * sonic-buildimage src/sonic-frr/dplane_fpm_sonic/dplane_fpm_sonic.c, the
 * same dual-copy convention fpm/fpm.h already follows. Shared by
 * routesync.cpp and the fpmsyncd unit tests, which hand-build raw
 * netlink frames carrying this attribute.
 *
 * If you change FPM_RTA_BACKUP_NH, update the encoder copy in lockstep.
 */

#ifndef SONIC_FPMSYNCD_FPM_BACKUP_NH_H
#define SONIC_FPMSYNCD_FPM_BACKUP_NH_H

/*
 * SONiC-private top-level RTA carrying BGP PIC backup nexthops on the FPM
 * wire, appended to RTM_NEWROUTE by dplane_fpm_sonic. Numbered well above
 * the kernel's RTA_MAX so it can't collide with future kernel additions.
 *
 * The payload is a sequence of struct rtnexthop, laid out like the
 * RTA_MULTIPATH payload, carrying regular IP nexthops (gateway as
 * RTA_GATEWAY, or RTA_VIA for an IPv6 nexthop on an IPv4 route, plus
 * ifindex and weight).
 */
#define FPM_RTA_BACKUP_NH 200

#endif /* SONIC_FPMSYNCD_FPM_BACKUP_NH_H */
