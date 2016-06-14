Schema data is defined in ABNF [RFC5234](https://tools.ietf.org/html/rfc5234) syntax.  

###Definitions of common tokens
	name					= 1*DIGIT/1*ALPHA
	ref_hash_key_reference	= "[" hash_key "]" ;The token is a refernce to another valid DB key.
	hash_key				= name ; a valid key name (i.e. exists in DB)


###PORT_TABLE
Stores information for physical switch ports managed by the switch chip.  device_names are defined in [port_config.ini](../portsyncd/port_config.ini).  Ports to the CPU (ie: management port) and logical ports (loopback) are not declared in the PORT_TABLE.   See INTF_TABLE.

    ;Defines layer 2 ports
    ;In SONiC, Data is loaded from configuration file by portsyncd
    ;Status: Mandatory
    port_table_key      = PORT_TABLE:ifname    ; ifname must be unique across PORT,INTF,VLAN,LAG TABLES
    device_name         = 1*64VCHAR     ; must be unique across PORT,INTF,VLAN,LAG TABLES and must map to PORT_TABLE.name
    admin_status	    = BIT           ; is the port enabled (1) or disabled (0)
    oper_status         = BIT           ; physical status up (1) or down (0) of the link attached to this port
    lanes               = list of lanes ; (need format spec???)
    ifname              = 1*64VCHAR     ; name of the port, must be unique 
    mac                 = 12HEXDIG      ; 

	;QOS Mappings
	map_dscp_to_tc		= ref_hash_key_reference
	map_tc_to_queue		= ref_hash_key_reference
	
	Example:
	127.0.0.1:6379> hgetall PORT_TABLE:ETHERNET4
	1) "dscp_to_tc_map"
	2) "[DSCP_TO_TC_MAP_TABLE:AZURE]"
	3) "tc_to_queue_map"
	4) "[TC_TO_QUEUE_MAP_TABLE:AZURE]"

---------------------------------------------
###INTF_TABLE
intfsyncd manages this table.  In SONiC, CPU (management) and logical ports (vlan, loopback, LAG) are declared in /etc/network/interface and loaded into the INTF_TABLE.

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
###VLAN_TABLE
    ;Defines VLANs and the interfaces which are members of the vlan
    ;Status: work in progress
    key                 = VLAN_TABLE:"vlan"vlanid ; DIGIT 0-4095 with prefix "vlan"
    admin_status        = "down" / "up"        ; admin status
    oper_status         = "down" / "up"        ; operating status
    mtu                 = 1*4DIGIT             ; MTU for the IP interface of the VLAN

    key                 = VLAN_TABLE:vlanid:ifname ; physical port member of VLAN
    tagging_mode        = "untagged" / "tagged" / "priority_tagged" ; default value as untagged


---------------------------------------------
###LAG_TABLE
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
    oper_status         = "down" / "up"        ; Oper status (physical + 802.3ad state)
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
    1) "linkup"
    2) "down"
    3) "speed"
    4) "0Mbit"
    5) "duplex"
    6) "half"

---------------------------------------------
###ROUTE_TABLE
    ;Stores a list of routes
    ;Status: Mandatory
    key           = ROUTE_TABLE:prefix
    nexthop       = *prefix, ;IP addresses separated “,” (empty indicates no gateway)
    intf          = ifindex? PORT_TABLE.key  ; zero or more separated by “,” (zero indicates no interface)
    blackhole     = BIT ; Set to 1 if this route is a blackhole (or null0)
  
---------------------------------------------
###NEIGH_TABLE
    ; Stores the neighbors or next hop IP address and output port or 
    ; interface for routes
    ; Note: neighbor_sync process will resolve mac addr for neighbors 
    ; using libnl to get neighbor table
    ;Status: Mandatory
    key           = prefix PORT_TABLE.name / VLAN_INTF_TABLE.name / LAG_INTF_TABLE.name = macaddress ; (may be empty)
    neigh         = 12HEXDIG         ;  mac address of the neighbor 
    family        = "IPv4" / "IPv6"  ; address family

---------------------------------------------
###QUEUE_TABLE

	; QUEUE table. Defines port queue.
	; SAI mapping - port queue.

	key					= "QUEUE_TABLE:"port_name":queue_index
	queue_index			= 1*DIGIT
	port_name			= ifName
	queue_reference		= ref_hash_key_reference

	;field					value
	scheduler			= ref_hash_key_reference; reference to scheduler key
	wred_profile		= ref_hash_key_reference; reference to wred profile key

	Example:
	127.0.0.1:6379> hgetall QUEUE_TABLE:ETHERNET4:1
	1) "scheduler"
	2) "[SCHEDULER_TABLE:BEST_EFFORT]"
	3) "wred_profile"
	4) "[WRED_PROFILE_TABLE:AZURE]"

---------------------------------------------
###TC\_TO\_QUEUE\_MAP\_TABLE
	; TC to queue map
	;SAI mapping - qos_map with SAI_QOS_MAP_ATTR_TYPE == SAI_QOS_MAP_TC_TO_QUEUE. See saiqosmaps.h
	key					= "TC_TO_QUEUE_MAP_TABLE:"name
	;field
	tc_num				= 1*DIGIT
	;values
	queue				= 1*DIGIT; queue index
	
	Example:
	27.0.0.1:6379> hgetall TC_TO_QUEUE_MAP_TABLE:AZURE
	1) "5" ;tc
	2) "1" ;queue index
	3) "6" 
	4) "1" 

---------------------------------------------
###DSCP\_TO\_TC\_MAP\_TABLE
	; dscp to TC map
	;SAI mapping - qos_map object with SAI_QOS_MAP_ATTR_TYPE == sai_qos_map_type_t::SAI_QOS_MAP_DSCP_TO_TC
	key					= "DSCP_TO_TC_MAP_TABLE:"name
	;field				value
	dscp_value			= 1*DIGIT
	tc_value			= 1*DIGIT
	
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
###SCHEDULER_TABLE
	; Scheduler table
	; SAI mapping - saicheduler.h
	key					= "SCHEDULER_TABLE":name
	; field						value
	type			= "DWRR"/"WRR"/"PRIORITY"
	weight				= 1*DIGIT
	priority			= 1*DIGIT
	
	Example:
	127.0.0.1:6379> hgetall SCHEDULER_TABLE:BEST_EFFORT
	1) "type"
	2) "PRIORITY"
	3) "priority"
	4) "7"
	127.0.0.1:6379> hgetall SCHEDULER_TABLE:SCAVENGER
	1) "type"
	2) "DWRR"
	3) "weight"
	4) "35"

---------------------------------------------
###WRED\_PROFILE\_TABLE
	; WRED profile
	; SAI mapping - saiwred.h
	key						= "WRED_PROFILE_TABLE:"name
	;field						value
	yellow_max_threshold	= byte_count
	green_max_threshold		= byte_count
	byte_count				= 1*DIGIT
	
	Example:
	127.0.0.1:6379> hgetall "WRED_PROFILE_TABLE:AZURE"
	1) "green_max_threshold"
	2) "20480"
	3) "yellow_max_threshold"
	4) "30720"

----------------------------------------------

###Configuration files
What configuration files should we have?  Do apps, orch agent each need separate files?  

[port_config.ini](https://github.com/stcheng/swss/blob/mock/portsyncd/port_config.ini) - defines physical port information  

portsyncd reads from port_config.ini and updates PORT_TABLE in APP_DB

All other apps (intfsyncd) read from PORT_TABLE in APP_DB

