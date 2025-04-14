Schema data is defined in ABNF [RFC5234](https://tools.ietf.org/html/rfc5234) syntax.

## Definitions of common tokens
    name                    = 1*DIGIT/1*ALPHA
    ref_hash_key_reference  = "[" hash_key "]" ;The token is a refernce to another valid DB key.
    hash_key                = name ; a valid key name (i.e. exists in DB)


## Application DB schema

### PORT_TABLE
Stores information for physical switch ports managed by the switch chip. Ports to the CPU (ie: management port) and logical ports (loopback) are not declared in the PORT_TABLE. See INTF_TABLE.

    ;Defines layer 2 ports
    ;In SONiC, Data is loaded from configuration file by portsyncd
    key                 = PORT_TABLE:ifname    ; ifname must be unique across PORT,INTF,VLAN,LAG TABLES
    admin_status        = "down" / "up"        ; admin status
    oper_status         = "down" / "up"        ; oper status
    lanes               = list of lanes ; (need format spec???)
    mac                 = 12HEXDIG      ;
    alias               = 1*64VCHAR     ; alias name of the port used by LLDP and SNMP, must be unique
    description         = 1*64VCHAR     ; port description
    speed               = 1*6DIGIT      ; port line speed in Mbps
    mtu                 = 1*4DIGIT      ; port MTU
    fec                 = 1*64VCHAR     ; port fec mode
    autoneg             = BIT           ; auto-negotiation mode
    preemphasis         = 1*8HEXDIG *( "," 1*8HEXDIG) ; list of hex values, one per lane
    idriver             = 1*8HEXDIG *( "," 1*8HEXDIG) ; list of hex values, one per lane
    ipredriver          = 1*8HEXDIG *( "," 1*8HEXDIG) ; list of hex values, one per lane
    pt_interface_id     = 1*4DIGIT      ; Path Tracing Interface ID (1-4095)
    pt_timestamp_template = "template1" / "template2" / "template3" / "template4"  ; Path Tracing Timestamp Template

    ;QOS Mappings
    map_dscp_to_tc      = ref_hash_key_reference
    map_tc_to_queue     = ref_hash_key_reference
    map_mpls_tc_to_tc   = ref_hash_key_reference

    Example:
    127.0.0.1:6379> hgetall PORT_TABLE:ETHERNET4
    1) "dscp_to_tc_map"
    2) "AZURE"
    3) "tc_to_queue_map"
    4) "AZURE"
    5) "mpls_tc_to_tc_map"
    6) "AZURE"

---------------------------------------------
### INTF_TABLE
cfgmgrd manages this table.  In SONiC, CPU (management) and logical ports (vlan, loopback, LAG) are declared in /etc/network/interface and /etc/sonic/config_db.json and loaded into the INTF_TABLE.

IP prefixes are formatted according to [RFC5954](https://tools.ietf.org/html/rfc5954) with a prefix length appended to the end

    ;defines logical network interfaces, an attachment to a PORT and list of 0 or more
    ;ip prefixes
    ;
    ;Status: stable
    key            = INTF_TABLE:ifname:IPprefix   ; an instance of this key will be repeated for each prefix
    IPprefix       = IPv4prefix / IPv6prefix   ; an instance of this key/value pair will be repeated for each prefix
    scope          = "global" / "local"        ; local is an interface visible on this localhost only
    if_mtu         = 1*4DIGIT                  ; MTU for the interface
    family         = "IPv4" / "IPv6"           ; address family

    IPv6prefix     =                             6( h16 ":" ) ls32
                    /                       "::" 5( h16 ":" ) ls32
                    / [               h16 ] "::" 4( h16 ":" ) ls32
                    / [ *1( h16 ":" ) h16 ] "::" 3( h16 ":" ) ls32
                    / [ *2( h16 ":" ) h16 ] "::" 2( h16 ":" ) ls32
                    / [ *3( h16 ":" ) h16 ] "::"    h16 ":"   ls32
                    / [ *4( h16 ":" ) h16 ] "::"              ls32
                    / [ *5( h16 ":" ) h16 ] "::"              h16
                    / [ *6( h16 ":" ) h16 ] "::"

     h16           = 1*4HEXDIG
     ls32          = ( h16 ":" h16 ) / IPv4address

     IPv4prefix    = dec-octet "." dec-octet "." dec-octet "." dec-octet “/” %d1-32

     dec-octet     = DIGIT                 ; 0-9
                    / %x31-39 DIGIT         ; 10-99
                    / "1" 2DIGIT            ; 100-199
                    / "2" %x30-34 DIGIT     ; 200-249
                    / "25" %x30-35          ; 250-255

For example (reorder output)

    127.0.0.1:6379> keys *
    1) "INTF_TABLE:lo:127.0.0.1/8"
    4) "INTF_TABLE:lo:::1"
    5) "INTF_TABLE:eth0:fe80::5054:ff:fece:6275/64"
    6) "INTF_TABLE:eth0:10.212.157.5/16"
    7) "INTF_TABLE:eth0.10:99.99.98.0/24"
    2) "INTF_TABLE:eth0.10:99.99.99.0/24"

    127.0.0.1:6379> HGETALL "INTF_TABLE:eth0.10:99.99.99.0/24"
    1) "scope"
    2) "global"
    3) "if_up"
    4) "1"
    5) "if_lower_up"
    6) "1"
    7) "if_mtu"
    8) "1500"
    127.0.0.1:6379> HGETALL "INTF_TABLE:eth0:fe80::5054:ff:fece:6275/64"
    1) "scope"
    2) "local"
    3) "if_up"
    4) "1"
    5) "if_lower_up"
    6) "1"
    7) "if_mtu"
    8) "65536"

---------------------------------------------
### VLAN_TABLE
    ;Defines VLANs and the interfaces which are members of the vlan
    ;Status: work in progress
    key                 = VLAN_TABLE:"Vlan"vlanid ; DIGIT 0-4095 with prefix "Vlan"
    admin_status        = "down" / "up"        ; admin status
    oper_status         = "down" / "up"        ; operating status
    mtu                 = 1*4DIGIT             ; MTU for the IP interface of the VLAN

---------------------------------------------
### LAG_TABLE
    ;a logical, link aggregation group interface (802.3ad) made of one or more ports
    ;In SONiC, data is loaded by teamsyncd
    ;Status: work in progress
    key                 = LAG_TABLE:lagname    ; logical 802.3ad LAG interface
    minimum_links       = 1*2DIGIT             ; to be implemented
    admin_status        = "down" / "up"        ; Admin status
    oper_status         = "down" / "up"        ; Oper status (physical + protocol state)
    mtu                 = 1*4DIGIT             ; MTU for this object
    linkup
    speed

    key                 = LAG_TABLE:lagname:ifname  ; physical port member of LAG, fk to PORT_TABLE:ifname
    status              = "enabled" / "disabled"    ; selected + distributing/collecting (802.3ad)
    speed               = ; set by LAG application, must match PORT_TABLE.duplex
    duplex              = ; set by LAG application, must match PORT_TABLE.duplex

