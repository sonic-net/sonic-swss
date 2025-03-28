import base64
import socket
import uuid
from ipaddress import ip_address as IP

from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *

VNET_ENCAP = "vnet_encap"
VNET_DIRECT = "vnet_direct"
PRIVATELINK = "privatelink"
DECAP = "decap"

SIP = "10.0.0.1"
UNDERLAY_IP = "25.1.1.1"
VNET_MAP_IP1 = "10.1.1.1"
VNET_MAP_IP2 = "10.1.1.2"
UNDERLAY_IP = "101.1.2.3"
OUTBOUND_ROUTE_PREFIX1 = "10.1.0.8/32"
OUTBOUND_ROUTE_PREFIX2 = "10.1.0.9/32"
OVERLAY_IP = "10.0.0.6"
PL_ENCODING_IP = "::56b2:0:20:0:0"
PL_ENCODING_MASK = "::ffff:ffff:ffff:0:0"
PL_UNDERLAY_SIP1 = "55.1.2.3"
PL_UNDERLAY_SIP2 = "55.2.3.4"
PL_OVERLAY_SIP = "fd40:108:0:d204:0:200::0"
PL_OVERLAY_SIP_MASK = "ffff:ffff:ffff:ffff:ffff:ffff::"
PL_OVERLAY_DIP = "2603:10e1:100:2::3401:203"
PL_OVERLAY_DIP_MASK = "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"

APPLIANCE_ID = "100"
VM_VNI = "4321"
ENCAP_VNI = 100
VNET1 = "Vnet1"
VNET1_VNI = "45654"
VNET1_GUID = "559c6ce8-26ab-4193-b946-ccc6e8f930b2"
MAC_STRING = "F4939FEFC47E"
MAC_ADDRESS = "F4:93:9F:EF:C4:7E"
ENI_ID = "497f23d7-f0ac-4c99-a98f-59b470e8c7bd"
ROUTE_GROUP1 = "RouteGroup1"
ROUTE_GROUP2 = "RouteGroup2"
ROUTE_GROUP1_GUID = "48af6ce8-26cc-4293-bfa6-0126e8fcdeb2"
ROUTE_GROUP2_GUID = "58cf62e0-22cc-4693-baa6-012358fcdec9"
TUNNEL1 = "Tunnel1"
TUNNEL1_ENDPOINTS = [IP("99.99.99.1")]
TUNNEL1_VNI = "101"
TUNNEL2 = "Tunnel2"
TUNNEL2_ENDPOINTS = [IP("55.55.55.1") + i for i in range(5)]
TUNNEL2_VNI = "102"
TUNNEL3 = "Tunnel3"
TUNNEL3_ENDPOINTS = [IP("2001:db8::1")]
TUNNEL3_VNI = "103"
TUNNEL4 = "Tunnel4"
TUNNEL4_ENDPOINTS = [IP("77.77.77.1") + i for i in range(1, 50)] + [IP("2002:db8::1") + i for i in range(1, 50)]
TUNNEL4_VNI = "104"
TUNNEL5 = "Tunnel5"
TUNNEL5_ENDPOINTS = [IP("88.88.88.1")] * 2
TUNNEL5_VNI = "105"


def fmt_tunnel_endpoints(endpoints):
    return [
        {"ipv4": socket.htonl(int(endpoint))}
        if endpoint.version == 4 else
        {"ipv6": base64.b64encode(endpoint.packed)}
        for endpoint in endpoints
    ]


APPLIANCE_CONFIG = {
    "sip": {
        "ipv4": socket.htonl(int(IP(SIP)))
    },
    "vm_vni": int(VM_VNI),
    "local_region_id": 10
}

VNET_CONFIG = {
    "vni": VNET1_VNI,
    "guid": {
        "value": base64.b64encode(bytes.fromhex(uuid.UUID(VNET1_GUID).hex))
    }
}

ENI_CONFIG = {
    "vnet": VNET1,
    "underlay_ip": {
        "ipv4": socket.htonl(int(IP(UNDERLAY_IP)))
    },
    "mac_address": bytes.fromhex(MAC_STRING),
    "eni_id": ENI_ID,
    "admin_state": State.STATE_ENABLED,
    "pl_underlay_sip": {
        "ipv4": socket.htonl(int(IP(PL_UNDERLAY_SIP1)))
    },
    "pl_sip_encoding": {
        "ip": {
            "ipv6": base64.b64encode(IP(PL_ENCODING_IP).packed)
        },
        "mask": {
            "ipv6": base64.b64encode(IP(PL_ENCODING_MASK).packed)
        }
    }
}

