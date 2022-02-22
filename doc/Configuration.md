# SONiC Configuration Database Manual

Table of Contents
=================

   * [Introduction](#introduction)  
   * [Configuration](#configuration)  
   * [<strong>Config Load and Save</strong>](#config-load-and-save)  
         * [Incremental Configuration](#incremental-configuration)  
   * [<strong>Redis and Json Schema</strong>](#redis-and-json-schema)  
         * [ACL and Mirroring](#acl-and-mirroring)  
         * [BGP Sessions](#bgp-sessions)  
         * [BUFFER_PG](#buffer_pg)  
         * [Buffer pool](#buffer-pool)  
         * [Buffer profile](#buffer-profile)  
         * [Buffer queue](#buffer-queue)  
         * [Buffer port ingress profile list](#buffer-port-ingress-profile-list)  
         * [Buffer port egress profile list](#buffer-port-egress-profile-list)  
         * [Cable length](#cable-length)  
         * [COPP_TABLE](#copp_table)  
         * [CRM](#crm)  
         * [Data Plane L3 Interfaces](#data-plane-l3-interfaces)  
         * [DEFAULT_LOSSLESS_BUFFER_PARAMETER](#DEFAULT_LOSSLESS_BUFFER_PARAMETER)  
         * [Device Metadata](#device-metadata)  
         * [Device neighbor metada](#device-neighbor-metada)  
         * [DSCP_TO_TC_MAP](#dscp_to_tc_map)  
         * [FLEX_COUNTER_TABLE](#flex_counter_table)  
         * [L2 Neighbors](#l2-neighbors)  
         * [Loopback Interface](#loopback-interface)  
         * [LOSSLESS_TRAFFIC_PATTERN](#LOSSLESS_TRAFFIC_PATTERN)  
         * [Management Interface](#management-interface)  
         * [Management port](#management-port)  
         * [Management VRF](#management-vrf)  
         * [MAP_PFC_PRIORITY_TO_QUEUE](#map_pfc_priority_to_queue)  
         * [NTP Global Configuration](#ntp-global-configuration)  
         * [NTP and SYSLOG servers](#ntp-and-syslog-servers)  
         * [Policer](#policer)   
         * [Port](#port)   
         * [Port Channel](#port-channel)  
         * [Portchannel member](#portchannel-member)  
         * [Scheduler](#scheduler)  
         * [Port QoS Map](#port-qos-map)  
         * [Queue](#queue)  
         * [Tacplus Server](#tacplus-server)    
         * [TC to Priority group map](#tc-to-priority-group-map)  
         * [TC to Queue map](#tc-to-queue-map)  
         * [Versions](#versions)  
         * [VLAN](#vlan)   
         * [VLAN_MEMBER](#vlan_member)  
         * [Virtual router](#virtual-router)  
         * [WRED_PROFILE](#wred_profile)  
   * [For Developers](#for-developers)  
      * [Generating Application Config by Jinja2 Template](#generating-application-config-by-jinja2-template)
      * [Incremental Configuration by Subscribing to ConfigDB](#incremental-configuration-by-subscribing-to-configdb)



# Introduction																																									
This document lists the configuration commands schema applied in the SONiC eco system. All these commands find relevance in collecting system information, analysis and even for trouble shooting. All the commands are categorized under relevant topics with corresponding examples.  																																																																					

# Configuration

SONiC is managing configuration in a single source of truth - a redisDB
instance that we refer as ConfigDB. Applications subscribe to ConfigDB
and generate their running configuration correspondingly.

(Before Sep 2017, we were using an XML file named minigraph.xml to
configure SONiC devices. For historical documentation, please refer to
[Configuration with
Minigraph](https://github.com/Azure/SONiC/wiki/Configuration-with-Minigraph-(~Sep-2017)))

# **Config Load and Save**

In current version of SONiC, ConfigDB is implemented as database 4 of
local redis. When system boots, configurations will be loaded from
/etc/sonic/config_db.json file into redis. Please note that ConfigDB
content won't be written back into /etc/sonic/config_db.json file
automatically. In order to do that, a config save command need to be
manually executed from CLI. Similarly, config load will trigger a force
load of json file into DB. Generally, content in
/etc/sonic/config_db.json can be considered as starting config, and
content in redisDB running config.

We keep a way to load configuration from minigraph and write into
ConfigDB for backward compatibility. To do that, run `config
load_minigraph`.

### Incremental Configuration

The design of ConfigDB supports incremental configuration - application
could subscribe to changes in ConfigDB and response correspondingly.
However, this feature is not implemented by all applications yet. By Sep
2017 now, the only application that supports incremental configuration
is BGP (docker-fpm-quagga). For other applications, a manual restart is
required after configuration changes in ConfigDB.

# **Redis and Json Schema** 

ConfigDB uses a table-object schema that is similar with
[AppDB](https://github.com/Azure/sonic-swss/blob/4c56d23b9ff4940bdf576cf7c9e5aa77adcbbdcc/doc/swss-schema.md),
and `config_db.json` is a straight-forward serialization of DB. As an
example, the following fragments could be BGP-related configuration in
redis and json, correspondingly:


***Redis format***
```
127.0.0.1:6379[4]> keys BGP_NEIGHBOR:*

1) "BGP_NEIGHBOR:10.0.0.31"
2) "BGP_NEIGHBOR:10.0.0.39"
3) "BGP_NEIGHBOR:10.0.0.11"
4) "BGP_NEIGHBOR:10.0.0.7"

...

127.0.0.1:6379[4]> hgetall BGP_NEIGHBOR:10.0.0.3

1) "admin_status"
2) "up"
3) "peer_addr"
4) "10.0.0.2"
5) "asn"
6) "65200"
7) "name"
8) "ARISTA07T2"
```

***Json format***
```
"BGP_NEIGHBOR": {
	"10.0.0.57": {
		"rrclient": "0", 
		"name": "ARISTA01T1", 
		"local_addr": "10.0.0.56", 
		"nhopself": "0", 
		"holdtime": "10", 
		"asn": "64600", 
		"keepalive": "3"
	}, 
    "10.0.0.59": {
        "rrclient": "0", 
        "name": "ARISTA02T1", 
        "local_addr": "10.0.0.58", 
        "nhopself": "0", 
        "holdtime": "10", 
        "asn": "64600", 
        "keepalive": "3"
	},
}
```

Full sample config_db.json files are availables at
[here](https://github.com/Azure/SONiC/blob/gh-pages/doc/config_db.json)
and
[here](https://github.com/Azure/SONiC/blob/gh-pages/doc/config_db_t0.json).


### ACL and Mirroring

ACL and mirroring related configuration are defined in
**MIRROR_SESSION**, **ACL_TABLE** and **ACL_RULE** tables. Those
tables are in progress of migrating from APPDB. Please refer to their
schema in APPDB
[here](https://github.com/Azure/sonic-swss/blob/4c56d23b9ff4940bdf576cf7c9e5aa77adcbbdcc/doc/swss-schema.md)
and migration plan
[here](https://github.com/Azure/SONiC/wiki/ACL-Configuration-Requirement-Description).

```
{
"MIRROR_SESSION": {
        "everflow0": {
                "src_ip": "10.1.0.32",
                "dst_ip": "2.2.2.2"
        }
    },

"ACL_TABLE": {
        "DATAACL": {
                "policy_desc" : "data_acl",
                "type": "l3",
                "ports": [
                        "Ethernet0",
                        "Ethernet4",
                        "Ethernet8",
                        "Ethernet12"
                ]
        }
    }
}
```

***Below ACL table added as per the mail***
```
{
"ACL_TABLE": {
        "aaa": {
                "type": "L3",
                "ports": "Ethernet0"
        }
   },
"ACL_RULE": {
        "aaa|rule_0": {
        "PRIORITY": "55",
        "PACKET_ACTION": "DROP",
        "L4_SRC_PORT": "0"
        },
        "aaa|rule_1": {
        "PRIORITY": "55",
        "PACKET_ACTION": "DROP",
        "L4_SRC_PORT": "1"
        }
   }
}
```

***Below ACL table added by comparig minigraph.xml & config_db.json***

```
{
"ACL_TABLE": {
		"EVERFLOW": {
		"type": "MIRROR",
		"policy_desc": "EVERFLOW",
		"ports": [
		  "PortChannel0001",
		  "PortChannel0002",
		  "PortChannel0003",
		  "PortChannel0004"
		]
	  },
		"EVERFLOWV6": {
        "type": "MIRRORV6",
        "policy_desc": "EVERFLOWV6",
        "ports": [
          "PortChannel0001",
          "PortChannel0002",
          "PortChannel0003",
          "PortChannel0004"
        ]
      },
        "SNMP_ACL": {
          "services": [
            "SNMP"
        ],
        "type": "CTRLPLANE",
        "policy_desc": "SNMP_ACL"
      },
        "SSH_ONLY": {
          "services": [
            "SSH"
          ],
          "type": "CTRLPLANE",
          "policy_desc": "SSH_ONLY"
      }
   },

"ACL_RULE": {
        "SNMP_ACL|DEFAULT_RULE": {
            "PRIORITY": "1",
            "PACKET_ACTION": "DROP",
            "ETHER_TYPE": "2048"
        },
        "SNMP_ACL|RULE_1": {
            "PRIORITY": "9999",
            "PACKET_ACTION": "ACCEPT",
            "SRC_IP": "1.1.1.1/32",
            "IP_PROTOCOL": "17"
        },
        "SNMP_ACL|RULE_2": {
            "PRIORITY": "9998",
            "PACKET_ACTION": "ACCEPT",
            "SRC_IP": "2.2.2.2/32",
            "IP_PROTOCOL": "17"
        },
        "SSH_ONLY|DEFAULT_RULE": {
            "PRIORITY": "1",
            "PACKET_ACTION": "DROP",
            "ETHER_TYPE": "2048"
        },
        "SSH_ONLY|RULE_1": {
            "PRIORITY": "9999",
            "PACKET_ACTION": "ACCEPT",
            "SRC_IP": "4.4.4.4/8",
            "IP_PROTOCOL": "6"
        }
    }
}

```

***ACL table type configuration example***
```
{
    "ACL_TABLE_TYPE": {
        "CUSTOM_L3": {
            "MATCHES": [
                "IN_PORTS",
                "OUT_PORTS",
                "SRC_IP"
            ],
            "ACTIONS": [
                "PACKET_ACTION",
                "MIRROR_INGRESS_ACTION"
            ],
            "BIND_POINTS": [
                "PORT",
                "LAG"
            ]
        }
    },
    "ACL_TABLE": {
        "DATAACL": {
            "STAGE": "INGRESS",
            "TYPE": "CUSTOM_L3",
            "PORTS": [
                "Ethernet0",
                "PortChannel1"
            ]
        }
    },
    "ACL_RULE": {
        "DATAACL|RULE0": {
            "PRIORITY": "999",
            "PACKET_ACTION": "DROP",
            "SRC_IP": "1.1.1.1/32",
        }
    }
}
```

### BGP Sessions

BGP session configuration is defined in **BGP_NEIGHBOR** table. BGP
neighbor address is used as key of bgp neighbor objects. Object
attributes include remote AS number, neighbor router name, and local
peering address. Dynamic neighbor is also supported by defining peer
group name and IP ranges in **BGP_PEER_RANGE** table.

```
{
"BGP_NEIGHBOR": {
        "10.0.0.61": {
                "local_addr": "10.0.0.60",
                "asn": 64015,
                "name": "ARISTA15T0"
        },
        "10.0.0.49": {
                "local_addr": "10.0.0.48",
                "asn": 64009,
                "name": "ARISTA09T0"
        },
 
        "10.0.0.63": {
                "rrclient": "0", 
				"name": "ARISTA04T1", 
				"local_addr": "10.0.0.62", 
				"nhopself": "0", 
				"holdtime": "10", 
				"asn": "64600", 
				"keepalive": "3"
        }
		
"BGP_PEER_RANGE": {
    "BGPSLBPassive": {
        "name": "BGPSLBPassive",
        "ip_range": [
            "10.250.0.0/27"
        ]
    },
    "BGPVac": {
        "name": "BGPVac",
        "ip_range": [
            "10.2.0.0/16"
        ]
    }
  }
}
```

### BUFFER_PG

When the system is running in traditional buffer model, profiles needs to explicitly configured:

```
{
"BUFFER_PG": {
    "Ethernet0|3-4": {
        "profile": "pg_lossless_40000_5m_profile"
    },
    "Ethernet1|3-4": {
        "profile": "pg_lossless_40000_5m_profile"
    },
    "Ethernet2|3-4": {
        "profile": "pg_lossless_40000_5m_profile"
    }
  }
}

```

When the system is running in dynamic buffer model, profiles can be:

 - either calculated dynamically according to ports' configuration and just configured as "NULL";
 - or configured explicitly.

```
{
"BUFFER_PG": {
    "Ethernet0|3-4": {
        "profile": "NULL"
    },
    "Ethernet1|3-4": {
        "profile": "NULL"
    },
    "Ethernet2|3-4": {
        "profile": "static_profile"
    }
  }
}

```

### Buffer pool

When the system is running in traditional buffer model, the size of all of the buffer pools and xoff of ingress_lossless_pool need to be configured explicitly.

```
{
"BUFFER_POOL": {
    "egress_lossless_pool": {
        "type": "egress",
        "mode": "static",
        "size": "15982720"
    },
    "egress_lossy_pool": {
        "type": "egress",
        "mode": "dynamic",
        "size": "9243812"
    },
    "ingress_lossless_pool": {
        "xoff": "4194112",
        "type": "ingress",
        "mode": "dynamic",
        "size": "10875072"
    }
  }
}

```

When the system is running in dynamic buffer model, the size of some of the buffer pools can be omitted and will be dynamically calculated.

```
{
"BUFFER_POOL": {
    "egress_lossless_pool": {
        "type": "egress",
        "mode": "static",
        "size": "15982720"
    },
    "egress_lossy_pool": {
        "type": "egress",
        "mode": "dynamic",
    },
    "ingress_lossless_pool": {
        "type": "ingress",
        "mode": "dynamic",
    }
  }
}

```


### Buffer profile

```
{
"BUFFER_PROFILE": {
    "egress_lossless_profile": {
        "static_th": "3995680",
        "pool": "egress_lossless_pool",
        "size": "1518"
    },
    "egress_lossy_profile": {
        "dynamic_th": "3",
        "pool": "egress_lossy_pool",
        "size": "1518"
    },
    "ingress_lossy_profile": {
        "dynamic_th": "3",
        "pool": "ingress_lossless_pool",
        "size": "0"
    },
    "pg_lossless_40000_5m_profile": {
        "xon_offset": "2288",
        "dynamic_th": "-3",
        "xon": "2288",
        "xoff": "66560",
        "pool": "ingress_lossless_pool",
        "size": "1248"
    },
    "pg_lossless_40000_40m_profile": {
        "xon_offset": "2288",
        "dynamic_th": "-3",
        "xon": "2288",
        "xoff": "71552",
        "pool": "ingress_lossless_pool",
        "size": "1248"
    }
  }
}

```

When the system is running in dynamic buffer model and the headroom_type is dynamic, only dynamic_th needs to be configured and rest of fields can be omitted.
This kind of profiles will be handled by buffer manager and won't be applied to SAI.

```
{
  {
    "non_default_dynamic_th_profile": {
        "dynamic_th": 1,
        "headroom_type": "dynamic"
    }
  }
}
```

### Buffer queue

```
{
"BUFFER_QUEUE": {
    "Ethernet50,Ethernet52,Ethernet54,Ethernet56|0-2": {
        "profile": "egress_lossy_profile"
    },
    "Ethernet50,Ethernet52,Ethernet54,Ethernet56|3-4": {
        "profile": "egress_lossless_profile"
    },
    "Ethernet50,Ethernet52,Ethernet54,Ethernet56|5-6": {
        "profile": "egress_lossy_profile"
    }
  }
}
 
```

### Buffer port ingress profile list

```
{
"BUFFER_PORT_INGRESS_PROFILE_LIST": {
    "Ethernet50": {
        "profile_list": "ingress_lossy_profile,ingress_lossless_profile"
    },
    "Ethernet52": {
        "profile_list": "ingress_lossy_profile,ingress_lossless_profile"
    },
    "Ethernet56": {
        "profile_list": "ingress_lossy_profile,ingress_lossless_profile"
    }
  }
}

```

### Buffer port egress profile list

```
{
"BUFFER_PORT_EGRESS_PROFILE_LIST": {
    "Ethernet50": {
        "profile_list": "egress_lossy_profile,egress_lossless_profile"
    },
    "Ethernet52": {
        "profile_list": "egress_lossy_profile,egress_lossless_profile"
    },
    "Ethernet56": {
        "profile_list": "egress_lossy_profile,egress_lossless_profile"
    }
  }
}

```

### Cable length

```
{
"CABLE_LENGTH": {
    "AZURE": {
        "Ethernet8": "5m",
        "Ethernet9": "5m",
        "Ethernet2": "5m",
        "Ethernet58": "5m",
        "Ethernet59": "5m",
        "Ethernet50": "40m",
        "Ethernet51": "5m",
        "Ethernet52": "40m",
        "Ethernet53": "5m",
        "Ethernet54": "40m",
        "Ethernet55": "5m",
        "Ethernet56": "40m"
    }
  }
}

```

### COPP_TABLE

```
{
"COPP_TABLE": {
     "default": {
         "cbs": "600",
         "cir": "600",
	 "meter_type": "packets",
	 "mode": "sr_tcm",
	 "queue": "0",
	 "red_action": "drop"
     },
   
     "trap.group.arp": {
         "cbs": "600",
         "cir": "600",
	 "meter_type": "packets",
	 "mode": "sr_tcm",
	 "queue": "4",
	 "red_action": "drop",
	 "trap_action": "trap",
	 "trap_ids": "arp_req,arp_resp,neigh_discovery",
	 "trap_priority": "4"
      },
    
     "trap.group.lldp.dhcp.udld": {
         "queue": "4",
         "trap_action": "trap",
	 "trap_ids": "lldp,dhcp,udld",
	 "trap_priority": "4"
      },
    
     "trap.group.bgp.lacp": {
         "queue": "4",
         "trap_action": "trap",
	 "trap_ids": "bgp,bgpv6,lacp",
	 "trap_priority": "4"
      },
   
     "trap.group.ip2me": {
         "cbs": "600",
         "cir": "600",
	 "meter_type": "packets",
	 "mode": "sr_tcm",
	 "queue": "1",
	 "red_action": "drop",
	 "trap_action": "trap",
	 "trap_ids": "ip2me",
	 "trap_priority": "1"
      }
    }
}
```

### CRM

```
{
"CRM": {
    "Config": {
        "acl_table_threshold_type": "percentage",
        "nexthop_group_threshold_type": "percentage",
        "fdb_entry_high_threshold": "85",
        "acl_entry_threshold_type": "percentage",
        "ipv6_neighbor_low_threshold": "70",
        "nexthop_group_member_low_threshold": "70",
        "acl_group_high_threshold": "85",
        "ipv4_route_high_threshold": "85",
        "acl_counter_high_threshold": "85",
        "ipv4_route_low_threshold": "70",
        "ipv4_route_threshold_type": "percentage",
        "ipv4_neighbor_low_threshold": "70",
        "acl_group_threshold_type": "percentage",
        "ipv4_nexthop_high_threshold": "85",
        "ipv6_route_threshold_type": "percentage",
        "snat_entry_threshold_type": "percentage",
        "snat_entry_high_threshold": "85",
        "snat_entry_low_threshold": "70",
        "dnat_entry_threshold_type": "percentage",
        "dnat_entry_high_threshold": "85",
        "dnat_entry_low_threshold": "70",
        "ipmc_entry_threshold_type": "percentage",
        "ipmc_entry_high_threshold": "85",
        "ipmc_entry_low_threshold": "70"
    }
  }
}

```

### Data Plane L3 Interfaces

IP configuration for data plane are defined in **INTERFACE**,
**PORTCHANNEL_INTERFACE**, and **VLAN_INTERFACE** table. The objects
in all three tables have the interface (could be physical port, port
channel, or vlan) that IP address is attached to as first-level key, and
IP prefix as second-level key. IP interface objects don't have any
attributes.

```
{
"INTERFACE": {
        "Ethernet0|10.0.0.0/31": {},
        "Ethernet4|10.0.0.2/31": {},
        "Ethernet8|10.0.0.4/31": {}
		...
    },
	
"PORTCHANNEL_INTERFACE": {
        "PortChannel01|10.0.0.56/31": {},
        "PortChannel01|FC00::71/126": {},
        "PortChannel02|10.0.0.58/31": {},
        "PortChannel02|FC00::75/126": {}
		...
    },
"VLAN_INTERFACE": {
        "Vlan1000|192.168.0.1/27": {}
    }
}

```


### DEFAULT_LOSSLESS_BUFFER_PARAMETER

This table stores the default lossless buffer parameters for dynamic buffer calculation.

```
{
    "DEFAULT_LOSSLESS_BUFFER_PARAMETER": {
        "AZURE": {
            "default_dynamic_th": "0",
            "over_subscribe_ratio": "2"
        }
    }
}
```

### Device Metadata

The **DEVICE_METADATA** table contains only one object named
*localhost*. In this table the device metadata such as hostname, hwsku,
deployment envionment id and deployment type are specified. BGP local AS
number is also specified in this table as current only single BGP
instance is supported in SONiC.

```
{
"DEVICE_METADATA": {
        "localhost": {
        "hwsku": "Force10-S6100",
        "default_bgp_status": "up",
        "docker_routing_config_mode": "unified",
        "hostname": "sonic-s6100-01",
        "platform": "x86_64-dell_s6100_c2538-r0",
        "mac": "4c:76:25:f4:70:82",
        "default_pfcwd_status": "disable",
        "bgp_asn": "65100",
        "deployment_id": "1",
        "type": "ToRRouter",
        "buffer_model": "traditional"
    }
  }
}

```


### Device neighbor metada

```
{
"DEVICE_NEIGHBOR_METADATA": {
    "ARISTA01T1": {
        "lo_addr": "None",
        "mgmt_addr": "10.11.150.45",
        "hwsku": "Arista-VM",
        "type": "LeafRouter"
    },
    "ARISTA02T1": {
        "lo_addr": "None",
        "mgmt_addr": "10.11.150.46",
        "hwsku": "Arista-VM",
        "type": "LeafRouter"
    }
  }
}

```


### DSCP_TO_TC_MAP
```
{
"DSCP_TO_TC_MAP": {
    "AZURE": {
        "1": "1",
        "0": "1",
        "3": "3",
        "2": "1",
        "5": "2",
        "4": "4",
        "7": "1",
        "6": "1",
        "9": "1",
        "8": "0"
    }
  }
}

```


### MPLS_TC_TO_TC_MAP
```
{
"MPLS_TC_TO_TC_MAP": {
    "AZURE": {
        "0": "0",
        "1": "1",
        "2": "1",
        "3": "2",
        "4": "2",
        "5": "3",
        "6": "3",
        "7": "4"
    }
  }
}

```

### FLEX_COUNTER_TABLE

```
{
"FLEX_COUNTER_TABLE": {
    "PFCWD": {
        "FLEX_COUNTER_STATUS": "enable"
    },
    "PORT": {
        "FLEX_COUNTER_STATUS": "enable"
    },
    "QUEUE": {
        "FLEX_COUNTER_STATUS": "enable"
    }
  }
}

```


### L2 Neighbors

The L2 neighbor and connection information can be configured in
**DEVICE_NEIGHBOR** table. Those information are used mainly for LLDP.
While mandatory fields include neighbor name acting as object key and
remote port / local port information in attributes, optional information
about neighbor device such as device type, hwsku, management address and
loopback address can also be defined.

```
{
"DEVICE_NEIGHBOR": {
        "ARISTA04T1": {
                "mgmt_addr": "10.20.0.163",
                "hwsku": "Arista",
                "lo_addr": null,
                "local_port": "Ethernet124",
                "type": "LeafRouter",
                "port": "Ethernet1"
        },
        "ARISTA03T1": {
                "mgmt_addr": "10.20.0.162",
                "hwsku": "Arista",
                "lo_addr": null,
                "local_port": "Ethernet120",
                "type": "LeafRouter",
                "port": "Ethernet1"
        },
        "ARISTA02T1": {
                "mgmt_addr": "10.20.0.161",
                "hwsku": "Arista",
                "lo_addr": null,
                "local_port": "Ethernet116",
                "type": "LeafRouter",
                "port": "Ethernet1"
        },
        "ARISTA01T1": {
                "mgmt_addr": "10.20.0.160",
                "hwsku": "Arista",
                "lo_addr": null,
                "local_port": "Ethernet112",
                "type": "LeafRouter",
                "port": "Ethernet1"
        }
    }
}
```

### Loopback Interface

Loopback interface configuration lies in **LOOPBACK_INTERFACE** table
and has similar schema with data plane interfaces. The loopback device
name and loopback IP prefix act as multi-level key for loopback
interface objects.

```
{
"LOOPBACK_INTERFACE": {
        "Loopback0|10.1.0.32/32": {},
        "Loopback0|FC00:1::32/128": {}
  }
}

```

### LOSSLESS_TRAFFIC_PATTERN

The LOSSLESS_TRAFFIC_PATTERN table stores parameters related to
lossless traffic for dynamic buffer calculation

```
{
    "LOSSLESS_TRAFFIC_PATTERN": {
        "AZURE": {
            "mtu": "1024",
            "small_packet_percentage": "100"
        }
    }
}
```

### Management Interface

Management interfaces are defined in **MGMT_INTERFACE** table. Object
key is composed of management interface name and IP prefix. Attribute
***gwaddr*** specify the gateway address of the prefix.
***forced_mgmt_routes*** attribute can be used to specify addresses /
prefixes traffic to which are forced to go through management network
instead of data network.

```
{
"MGMT_INTERFACE": {
        "eth0|10.11.150.11/16": {
        "gwaddr": "10.11.0.1"
    },
    "eth0|FC00:2::32/64": {
        "forced_mgmt_routes": [
            "10.0.0.100/31",
            "10.250.0.8",
                "10.255.0.0/28"
        ],
        "gwaddr": "fc00:2::1"
    }
  }
}

```

### Management port

```
{
"MGMT_PORT": {
    "eth0": {
        "alias": "eth0",
        "admin_status": "up"
    }
  }
}
 
```


### Management VRF

```
{
"MGMT_VRF_CONFIG": {
    "vrf_global": {
        "mgmtVrfEnabled": "true"
     }
  }
}
```

### MAP_PFC_PRIORITY_TO_QUEUE

```
{
"MAP_PFC_PRIORITY_TO_QUEUE": {
    "AZURE": {
        "1": "1",
        "0": "0",
        "3": "3",
        "2": "2",
        "5": "5",
        "4": "4",
        "7": "7",
        "6": "6"
    }
  }
}
```
### NTP Global Configuration

These configuration options are used to modify the way that
ntp binds to the ports on the switch and which port it uses to
make ntp update requests from.

***NTP VRF***

If this option is set to `default` then ntp will run within the default vrf
**when the management vrf is enabled**. If the mgmt vrf is enabled and this value is
not set to default then ntp will run within the mgmt vrf.

This option **has no effect** if the mgmt vrf is not enabled.

```
{
"NTP": {
    "global": {
        "vrf": "default"
        }
    }
}
```


***NTP Source Port***

This option sets the port which ntp will choose to send time update requests from by.  

NOTE: If a Loopback interface is defined on the switch ntp will choose this by default, so this setting
is **required** if the switch has a Loopback interface and the ntp peer does not have defined routes
for that address.
 
```
{
"NTP": {
    "global": {
        "src_intf": "Ethernet1"
        }
    }
}
```

### NTP and SYSLOG servers

These information are configured in individual tables. Domain name or IP
address of the server is used as object key. Currently there are no
attributes in those objects.

***NTP server***
```
{
"NTP_SERVER": {
        "2.debian.pool.ntp.org": {},
        "1.debian.pool.ntp.org": {},
        "3.debian.pool.ntp.org": {},
        "0.debian.pool.ntp.org": {}
    },

"NTP_SERVER": {
    "23.92.29.245": {},
    "204.2.134.164": {}
    }
}
```

***Syslogserver***
```
{
"SYSLOG_SERVER": {
    "10.0.0.5": {},
    "10.0.0.6": {},
    "10.11.150.5": {}
    }
}
```

### Policer

Below is an example of the policer table configuration.
```
{
    "POLICER": {
        "everflow_static_policer": {
            "meter_type": "bytes",
            "mode": "sr_tcm",
            "cir": "12500000",
            "cbs": "12500000",
	    "pir": "17500000",
            "pbs": "17500000",
            "color": "aware",
            "red_packet_action": "drop",
	    "yellow_packet_action": "drop"
	    "green_packet_action": "forward"
       }
    }
}

```
Key to the table defines policer name Below are the fields
-   meter_type - Mandatory field. Defines how the metering is done. values - bytes, packets
-   mode - Mandatory field. Defines one of the three modes support. values - sr_tcm, tr_tcm, storm
-   cir  - Committed information rate bytes/sec or packets/sec based on meter_type
-   cbs - Committed burst size in bytes or packets based on meter_type
-   pir - Peak information rate in bytes/sec or packets/sec based on meter_type
-   pbs - Peak burst size in bytes or packets based on meter_type
-   color - Defines the color source for the policer. values - aware, blind
-   red_packet_action - Defines the action to be taken for red color packets
-   yellow_packet_action - Defines the action to be taken for yellow color packets
-   green_packet_action - Defines the action to be taken for green color packets.

The packet action could be:

-   'drop'
-   'forward'
-   'copy'
-   'copy_cancel'
-   'trap'
-   'log'
-   'deny'
-   'transit'
### Port

In this table the physical port configurations are defined. Each object
will have port name as its key, and port name alias and port speed as
optional attributes.

```
{
"PORT": {
        "Ethernet0": {
            "index": "0",
            "lanes": "101,102",
            "description": "fortyGigE1/1/1",
            "mtu": "9100",
            "alias": "fortyGigE1/1/1",
            "speed": "40000"
        },
        "Ethernet1": {
            "index": "1",
            "lanes": "103,104",
            "description": "fortyGigE1/1/2",
            "mtu": "9100",
            "alias": "fortyGigE1/1/2",
            "admin_status": "up",
            "speed": "40000"
        },
		"Ethernet63": {
            "index": "63",
            "lanes": "87,88",
            "description": "fortyGigE1/4/16",
            "mtu": "9100",
            "alias": "fortyGigE1/4/16",
            "speed": "40000"
        }
    }
}

```

### Port Channel

Port channels are defined in **PORTCHANNEL** table with port channel
name as object key and member list as attribute.

```
{
"PORTCHANNEL": {
        "PortChannel0003": {
                "admin_status": "up",
        "min_links": "1",
        "members": [
            "Ethernet54"
        ],
        "mtu": "9100"
    },
    "PortChannel0004": {
        "admin_status": "up",
        "min_links": "1",
        "members": [
            "Ethernet56"
        ],
        "mtu": "9100"
    }
  }
}
```


### Portchannel member

```
{
"PORTCHANNEL_MEMBER": {
    "PortChannel0001|Ethernet50": {}, 
    "PortChannel0002|Ethernet52": {}, 
    "PortChannel0003|Ethernet54": {}, 
    "PortChannel0004|Ethernet56": {}
  }
}

```
### Scheduler

```
{
"SCHEDULER": {
    "scheduler.0": {
        "type": "STRICT"
    },
    "scheduler.1": {
        "type": "WRR"
        "weight": "1",
        "meter_type": "bytes",
        "pir": "1250000000",
        "pbs": "8192"
    },
    "scheduler.port": {
        "meter_type": "bytes",
        "pir": "1000000000",
        "pbs": "8192"
    }
  }
}
```

### Port QoS Map

```
{
"PORT_QOS_MAP": {
    "Ethernet50,Ethernet52,Ethernet54,Ethernet56": {
        "tc_to_pg_map": "AZURE", 
        "tc_to_queue_map": "AZURE", 
        "pfc_enable": "3,4", 
        "pfc_to_queue_map": "AZURE", 
        "dscp_to_tc_map": "AZURE",
        "dscp_to_fc_map": "AZURE",
        "exp_to_fc_map": "AZURE",
        "scheduler": "scheduler.port"
    }
  }
}  
```

### Queue
```
{
"QUEUE": {
	"Ethernet56|4": {
        "wred_profile": "AZURE_LOSSLESS", 
        "scheduler": "scheduler.1"
    }, 
    "Ethernet56|5": {
        "scheduler": "scheduler.0"
    }, 
    "Ethernet56|6": {
        "scheduler": "scheduler.0"
    }
  }
}
```


### Tacplus Server

```
{
"TACPLUS_SERVER": {
    "10.0.0.8": {
        "priority": "1", 
        "tcp_port": "49"
    }, 
    "10.0.0.9": {
        "priority": "1", 
        "tcp_port": "49"
    }
  }
}
```


### TC to Priority group map

```
{
"TC_TO_PRIORITY_GROUP_MAP": {
    "AZURE": {
        "1": "1", 
        "0": "0", 
        "3": "3", 
        "2": "2", 
        "5": "5", 
        "4": "4", 
        "7": "7", 
        "6": "6"
    }
  }
}  
```

### TC to Queue map

```
{
"TC_TO_QUEUE_MAP": {
    "AZURE": {
        "1": "1", 
        "0": "0", 
        "3": "3", 
        "2": "2", 
        "5": "5", 
        "4": "4", 
        "7": "7", 
        "6": "6"
    }
  }
}  
```

### Versions

This table is where the curret version of the software is recorded.
```
{
    "VERSIONS": {
        "DATABASE": {
            "VERSION": "version_1_0_1"
        }
    }
}
```

### VLAN

This table is where VLANs are defined. VLAN name is used as object key,
and member list as well as an integer id are defined as attributes. If a
DHCP relay is required for this VLAN, a dhcp_servers attribute must be
specified for that VLAN, the value of which is a list that must contain
the domain name or IP address of one or more DHCP servers.

```
{
"VLAN": {
	"Vlan1000": {
		"dhcp_servers": [
			"192.0.0.1",
			"192.0.0.2",
			"192.0.0.3",
			"192.0.0.4"
		],
		"members": [
			"Ethernet0",
			"Ethernet4",
			"Ethernet8",
			"Ethernet12"
		],
		"vlanid": "1000"
	}
  }
}
```

### VLAN_MEMBER

VLAN member table has Vlan name together with physical port or port
channel name as object key, and tagging mode as attributes.

```
{
"VLAN_MEMBER": {
	"Vlan1000|PortChannel47": {
		"tagging_mode": "untagged"
	},
	"Vlan1000|Ethernet8": {
		"tagging_mode": "untagged"
	},
	"Vlan2000|PortChannel47": {
		"tagging_mode": "tagged"
	}
  }
}
```

### Virtual router

The virtual router table allows to insert or update a new virtual router
instance. The key of the instance is its name. The attributes in the
table allow to change properties of a virtual router. Attributes:

-   'v4' contains boolean value 'true' or 'false'. Enable or
    disable IPv4 in the virtual router
-   'v6' contains boolean value 'true' or 'false'. Enable or
    disable IPv6 in the virtual router
-   'src_mac' contains MAC address. What source MAC address will be
    used for packets egressing from the virtual router
-   'ttl_action' contains packet action. Defines the action for
    packets with TTL == 0 or TTL == 1
-   'ip_opt_action' contains packet action. Defines the action for
    packets with IP options
-   'l3_mc_action' contains packet action. Defines the action for
    unknown L3 multicast packets

The packet action could be:

-   'drop'
-   'forward'
-   'copy'
-   'copy_cancel'
-   'trap'
-   'log'
-   'deny'
-   'transit'


***TBD***
```
'VRF:rid1': {
	'v4': 'true',
	'v6': 'false',
	'src_mac': '02:04:05:06:07:08',
	'ttl_action': 'copy',
	'ip_opt_action': 'deny',
	'l3_mc_action': 'drop'
}
```


### WRED_PROFILE

```
{
"WRED_PROFILE": {
    "AZURE_LOSSLESS": {
        "red_max_threshold": "2097152", 
        "wred_green_enable": "true", 
        "ecn": "ecn_all", 
        "green_min_threshold": "1048576", 
        "red_min_threshold": "1048576", 
        "wred_yellow_enable": "true", 
        "yellow_min_threshold": "1048576", 
        "green_max_threshold": "2097152", 
        "green_drop_probability": "5", 
        "yellow_max_threshold": "2097152", 
        "wred_red_enable": "true", 
        "yellow_drop_probability": "5", 
        "red_drop_probability": "5"
    }
  }
}
```

### BREAKOUT_CFG

This table is introduced as part of Dynamic Port Breakout(DPB) feature.
It shows the current breakout mode of all ports(root ports).
The list of root ports, all possible breakout modes, and default breakout modes
 are obtained/derived from platform.json and hwsku.json files.

```
"BREAKOUT_CFG": {
    "Ethernet0": {
        "brkout_mode": "4x25G[10G]"
    },
    "Ethernet4": {
        "brkout_mode": "4x25G[10G]"
    },
    "Ethernet8": {
        "brkout_mode": "4x25G[10G]"
    },

        ......

    "Ethernet116": {
        "brkout_mode": "2x50G"
    },
    "Ethernet120": {
        "brkout_mode": "2x50G"
    },
    "Ethernet124": {
        "brkout_mode": "2x50G"
    }
}
```

For Developers
==============

Generating Application Config by Jinja2 Template
------------------------------------------------

To be added.

Incremental Configuration by Subscribing to ConfigDB
----------------------------------------------------

Detail instruction to be added. A sample could be found in this
[PR](https://github.com/Azure/sonic-buildimage/pull/861) that
implemented dynamic configuration for BGP.