For example, in a libteam implemenation, teamsyncd listens to Linux `RTM_NEWLINK` and `RTM_DELLINK` messages and creates or delete entries at the `LAG_TABLE:<team0>`

    127.0.0.1:6379> HGETALL "LAG_TABLE:team0"
    1) "admin_status"
    2) "down"
    3) "oper_status"
    4) "down"
    5) "mtu"
    6) "1500"

In addition for each team device, the teamsyncd listens to team events
and reflects the LAG ports into the redis under: `LAG_TABLE:<team0>:port`

    127.0.0.1:6379> HGETALL "LAG_TABLE:team0:veth0"
    1) "status"
    2) "disabled"
    3) "speed"
    4) "0Mbit"
    5) "duplex"
    6) "half"

---------------------------------------------
### ROUTE_TABLE
    ;Stores a list of routes
    ;Status: Mandatory
    key           = ROUTE_TABLE:prefix
    nexthop       = *prefix, ;IP addresses separated “,” (empty indicates no gateway)
    ifname        = ifindex? PORT_TABLE.key  ; zero or more separated by “,” (zero indicates no interface)
    mpls_nh       = STRING                   ; Comma-separated list of MPLS NH info.
    blackhole     = BIT ; Set to 1 if this route is a blackhole (or null0)
    weight        = weight_list              ; List of weights.
    nexthop_group = string ; index within the NEXTHOP_GROUP_TABLE, used instead of nexthop and intf fields
    segment       = string ; SRV6 segment name
    seg_src       = string ; ipv6 address for SRV6 tunnel source

---------------------------------------------

###### LABEL_ROUTE_TABLE
    ; Defines schema for MPLS label route table attributes
    key           = LABEL_ROUTE_TABLE:mpls_label ; MPLS label
    ; field       = value
    nexthop       = STRING                   ; Comma-separated list of nexthops.
    ifname        = STRING                   ; Comma-separated list of interfaces.
    mpls_nh       = STRING                   ; Comma-separated list of MPLS NH info.
    mpls_pop      = STRING                   ; Number of ingress MPLS labels to POP
    weight        = STRING                   ; Comma-separated list of weights.
    blackhole     = BIT ; Set to 1 if this route is a blackhole (or null0)
    nexthop_group = string ; index within the NEXTHOP_GROUP_TABLE, used instead of nexthop and intf fields

---------------------------------------------
### NEXTHOP_GROUP_TABLE
    ;Stores a list of groups of one or more next hops
    ;Status: Mandatory
    key           = NEXTHOP_GROUP_TABLE:string ; arbitrary index for the next hop group
    nexthop       = *prefix, ;IP addresses separated “,” (empty indicates no gateway)
    ifname        = ifindex? PORT_TABLE.key  ; zero or more separated by “,” (zero indicates no interface)
    mpls_nh       = STRING                   ; Comma-separated list of MPLS NH info.
    weight        = weight_list              ; List of weights.

---------------------------------------------
### CLASS_BASED_NEXT_HOP_GROUP_TABLE
    ;Stores a list of groups of one or more next hop groups used for class based forwarding
    ;Status: Mandatory
    key           = CLASS_BASED_NEXT_HOP_GROUP_TABLE:string ; arbitrary index for the next hop group
    members       = NEXT_HOP_GROUP_TABLE.key ; one or more separated by ","
    selection_map = FC_TO_NHG_INDEX_MAP_TABLE.key ; the NHG map to use for this CBF NHG

---------------------------------------------
### FC_TO_NHG_INDEX_MAP_TABLE
    ; FC to Next hop group index map
    key                    = "FC_TO_NHG_INDEX_MAP_TABLE:"name
    fc_num = 1*DIGIT ;value
    nh_index  = 1*DIGIT;  index of NH inside NH group

    Example:
    127.0.0.1:6379> hgetall "FC_TO_NHG_INDEX_MAP_TABLE:AZURE"
     1) "0" ;fc_num
     2) "0" ;nhg_index
     3) "1"
     4) "0"

---------------------------------------------
### NEIGH_TABLE
    ; Stores the neighbors or next hop IP address and output port or
    ; interface for routes
    ; Note: neighbor_sync process will resolve mac addr for neighbors
    ; using libnl to get neighbor table
    ;Status: Mandatory
    key           = prefix PORT_TABLE.name / VLAN_INTF_TABLE.name / LAG_INTF_TABLE.name = macaddress ; (may be empty)
    neigh         = 12HEXDIG         ;  mac address of the neighbor
    family        = "IPv4" / "IPv6"  ; address family

---------------------------------------------
### SRV6_SID_LIST_TABLE
    ; Stores IPV6 prefixes for a SRV6 segment name
    key           = ROUTE_TABLE:segment ; SRV6 segment name
    ; field       = value
    path          = STRING              ; Comma-separated list of IPV6 prefixes for a SRV6 segment
    type          = STRING              ; SRV6 segment list type like "insert", "encaps.red"; If not provided, default type will be "encaps.red"

---------------------------------------------
### SRV6_MY_SID_TABLE
    ; Stores SRV6 MY_SID table entries and associated actions
    key           = STRING ; SRV6 MY_SID prefix string
    ; field       = value
    action        = STRING ; MY_SID actions like "end", "end.dt46"
    vrf           = STRING ; VRF string for END.DT46 or END.DT4 or END.DT6

---------------------------------------------
### FDB_TABLE

    ; Stores FDB entries which were inserted into SAI state manually
    ; Notes:
    ; - only unicast FDB entries supported
    ; - only Vlan interfaces are supported
    key           = FDB_TABLE:"Vlan"vlanid:mac_address ; mac address will be inserted to FDB for the vlan interface
    port          = ifName                ; interface where the entry is bound to
    type          = "static" / "dynamic"  ; type of the entry

    Example:
    127.0.0.1:6379> hgetall FDB_TABLE:Vlan2:52-54-00-25-06-E9
    1) "port"
    2) "Ethernet0"
    3) "type"
    4) "static"

---------------------------------------------
### QUEUE_TABLE

    ; QUEUE table. Defines port queue.
    ; SAI mapping - port queue.

    key             = "QUEUE_TABLE:"port_name":queue_index
    queue_index     = 1*DIGIT
    port_name       = ifName
    queue_reference = ref_hash_key_reference

    ;field            value
    scheduler    = ref_hash_key_reference; reference to scheduler key
    wred_profile = ref_hash_key_reference; reference to wred profile key

    Example:
    127.0.0.1:6379> hgetall QUEUE_TABLE:ETHERNET4:1
    1) "scheduler"
    2) "BEST_EFFORT"
    3) "wred_profile"
    4) "AZURE"