VNET_MAPPING_CONFIG_VNET_ENCAP = {
    "mac_address": bytes.fromhex(MAC_STRING),
    "action_type": RoutingType.ROUTING_TYPE_VNET_ENCAP,
    "underlay_ip": {
        "ipv4": socket.htonl(int(IP(UNDERLAY_IP)))
    },
}

VNET_MAPPING_CONFIG_PRIVATELINK = {
    "action_type": RoutingType.ROUTING_TYPE_PRIVATELINK,
    "underlay_ip": {
        "ipv4": socket.htonl(int(IP(UNDERLAY_IP)))
    },
    "overlay_sip_prefix": {
        "ip": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_SIP).packed)
        },
        "mask": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_SIP_MASK).packed)
        }
    },
    "overlay_dip_prefix": {
        "ip": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_DIP).packed)
        },
        "mask": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_DIP_MASK).packed)
        }
    },
}

VNET_MAPPING_CONFIG_PLNSG= {
    "routing_type": RoutingType.ROUTING_TYPE_PRIVATELINK,
    "underlay_ip": {
        "ipv4": socket.htonl(int(IP(UNDERLAY_IP)))
    },
    "overlay_sip_prefix": {
        "ip": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_SIP).packed)
        },
        "mask": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_SIP_MASK).packed)
        }
    },
    "overlay_dip_prefix": {
        "ip": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_DIP).packed)
        },
        "mask": {
            "ipv6": base64.b64encode(IP(PL_OVERLAY_DIP_MASK).packed)
        }
    },
    "tunnel": TUNNEL1
}

ROUTE_VNET_CONFIG = {
    "routing_type": RoutingType.ROUTING_TYPE_VNET,
    "vnet": VNET1,
}

ROUTE_VNET_CONFIG_UNDERLAY_SIP = {
    "routing_type": RoutingType.ROUTING_TYPE_VNET,
    "vnet": VNET1,
    "underlay_sip": {
        "ipv4": socket.htonl(int(IP(PL_UNDERLAY_SIP2)))
    }
}

ROUTING_TYPE_VNET_ENCAP_CONFIG = {
    "items": [
        {
            "action_name": "action1",
            "action_type": ActionType.ACTION_TYPE_MAPROUTING
        },
    ]
}

ROUTING_TYPE_PL_CONFIG = {
    "items": [
        {
            "action_name": "action1",
            "action_type": ActionType.ACTION_TYPE_4_to_6
        },
        {
            "action_name": "action2",
            "action_type": ActionType.ACTION_TYPE_STATICENCAP,
            "encap_type": EncapType.ENCAP_TYPE_NVGRE,
            "vni": ENCAP_VNI
        }
    ]
}

ROUTE_GROUP1_CONFIG = {
    "guid": ROUTE_GROUP1_GUID,
    "version": "rg_version"
}

ROUTE_GROUP2_CONFIG = {
    "guid": ROUTE_GROUP2_GUID,
    "version": "rg_version"
}

ENI_ROUTE_GROUP1_CONFIG = {
    "group_id": ROUTE_GROUP1,
}

ENI_ROUTE_GROUP2_CONFIG = {
    "group_id": ROUTE_GROUP2,
}

TUNNEL1_CONFIG = {
    "endpoints": fmt_tunnel_endpoints(TUNNEL1_ENDPOINTS),
    "encap_type": EncapType.ENCAP_TYPE_VXLAN,
    "vni": TUNNEL1_VNI
}

TUNNEL2_CONFIG = {
    "endpoints": fmt_tunnel_endpoints(TUNNEL2_ENDPOINTS),
    "encap_type": EncapType.ENCAP_TYPE_NVGRE,
    "vni": TUNNEL2_VNI
}

TUNNEL3_CONFIG = {
    "endpoints": fmt_tunnel_endpoints(TUNNEL3_ENDPOINTS),
    "encap_type": EncapType.ENCAP_TYPE_VXLAN,
    "vni": TUNNEL3_VNI
}

TUNNEL4_CONFIG = {
    "endpoints": fmt_tunnel_endpoints(TUNNEL4_ENDPOINTS),
    "encap_type": EncapType.ENCAP_TYPE_NVGRE,
    "vni": TUNNEL4_VNI
}

TUNNEL5_CONFIG = {
    "endpoints": fmt_tunnel_endpoints(TUNNEL5_ENDPOINTS),
    "encap_type": EncapType.ENCAP_TYPE_VXLAN,
    "vni": TUNNEL5_VNI
}
