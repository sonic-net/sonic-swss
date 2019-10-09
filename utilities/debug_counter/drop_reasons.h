#ifndef DROP_REASONS_H
#define DROP_REASONS_H

// L2 Drop Reasons
#define L2_ANY               "L2_ANY"
#define SMAC_MULTICAST       "SMAC_MULTICAST"
#define SMAC_EQUALS_DMAC     "SMAC_EQUALS_DMAC"
#define VLAN_TAG_NOT_ALLOWED "VLAN_TAG_NOT_ALLOWED"
#define INGRESS_VLAN_FILTER  "INGRESS_VLAN_FILTER"
#define EGRESS_VLAN_FILTER   "EGRESS_VLAN_FILTER"
#define INGRESS_STP_FILTER   "INGRESS_STP_FILTER"
#define FDB_UC_DISCARD       "FDB_UC_DISCARD"
#define FDB_MC_DISCARD       "FDB_MC_DISCARD"
#define L2_LOOPBACK_FILTER   "L2_LOOPBACK_FILTER"
#define EXCEEDS_L2_MTU       "EXCEEDS_L2_MTU"

// L3 Ingress Drop Reasons
#define L3_ANY              "L3_ANY"
#define EXCEEDS_L3_MTU      "EXCEEDS_L3_MTU"
#define TTL                 "TTL"
#define L3_LOOPBACK_FILTER  "L3_LOOPBACK_FILTER"
#define REASON_NON_ROUTABLE "REASON_NON_ROUTABLE"
#define NO_L3_HEADER        "NO_L3_HEADER"
#define IP_HEADER_ERROR     "IP_HEADER_ERROR"
#define UC_DIP_MC_DMAC      "UC_DIP_MC_DMAC"
#define DIP_LOOPBACK        "DIP_LOOPBACK"
#define SIP_LOOPBACK        "SIP_LOOPBACK"
#define SIP_MC              "SIP_MC"
#define SIP_CLASS_E         "SIP_CLASS_E"
#define SIP_UNSPECIFIED     "SIP_UNSPECIFIED"
#define MC_DMAC_MISMATCH    "MC_DMAC_MISMATCH"
#define SIP_EQUALS_DIP      "SIP_EQUALS_DIP"
#define SIP_BC              "SIP_BC"
#define DIP_LOCAL           "DIP_LOCAL"
#define DIP_LINK_LOCAL      "DIP_LINK_LOCAL"
#define SIP_LINK_LOCAL      "SIP_LINK_LOCAL"
#define IPV6_MC_SCOPE0      "IPV6_MC_SCOPE0"
#define IPV6_MC_SCOPE1      "IPV6_MC_SCOPE1"
#define IRIF_DISABLED       "IRIF_DISABLED"
#define ERIF_DISABLED       "ERIF_DISABLED"
#define LPM4_MISS           "LPM4_MISS"
#define LPM6_MISS           "LPM6_MISS"
#define BLACKHOLE_ROUTE     "BLACKHOLE_ROUTE"
#define BLACKHOLE_ARP       "BLACKHOLE_ARP"
#define UNRESOLVED_NEXT_HOP "UNRESOLVED_NEXT_HOP"
#define L3_EGRESS_LINK_DOWN "L3_EGRESS_LINK_DOWN"
#define DECAP_ERROR         "DECAP_ERROR"

// ACL Drop Reasons
#define ACL_ANY             "ACL_ANY"
#define ACL_INGRESS_PORT    "ACL_INGRESS_PORT"
#define ACL_INGRESS_LAG     "ACL_INGRESS_LAG"
#define ACL_INGRESS_VLAN    "ACL_INGRESS_VLAN"
#define ACL_INGRESS_RIF     "ACL_INGRESS_RIF"
#define ACL_INGRESS_SWITCH  "ACL_INGRESS_SWITCH"
#define ACL_EGRESS_PORT     "ACL_EGRESS_PORT"
#define ACL_EGRESS_LAG      "ACL_EGRESS_LAG"
#define ACL_EGRESS_VLAN     "ACL_EGRESS_VLAN"
#define ACL_EGRESS_RIF      "ACL_EGRESS_RIF"
#define ACL_EGRESS_SWITCH   "ACL_EGRESS_SWITCH"

#endif