---------------------------------------------
### TC\_TO\_QUEUE\_MAP\_TABLE
    ; TC to queue map
    ;SAI mapping - qos_map with SAI_QOS_MAP_ATTR_TYPE == SAI_QOS_MAP_TC_TO_QUEUE. See saiqosmaps.h
    key                    = "TC_TO_QUEUE_MAP_TABLE:"name
    ;field
    tc_num = 1*DIGIT
    ;values
    queue  = 1*DIGIT; queue index

    Example:
    27.0.0.1:6379> hgetall TC_TO_QUEUE_MAP_TABLE:AZURE
    1) "5" ;tc
    2) "1" ;queue index
    3) "6"
    4) "1"

---------------------------------------------
### DSCP\_TO\_TC\_MAP\_TABLE
    ; dscp to TC map
    ;SAI mapping - qos_map object with SAI_QOS_MAP_ATTR_TYPE == sai_qos_map_type_t::SAI_QOS_MAP_DSCP_TO_TC
    key        = "DSCP_TO_TC_MAP_TABLE:"name
    ;field    value
    dscp_value = 1*DIGIT
    tc_value   = 1*DIGIT

    Example:
    127.0.0.1:6379> hgetall "DSCP_TO_TC_MAP_TABLE:AZURE"
     1) "3" ;dscp
     2) "3" ;tc
     3) "6"
     4) "5"
     5) "7"
     6) "5"
     7) "8"
     8) "7"
     9) "9"
    10) "8"

---------------------------------------------
### MPLS\_TC\_TO\_TC\_MAP\_TABLE
    ; MPLS TC to TC map
    ;SAI mapping - qos_map object with SAI_QOS_MAP_ATTR_TYPE == sai_qos_map_type_t::SAI_QOS_MAP_EXP_TO_TC
    key        = "MPLS_TC_TO_TC_MAP_TABLE:"name
    ;field    value
    mpls_tc_value = 1*DIGIT
    tc_value   = 1*DIGIT

    Example:
    127.0.0.1:6379> hgetall "MPLS_TC_TO_TC_MAP_TABLE:AZURE"
     1) "0" ;mpls_tc
     2) "3" ;tc
     3) "1"
     4) "5"
     5) "2"
     6) "5"
     7) "3"
     8) "7"
     9) "4"
    10) "8"

### DSCP_TO_FC_TABLE_NAME
    ; dscp to FC map
    ;SAI mapping - qos_map object with SAI_QOS_MAP_ATTR_TYPE == sai_qos_map_type_t::SAI_QOS_MAP_TYPE_DSCP_TO_FORWARDING_CLASS
    key        = "DSCP_TO_FC_MAP_TABLE:"name
    ;field       value
    dscp_value = 1*DIGIT
    fc_value   = 1*DIGIT

    Example:
    127.0.0.1:6379> hgetall "DSCP_TO_FC_MAP_TABLE:AZURE"
     1) "0" ;dscp
     2) "1" ;fc
     3) "1"
     4) "1"
     5) "2"
     6) "3"
     7)
---------------------------------------------
### EXP_TO_FC_MAP_TABLE
    ; dscp to FC map
    ;SAI mapping - qos_map object with SAI_QOS_MAP_ATTR_TYPE == sai_qos_map_type_t::SAI_QOS_MAP_TYPE_MPLS_EXP_TO_FORWARDING_CLASS
    key            = "EXP_TO_FC_MAP_TABLE:"name
    ;field           value
    mpls_exp_value = 1*DIGIT
    fc_value       = 1*DIGIT

    Example:
    127.0.0.1:6379> hgetall "EXP_TO_FC_MAP_TABLE:AZURE"
     1) "0" ;mpls_exp
     2) "1" ;fc
     3) "1"
     4) "1"
     5) "2"
     6) "3"

---------------------------------------------
### SCHEDULER_TABLE
    ; Scheduler table
    ; SAI mapping - saicheduler.h
    key        = "SCHEDULER_TABLE":name
    ; field      value
    type       = "DWRR"/"WRR"/"STRICT"
    weight     = 2*DIGIT
    priority   = 1*DIGIT
    meter_type = "packets"/"bytes"
    cir        = 1*11 DIGIT  ; guaranteed rate in pps or bytes/sec
    cbs        = 1*11 DIGIT  ; guaranteed burst size in packets or bytes
    pir        = 1*11 DIGIT  ; max rate in pps or bytes/sec
    pbs        = 1*11 DIGIT  ; max burst size in packets or bytes

    Example:
    127.0.0.1:6379> hgetall SCHEDULER_TABLE:BEST_EFFORT
     1) "type"
     2) "PRIORITY"
     3) "priority"
     4) "7"
     5) "meter_type"
     6) "bytes"
     7) "cir"
     8) "1000000000"
     9) "cbs"
    10) "8192"
    11) "pir"
    12) "1250000000"
    13) "pbs"
    14) "8192"
    127.0.0.1:6379> hgetall SCHEDULER_TABLE:SCAVENGER
     1) "type"
     2) "DWRR"
     3) "weight"
     4) "35"
     5) "meter_type"
     6) "bytes"
     7) "cir"
     8) "1000000000"
     9) "cbs"
    10) "8192"
    11) "pir"
    12) "1250000000"
    13) "pbs"
    14) "8192"

---------------------------------------------
### WRED\_PROFILE\_TABLE
    ; WRED profile
    ; SAI mapping - saiwred.h
    key                     = "WRED_PROFILE_TABLE:"name
    ;field                  = value
    yellow_max_threshold    = byte_count
    green_max_threshold     = byte_count
    red_max_threshold       = byte_count
    byte_count              = 1*DIGIT
    ecn                     = "ecn_none" / "ecn_green" / "ecn_yellow" / "ecn_red" /  "ecn_green_yellow" / "ecn_green_red" / "ecn_yellow_red" / "ecn_all"
    wred_green_enable       = "true" / "false"
    wred_yellow_enable      = "true" / "false"
    wred_red_enable         = "true" / "false"

    Example:
    127.0.0.1:6379> hgetall "WRED_PROFILE_TABLE:AZURE"
    1) "green_max_threshold"
    2) "20480"
    3) "yellow_max_threshold"
    4) "30720"
    5) "red_max_threshold"
    6) "1234"
    7) "ecn"
    8) "ecn_all"
    9) "wred_green_enable"
    10) "true"
    11) "wred_yellow_enable"
    12) "true"
    13) "wred_red_enable"
    14) "true"


----------------------------------------------
### TUNNEL_DECAP_TABLE
    ; Stores tunnel decap rules
    key                     = TUNNEL_DECAP_TABLE:name
    ;field                      value
    tunnel_type             = "IPINIP"
    src_ip                  = IP
    dst_ip                  = IP1,IP2 ;IP addresses separated by ","
    dscp_mode               = "uniform" / "pipe"
    ecn_mode                = "copy_from_outer" / "standard" ;standard: Behavior defined in RFC 6040 section 4.2
    ttl_mode                = "uniform" / "pipe"

    IP = dec-octet "." dec-octet "." dec-octet "." dec-octet

    "src_ip" field is optional

    Example:
    127.0.0.1:6379> hgetall TUNNEL_DECAP_TABLE:NETBOUNCER
    1) "dscp_mode"
    2) "uniform"
    3) "src_ip"
    4) "127.0.0.1"
    5) "dst_ip"
    6) "127.0.0.1"
    7) "ecn_mode"
    8) "copy_from_outer"
    9) "ttl_mode"
    10) "uniform"
    11) "tunnel_type"
    12) "IPINIP"

---------------------------------------------

### LLDP_ENTRY_TABLE
    ; current LLDP neighbor information.
    port_table_key           = LLDP_ENTRY_TABLE:ifname ; .1.0.8802.1.1.2.1
    ; field                    value
    lldp_rem_port_id_subtype = 1DIGIT     ; 4.1.1.6
    lldp_rem_port_id         = 1*255VCHAR ; 4.1.1.7
    lldp_rem_port_desc       = 0*255VCHAR ; 4.1.1.8
    lldp_rem_sys_name        = 0*255VCHAR ; 4.1.1.9

    Example:
    127.0.0.1:6379[1]> hgetall  "LLDP_ENTRY_TABLE:ETHERNET4"
    1) "lldp_rem_sys_name"
    2) "the-system-name"
    3) "lldp_rem_port_id_subtype"
    4) "6"
    5) "lldp_rem_port_id"
    6) "Ethernet7"
    7) "lldp_rem_sys_desc"
    8) "Debian - SONiC v2"

---------------------------------------------

### COPP_TABLE
Control plane policing configuration table.
The settings in this table configure trap group, which is assigned a list of trap IDs (protocols), priority of CPU queue priority, and a policer.
The CPU queue priority is strict priority.
The policer is created and exclusively owned by the given trap group; it will be not shared (bound to) any other object.

packet_action = "drop" | "forward" | "copy" | "copy_cancel" | "trap" | "log" | "deny" | "transit"

    key = "COPP_TABLE:name"
    name_list     = name | name,name_list
    queue         = number; strict queue priority. Higher number means higher priority.
    trap_ids      = name_list; Acceptable values: bgp, lacp, arp, lldp, snmp, ssh, ttl error, ip2me
    trap_action   = packet_action; trap action which will be applied to all trap_ids.

    ;Settings for embedded policer. NOTE - if no policer settings are specified, then no policer is created.
    meter_type  = "packets" | "bytes"
    mode        = "sr_tcm" | "tr_tcm" | "storm"
    color        = "aware" | "blind"
    cbs         = number ;packets or bytes depending on the meter_type value
    cir         = number ;packets or bytes depending on the meter_type value
    pbs         = number ;packets or bytes depending on the meter_type value
    pir         = number ;packets or bytes depending on the meter_type value

    green_action   = packet_action
    yellow_action  = packet_action
    red_action     = packet_action

    Example:
    127.0.0.1:6379> hgetall  "COPP_TABLE:Group.P7"
     1) "cbs"
     2) "1024"
     3) "cir"
     4) "6600"
     5) "color"
     6) "aware"
     7) "meter_type"
     8) "packets"
     9) "mode"
    10) "sr_tcm"
    11) "pbs"
    12) "1024"
    13) "pir"
    14) "4096"
    15) "red_action"
    16) "drop"
    17) "trap_ids"
    18) "lacp"
    19) "trap_action"
    20) "trap"
    127.0.0.1:6379>

Note: The configuration will be created as json file to be consumed by swssconfig tool, which will place the table into the redis database.
It's possible to create separate configuration files for different ASIC platforms.

----------------------------------------------

### ACL\_TABLE\_TYPE
Stores a definition of table - set of matches, actions and bind point types. ACL_TABLE references a key inside this table in "type" field.

```
key: ACL_TABLE_TYPE:name           ; key of the ACL table type entry. The name is arbitary name user chooses.
; field         = value
matches         = match-list       ; list of matches for this table, matches are same as in ACL_RULE table.
actions         = action-list      ; list of actions for this table, actions are same as in ACL_RULE table.
bind_points     = bind-points-list ; list of bind point types for this table.

; values annotation
match            = 1*64VCHAR
match-list       = [1-max-matches]*match
action           = 1*64VCHAR
action-list      = [1-max-actions]*action
bind-point       = port/lag
bind-points-list = [1-max-bind-points]*bind-point
```

### ACL\_TABLE
Stores information about ACL tables on the switch.  Port names are defined in [port_config.ini](../portsyncd/port_config.ini).

    key           = ACL_TABLE:name          ; acl_table_name must be unique
    ;field        = value
    policy_desc   = 1*255VCHAR              ; name of the ACL policy table description
    type          = 1*255VCHAR              ; type of acl table, every type of
                                            ; table defines the match/action a
                                            ; specific set of match and actions.
                                            ; There are pre-defined table types like
                                            ; "MIRROR", "MIRRORV6", "MIRROR_DSCP",
                                            ; "L3", "L3V6", "MCLAG", "PFCWD", "DROP".
    ports         = [0-max_ports]*port_name ; the ports to which this ACL
                                            ; table is applied, can be emtry
                                            ; value annotations
    port_name     = 1*64VCHAR               ; name of the port, must be unique
    max_ports     = 1*5DIGIT                ; number of ports supported on the chip



### ACL\_RULE\_TABLE
Stores rules associated with a specific ACL table on the switch.

    key: ACL_RULE_TABLE:table_name:rule_name   ; key of the rule entry in the table,
                                               ; seq is the order of the rules
                                               ; when the packet is filtered by the
                                               ; ACL "policy_name".
                                               ; A rule is always assocaited with a
                                               ; policy.

    ;field        = value
    priority      = 1*3DIGIT                   ; rule priority. Valid values range
                                               ; could be platform dependent

    packet_action = "forward"/"drop"/"redirect:"redirect_parameter
                                               ; an action when the fields are matched
                                               ; we have a parameter in case of packet_action="redirect"
                                               ; This parameter defines a destination for redirected packets
                                               ; it could be:
                                               : name of physical port.          Example: "Ethernet10"
                                               : name of LAG port                Example: "PortChannel5"
                                               : next-hop ip address (in global) Example: "10.0.0.1"
                                               : next-hop ip address and vrf     Example: "10.0.0.2@Vrf2"
                                               : next-hop ip address and ifname  Example: "10.0.0.3@Ethernet1"
                                               : next-hop group set of next-hop  Example: "10.0.0.1,10.0.0.3@Ethernet1"

    redirect_action = 1*255CHAR                ; redirect parameter
                                               ; This parameter defines a destination for redirected packets
                                               ; it could be:
                                               : name of physical port.          Example: "Ethernet10"
                                               : name of LAG port                Example: "PortChannel5"
                                               : next-hop ip address (in global) Example: "10.0.0.1"
                                               : next-hop ip address and vrf     Example: "10.0.0.2@Vrf2"
                                               : next-hop ip address and ifname  Example: "10.0.0.3@Ethernet1"
                                               : next-hop group set of next-hop  Example: "10.0.0.1,10.0.0.3@Ethernet1"

    mirror_action = 1*255VCHAR                 ; refer to the mirror session (by default it will be ingress mirror action)
    mirror_ingress_action = 1*255VCHAR         ; refer to the mirror session
    mirror_egress_action = 1*255VCHAR          ; refer to the mirror session

    ether_type    = h16                        ; Ethernet type field

    ip_type       = ip_types                   ; options of the l2_protocol_type
                                               ; field.

    ip_protocol   = h8                         ; options of the l3_protocol_type field

    src_ip        = ipv4_prefix                ; options of the source ipv4
                                               ; address (and mask) field

    dst_ip        = ipv4_prefix                ; options of the destination ipv4
                                               ; address (and mask) field

    src_ipv6      = ipv6_prefix                ; options of the source ipv6
                                               ; address (and mask) field

    dst_ipv6      = ipv6_prefix                ; options of the destination ipv6
                                               ; address (and mask) field

    l4_src_port   = port_num                   ; source L4 port or the
    l4_dst_port   = port_num                   ; destination L4 port

    l4_src_port_range = port_num_L-port_num_H  ; source ports range of L4 ports field
    l4_dst_port_range = port_num_L-port_num_H  ; destination ports range of L4 ports field

    tcp_flags     = h8/h8                      ; TCP flags field and mask
    dscp          = h8                         ; DSCP field (only available for mirror
                                               ; table type)

    ;value annotations
    ip_types = any | ip | ipv4 | ipv4any | non_ipv4 | ipv6any | non_ipv6
    port_num      = 1*5DIGIT   ; a number between 0 and 65535
    port_num_L    = 1*5DIGIT   ; a number between 0 and 65535,
                               ; port_num_L < port_num_H
    port_num_H    = 1*5DIGIT   ; a number between 0 and 65535,
                               ; port_num_L < port_num_H
    ipv6_prefix   =                 6( h16 ":" ) ls32
       /                       "::" 5( h16 ":" ) ls32
       / [               h16 ] "::" 4( h16 ":" ) ls32
       / [ *1( h16 ":" ) h16 ] "::" 3( h16 ":" ) ls32
       / [ *2( h16 ":" ) h16 ] "::" 2( h16 ":" ) ls32
       / [ *3( h16 ":" ) h16 ] "::"    h16 ":"   ls32
       / [ *4( h16 ":" ) h16 ] "::"              ls32
       / [ *5( h16 ":" ) h16 ] "::"              h16
       / [ *6( h16 ":" ) h16 ] "::"
    h8          = 1*2HEXDIG
    h16         = 1*4HEXDIG
    ls32        = ( h16 ":" h16 ) / IPv4address
    ipv4_prefix = dec-octet "." dec-octet "." dec-octet "." dec-octet “/” %d1-32
    dec-octet   = DIGIT                     ; 0-9
                    / %x31-39 DIGIT         ; 10-99
                    / "1" 2DIGIT            ; 100-199
                    / "2" %x30-34 DIGIT     ; 200-249

Example:

    [
        {
            "ACL_TABLE:Drop_IP": {
                "policy_desc" : "Drop_Traffic",
                "type" : "L3",
                "ports" : "Ethernet0,Ethernet4"
            },
            "OP": "SET"
        },
        {
            "ACL_RULE_TABLE:Drop_IP:TheDrop": {
                "priority" : "55",
                "SRC_IP" : "20.0.0.0/25",
                "DST_IP" : "20.0.0.0/23",
                "L4_SRC_PORT" : "80",
                "PACKET_ACTION" : "DROP"
            },
            "OP": "SET"
        }
    ]

Equivalent RedisDB entry:

    127.0.0.1:6379> KEYS *ACL*
    1) "ACL_TABLE:Drop_IP"
    2) "ACL_RULE_TABLE:Drop_IP:TheDrop"
    127.0.0.1:6379> HGETALL ACL_TABLE:Drop_IP
    1) "policy_desc"
    2) "Drop_Traffic"
    3) "ports"
    4) "Ethernet0,Ethernet4"
    5) "type"
    6) "L3"
    127.0.0.1:6379> HGETALL ACL_RULE_TABLE:Drop_IP:TheDrop
     1) "DST_IP"
     2) "20.0.0.0/23"
     3) "L4_SRC_PORT"
     4) "80"
     5) "PACKET_ACTION"
     6) "DROP"
     7) "SRC_IP"
     8) "20.0.0.0/25"
     9) "priority"
    10) "55"
    127.0.0.1:6379>

----------------------------------------------

### MIRROR\_SESSION\_TABLE
Mirror session table
Stores information about mirror sessions and their properties.

    key       = MIRROR_SESSION_TABLE:mirror_session_name ; mirror_session_name is
                                                         ; unique session
                                                         ; identifier
    ; field   = value
    status    = "active"/"inactive"   ; Session state.
    src_ip    = ipv4_address          ; Session souce IP address
    dst_ip    = ipv4_address          ; Session destination IP address
    gre_type  = h16                   ; Session GRE protocol type
    dscp      = h8                    ; Session DSCP
    ttl       = h8                    ; Session TTL
    queue     = h8                    ; Session output queue
    policer   = policer_name          ; Session policer name
    dst_port  = PORT_TABLE|ifname     ; Session destination PORT
    src_port  = PORT_TABLE|ifname     ; Session source PORT/LAG list
    direction = "RX"/"TX"/"BOTH"      ; Session direction
    type      = "SPAN"/"ERSPAN"       ; Session type. Default is ERSPAN

    ;value annotations
    mirror_session_name = 1*255VCHAR
    policer_name        = 1*255VCHAR
    h8                  = 1*2HEXDIG
    h16                 = 1*4HEXDIG
    ipv4_address        = dec-octet "." dec-octet "." dec-octet "." dec-octet “/” %d1-32
    dec-octet           = DIGIT                     ; 0-9
                           / %x31-39 DIGIT         ; 10-99
                           / "1" 2DIGIT            ; 100-199
                           / "2" %x30-34 DIGIT     ; 200-249

Example:

    [
        {
            "MIRROR_SESSION_TABLE:session_1": {
                "src_ip": "1.1.1.1",
                "dst_ip": "2.2.2.2",
                "gre_type": "0x6558",
                "dscp": "50",
                "ttl": "64",
                "queue": "0"
            },
            "OP": "SET"
        }
    ]

    [
        {
            "MIRROR_SESSION_TABLE:session_2": {
                "src_ip": "1.1.1.1",
                "dst_ip": "2.2.2.2",
                "gre_type": "0x6558",
                "dscp": "50",
                "ttl": "64",
                "queue": "0"
                "src_port": "Ethernet0,PortChannel001"
                "direction": "BOTH"
                "type": "ERSPAN"
            },
            "OP": "SET"
        }
    ]

    [
        {
            "MIRROR_SESSION_TABLE:session_3": {
                "type": "SPAN"
                "dst_port": "Ethernet0"
                "src_port": "Ethernet4,PortChannel002"
                "direction": "BOTH"
            },
            "OP": "SET"
        }
    ]



Equivalent RedisDB entry:

    127.0.0.1:6379> KEYS *MIRROR*
    1) "MIRROR_SESSION_TABLE:session_1"
    127.0.0.1:6379> HGETALL MIRROR_SESSION_TABLE:session_1
     1) "src_ip"
     2) "1.1.1.1"
     3) "dst_ip"
     4) "2.2.2.2"
     5) "gre_type"
     6) "0x6558"
     7) "dscp"
     8) "50"
     9) "ttl"
    10) "64"
    11) "queue"
    12) "0"

    127.0.0.1:6379> KEYS *MIRROR*
    1) "MIRROR_SESSION_TABLE:session_2"
    127.0.0.1:6379> HGETALL MIRROR_SESSION_TABLE:session_2
     1) "src_ip"
     2) "1.1.1.1"
     3) "dst_ip"
     4) "2.2.2.2"
     5) "gre_type"
     6) "0x6558"
     7) "dscp"
     8) "50"
     9) "ttl"
    10) "64"
    11) "queue"
    12) "0"
    13) "src_port"
    14) "Ethernet0,PortChannel001"
    15) "direction"
    16) "BOTH"
    17) "type"
    18) "ERSPAN"

    127.0.0.1:6379> KEYS *MIRROR*
    1) "MIRROR_SESSION_TABLE:session_1"
    127.0.0.1:6379> HGETALL MIRROR_SESSION_TABLE:session_3i
    1) "type"
    2) "SPAN"
    3) "dst_port"
    4) "Ethernet0"
    5) "src_port"
    6) "Ethernet4,PortChannel002"
    7) "direction"
    8) "RX"
---------------------------------------------

### POLICER_TABLE
Policer table
Stores information about policers and their properties.

packet_action = "drop" | "forward" | "copy" | "copy_cancel" | "trap" | "log" | "deny" | "transit"

    ;Key
    key = "POLICER_TABLE:name"

    ;Field-Value tuples
    meter_type  = "packets" | "bytes"
    mode        = "sr_tcm" | "tr_tcm" | "storm"
    color       = "aware" | "blind"
    cbs         = number ;packets or bytes depending on the meter_type value
    cir         = number ;packets or bytes depending on the meter_type value
    pbs         = number ;packets or bytes depending on the meter_type value
    pir         = number ;packets or bytes depending on the meter_type value

    green_action   = packet_action
    yellow_action  = packet_action
    red_action     = packet_action

    Example:
    127.0.0.1:6379> hgetall "POLICER_TABLE:POLICER_1"
     1) "cbs"
     2) "600"
     3) "cir"
     4) "600"
     5) "meter_type"
     6) "packets"
     7) "mode"
     8) "sr_tcm"
     9) "red_action"
    10) "drop"

----------------------------------------------

### VXLAN\_TUNNEL\_MAP
    ;Stores vxlan tunnel map configuration. Defines mapping between vxlan vni and vrf

    key       = VXLAN_TUNNEL_MAP:tunnel_name:tunnel_map_name
                                                ; tunnel_name is a reference to created vxlan tunnel
                                                ; tunnel_map_name is an arbitrary name of the map
    vni       = 1*8DIGIT                        ; vni id, defined for tunnel map
    vrf       = vrf_name                        ; name of the vrf

### VNET\_ROUTE\_TUNNEL_TABLE
    ;Defines schema for VNet Route tunnel table attributes

    key                        = VNET_ROUTE_TUNNEL_TABLE:vnet_name:prefix
                                                ; Vnet route tunnel table with prefix
    ; field                      value
    endpoint                   = IP             ; Host VM IP address
    mac_address                = 12HEXDIG       ; Inner dest mac in encapsulated packet (Optional)
    vxlanid                    = 1*8DIGIT       ; VNI value in encapsulated packet (Optional)

    ;value annotations
    vnet_name                  = 1*16VCHAR

### VNET\_ROUTE_TABLE
    ;Defines schema for VNet Route table attributes
    key                        = VNET_ROUTE_TABLE:vnet_name:prefix
                                                ; Vnet route table with prefix
    ;field                       value
    nexthop                    = IP             ; Nexthop IP address (Optional)
    ifname                     = ifname         ; Interface name

### BUFFER_POOL_TABLE
    ;Stores buffer pools

    key             = BUFFER_POOL_TABLE:poolname    ; The poolname can be one of ingress_lossless_pool, ingress_lossy_pool, egress_lossless_pool, and egress_lossy_pool or other used-defined pools.
    mode            = "dynamic" / "static"          ; Whether the pool uses dynamic threshold or static threshold.
    type            = "ingress" / "egress"          ; Whether the pool serves for ingress or egress traffic
    size            = 1*10DIGIT                     ; The size of the shared buffer pool
    xoff            = 1*10DIGIT                     ; The size of the shared headroom pool. Available only for ingress_lossless_pool.

### BUFFER_PROFILE_TABLE
    ;Stores buffer profiles

    key             = BUFFER_PROFILE_TABLE:profilename      ; profile name can be predefined or dynamically generated with name convention "pg_lossless_<speed>_<cable_length>_profile"
    pool            = reference to BUFFER_POOL_TABLE object
    xon             = 1*6DIGIT                              ; The xon threshold. The switch stops sending PFC frame when the buffer occupancy drops to this threshold.
    xon_offset      = 1*6DIGIT                              ; The xon offset. If both xon and xon_offset have been defined, the switch stops sending PFC frame
                                                            ; when the buffer occupancy drops to xon or size of buffer pool size minus xon_offset, whichever is larger.
    xoff            = 1*6DIGIT                              ; The xoff threshold. The switch starts sending PFC frame when the buffer occupancy rises to this threshold.
    size            = 1*6DIGIT                              ; The reserved size of the PG or queue referencing this buffer profile.
    dynamic_th      = 1*2DIGIT                              ; For dynamic pools, representing the proportion of currently available memory in the pool the PG or queue can occupy.
                                                            ; It is calculated as:
                                                            ;     alpha = 2 ^ dynamic_th;
                                                            ;     proportion = alpha / (1 + alpha)
    static_th       = 1*10DIGIT                             ; For static pools, representing the threshold in bytes the PG or queue can occupy.

### BUFFER_PG_TABLE
    ;Stores buffer PG (priority-groups)

    key            = BUFFER_PG_TABLE:port_name:pg               ; The pg consists of a single number representing a single priority or two numbers connected by a dash representing a range of priorities.
                                                                ; By default, PG 0 for lossy traffic and PG 3-4 for lossless traffic.
    profile        = reference to BUFFER_PROFILE_TABLE object

### BUFFER_QUEUE_TABLE
    ;Stores buffer queue

    key            = BUFFER_QUEUE_TABLE:port_name:queue         ; The queue consists of a single number representing a single priority or two numbers connected by a dash representing a range of priorities.
                                                                ; By default, queue 0-2 and 5-6 for lossy traffic and queue 3-4 for lossless traffic
    profile        = reference to BUFFER_PROFILE_TABLE object

### BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE
    ;Stores per port buffer threshold on ingress side

    key            = BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE:port_name
    profile_list   = a list of references to BUFFER_PROFILE_TABLE object    ; Typically, for each ingress buffer pools there should be a buffer profile referencing the pool in the list.
                                                                            ; For example, if there are two ingress buffer pools in the system, ingress_lossless_pool and ingress_lossy_pool,
                                                                            ; there should be two profiles in the list: ingress_lossless_profile and ingress_lossy_profile

### BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE
    ;Stores per port buffer threshold on egress side

    key            = BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE:port_name
    profile_list   = a list of references to BUFFER_PROFILE_TABLE object    ; Similar to profile_list in BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE but on egress side.

## Configuration DB schema

### PORT_TABLE
Stores information for physical switch ports managed by the switch chip. Ports to the CPU (ie: management port) and logical ports (loopback) are not declared in the PORT_TABLE. See MGMT_PORT.

    ;Configuration for layer 2 ports
    key                 = PORT|ifname   ; ifname must be unique across PORT,INTF,VLAN,LAG TABLES
    admin_status        = "down" / "up" ; admin status
    lanes               = list of lanes ; (need format spec???)
    mac                 = 12HEXDIG      ;
    alias               = 1*64VCHAR     ; alias name of the port used by LLDP and SNMP, must be unique
    description         = 1*64VCHAR     ; port description
    speed               = 1*6DIGIT      ; port line speed in Mbps
    mtu                 = 1*4DIGIT      ; port MTU
    fec                 = 1*64VCHAR     ; port fec mode
    autoneg             = BIT           ; auto-negotiation mode
    pt_interface_id     = 1*4DIGIT      ; Path Tracing Interface ID (1-4095)
    pt_timestamp_template = "template1" / "template2" / "template3" / "template4"  ; Path Tracing Timestamp Template

### MGMT_PORT_TABLE
    ;Configuration for management port, including at least one key
    key                 = MGMT_PORT|ifname    ; ifname must be unique across PORT,INTF,VLAN,LAG TABLES
    admin_status        = "down" / "up" ; admin status
    mac                 = 12HEXDIG      ;
    alias               = 1*64VCHAR     ; alias name of the port used by LLDP and SNMP, must be unique
    description         = 1*64VCHAR     ; port description
    speed               = 1*6DIGIT      ; port line speed in Mbps
    mtu                 = 1*4DIGIT      ; port MTU
    fec                 = 1*64VCHAR     ; port fec mode
    autoneg             = BIT           ; auto-negotiation mode

### WARM\_RESTART
    ;Stores system warm start configuration
    ;Status: work in progress

    key                 = WARM_RESTART:name ; name is the name of SONiC docker or "system" for global configuration.

    neighsyncd_timer    = 1*4DIGIT          ; neighsyncd_timer is the timer used for neighsyncd during the warm restart.
                                            ; Timer is started after we restored the neighborTable to internal data structures.
                                            ; neighborsyncd then starts to read all linux kernel entries and mark the entries in
                                            ; the data structures accordingly. Once the timer is expired, we will do reconciliation
                                            ; and push the delta to appDB
                                            ; Valid value is 1-9999. 0 is invalid.

    bgp_timer           = 1*4DIGIT          ; bgp_timer holds the time interval utilized by fpmsyncd during warm-restart episodes.
                                            ; During this interval fpmsyncd will recover all the routing state previously pushed to
                                            ; AppDB, as well as all the new state coming from zebra/bgpd. Upon expiration of this
                                            ; timer, fpmsyncd will execute the reconciliation logic to eliminate all the staled
                                            ; state from AppDB. This timer should match the BGP-GR restart-timer configured within
                                            ; the elected routing-stack.
                                            ; Supported range: 1-3600.

    teamsyncd_timer     = 1*4DIGIT          ; teamsyncd_timer holds the time interval utilized by teamsyncd during warm-restart episodes.
                                            ; The timer is started when teamsyncd starts. During the timer interval teamsyncd
                                            ; will preserver all LAG interface changes, but it will not apply them. The changes
                                            ; will only be applied when the timer expired. During the changes application the stale
                                            ; LAG entries will be removed, the new LAG entries will be created.
                                            ; Supported range: 1-9999. 0 is invalid


### VXLAN\_TUNNEL
Stores vxlan tunnels configuration
Status: ready

    key       = VXLAN_TUNNEL:name               ; name is an arbitrary name of vxlan tunnel
    src_ip    = ipv4_address                    ; tunnel source IP address. Mandatory
    dst_ip    = ipv4_address                    ; tunnel destination IP address. Optional. When this attribute is omitted or equal to "0.0.0.0"
                                                ; the created tunnel will be P2MP. Otherwise the created tunnel will be P2P

### VXLAN\_TUNNEL\_MAP
Stores vxlan tunnel map configuration. Defines mapping between vxlan vni and vlan interface
Status: ready

    key       = VXLAN_TUNNEL_MAP:tunnel_name:tunnel_map_name
                                                ; tunnel_name is a reference to created vxlan tunnel
                                                ; tunnel_map_name is an arbitrary name of the map
    vni       = uint24                          ; vni id, defined for tunnel map
    vlan      = "Vlan"vlan_id                   ; name of the existing vlan interface

### NEIGH_TABLE
    ; Stores the neighbors. Defines static configuration of neighbor entries. If mac address is not specified, implementation shall resolve the mac-address for the neighbor IP.
    key           = NEIGH|PORT_TABLE.name / VLAN_INTF_TABLE.name / LAG_INTF_TABLE.name|prefix
    neigh         = 12HEXDIG         ; mac address of the neighbor (optional)
    family        = "IPv4" / "IPv6"  ; address family

## State DB schema

### PORT_TABLE
Stores information for physical switch ports managed by the switch chip. Ports to the CPU (ie: management port) and logical ports (loopback) are not declared in the PORT_TABLE. See MGMT_PORT.

    ;State for layer 2 ports
    key                 = PORT_TABLE|ifname    ; ifname must be unique across PORT,INTF,VLAN,LAG TABLES
    oper_status         = "down" / "up" ; oper status
    state               = "" / "ok"     ; port created successfully

### MGMT_PORT_TABLE
    ;State for management port, including at least one key
    key                 = MGMT_PORT_TABLE|ifname    ; ifname must be unique across PORT,INTF,VLAN,LAG TABLES
    oper_status         = "down" / "up" ; oper status

### WARM\_RESTART\_ENABLE\_TABLE
    ;Stores system warm start and docker warm start enable/disable configuration
    ;The configuration is persistent across warm reboot but not cold reboot.
    ;Status: work in progress

    key                 = WARM_RESTART_ENABLE_TABLE:name ; name is the name of SONiC docker or "system" for global configuration.

    enable              = "true" / "false"  ; Default value as false.
                                            ; If "system" warm start knob is true, docker level knob will be ignored.
                                            ; If "system" warm start knob is false, docker level knob takes effect.

### WARM\_RESTART\_TABLE
    ;Stores application and orchdameon warm start status
    ;Status: work in progress

    key             = WARM_RESTART_TABLE|process_name         ; process_name is a unique process identifier.
                                                              ; with exception of 'warm-shutdown' operation.
                                                              ; 'warm-shutdown' operation key is used to
                                                              ; track warm shutdown stages and results.
                                                              ; Added to this table to leverage the existing
                                                              ; "show warm-restart state" command.

    restore_count   = 1*10DIGIT                               ; a value between 0 and 2147483647 to keep track
                                                              ; of the number of times that an application has
                                                              ; 'restored' its state from its associated redis
                                                              ; data-store; which is equivalent to the number
                                                              ; of times an application has iterated through
                                                              ; a warm-restart cycle.

    state           = "initialized" / "restored" / "reconciled"  ; initialized: initial FSM state for processes
                                                                 ; with warm-restart capabilities turned on.
                                                                 ;
                                                                 ; restored: process restored the state previously
                                                                 ; uploaded to redis data-stores.
                                                                 ;
                                                                 ; reconciled: process reconciled 'old' and 'new'
                                                                 ; state collected in 'restored' phase. Examples:
                                                                 ; dynanic data like port state, neighbor, routes
                                                                 ; and so on.

### NEIGH_RESTORE_TABLE
    ;State for neighbor table restoring process during warm reboot
    key                 = NEIGH_RESTORE_TABLE|Flags
    restored            = "true" / "false" ; restored state

### BGP\_STATE\_TABLE
    ;Stores bgp status
    ;Status: work in progress

    key             = BGP_STATE_TABLE|family|eoiu             ; family = "IPv4" / "IPv6"  ; address family.

    state           = "unknown" / "reached" / "consumed"         ; unknown: eoiu state not fetched yet.
                                                                 ; reached: bgp eoiu done.
                                                                 ;
                                                                 ; consumed: the reached state has been consumed by application.
    timestamp       = time-stamp                                 ; "%Y-%m-%d %H:%M:%S", full-date and partial-time separated by
                                                                 ; white space.  Example: 2019-04-25 09:39:19

    ;value annotations
    date-fullyear   = 4DIGIT
    date-month      = 2DIGIT  ; 01-12
    date-mday       = 2DIGIT  ; 01-28, 01-29, 01-30, 01-31 based on
                              ; month/year
    time-hour       = 2DIGIT  ; 00-23
    time-minute     = 2DIGIT  ; 00-59
    time-second     = 2DIGIT  ; 00-58, 00-59, 00-60 based on leap second
                              ; rules

    partial-time    = time-hour ":" time-minute ":" time-second
    full-date       = date-fullyear "-" date-month "-" date-mday
    time-stamp      = full-date %x20 partial-time

### INTERFACE_TABLE
    ;State for interface status, including two types of key

    key                 = INTERFACE_TABLE|ifname    ; ifname should be Ethernet,Portchannel,Vlan,Loopback
    vrf                 = "" / vrf_name             ; interface has been created, global or vrf

    key                 = INTERFACE_TABLE|ifname|IPprefix
    state               = "ok"                      ; IP address has been set to interface

### VRF_TABLE
    ;State for vrf status, vrfmgrd has written it to app_db

    key                 = VRF_TABLE|vrf_name        ; vrf_name start with 'Vrf' or 'Vnet' prefix
    state               = "ok"                      ; vrf entry exist in app_db, if yes vrf device must exist

### VRF_OBJECT_TABLE
    ;State for vrf object status, vrf exist in vrforch

    key                 = VRF_OBJECT_TABLE|vrf_name ; vrf_name start with 'Vrf' prefix
    state               = "ok"                      ; vrf entry exist in orchagent

### BUFFER_MAX_PARAM_TABLE
    ;Available only when the switch is running in dynamic buffer model
    ;Stores the maximum available buffer on a global or per-port basis

    key                 = BUFFER_MAX_PARAM_TABLE|ifname  ; The maximum headroom of the port. The ifname should be the name of a physical port.
                          BUFFER_MAX_PARAM_TABLE|global  ; The maximum available of the system.
    mmu_size            = 1*10DIGIT                      ; The maximum available of the system. Available only when the key is "global".
    max_headroom_size   = 1*10DIGIT                      ; The maximum headroom of the port. Available only when the key is ifname.

## Configuration files
What configuration files should we have?  Do apps, orch agent each need separate files?

[port_config.ini](https://github.com/stcheng/swss/blob/mock/portsyncd/port_config.ini) - defines physical port information

portsyncd reads from port_config.ini and updates PORT_TABLE in APP_DB
