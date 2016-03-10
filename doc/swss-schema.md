Schema data is defined in ABNF [RFC5234](https://tools.ietf.org/html/rfc5234) syntax.  

###PORT_TABLE
Stores information for physical switch ports managed by the switch chip.  device_names are defined in [port_config.ini](https://github.com/stcheng/swss/blob/mock/portsyncd/port_config.ini).  Ports to the CPU (ie: management port) and logical ports (loopback) are not declared in the PORT_TABLE.   See INTF_TABLE.

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


###VLAN_TABLE
    ;Defines VLANs and the interfaces which are members of the vlan
    ;Status: work in progress
    key                 = DEVICE_NAME    ; must be unique across PORT,INTF,VLAN,LAG TABLES
    vland_id            = DIGIT 0-4095
    admin_status        = BIT ; administrative status for vlan up or down
    attach_to           = PORT_TABLE.key

###LAG_TABLE
    ;a logical, link aggregation group interface (802.3ad) made of one or more ports
    ;In SONiC, data is loaded by teamsyncd
    ;Status: work in progress
    key                 = LAG_TABLE:lagname    ; logical 802.3ad LAG interface
    minimum_links       = 1*2DIGIT             ; to be implemented
    admin_status        = "down" / "up"        ; Admin status
    oper_status         = "down" / "up"        ; Oper status (physical + protocol state)
    if_mtu              = 1*4DIGIT   ; MTU for this object
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


###ROUTE_TABLE
    ;Stores a list of routes
    ;Status: Mandatory
    key           = ROUTE_TABLE:prefix
    nexthop       = *prefix, ;IP addresses separated “,” (empty indicates no gateway)
    intf          = ifindex? PORT_TABLE.key  ; zero or more separated by “,” (zero indicates no interface)
    blackhole     = BIT ; Set to 1 if this route is a blackhole (or null0)
  

###NEIGH_TABLE
    ; Stores the neighbors or next hop IP address and output port or 
    ; interface for routes
    ; Note: neighbor_sync process will resolve mac addr for neighbors 
    ; using libnl to get neighbor table
    ;Status: Mandatory
    key           = prefix PORT_TABLE.name / VLAN_INTF_TABLE.name / LAG_INTF_TABLE.name = macaddress ; (may be empty)
    neigh         = 12HEXDIG         ;  mac address of the neighbor 
    family        = "IPv4" / "IPv6"  ; address family


###Configuration files
What configuration files should we have?  Do apps, orch agent each need separate files?  

[port_config.ini](https://github.com/stcheng/swss/blob/mock/portsyncd/port_config.ini) - defines physical port information  

portsyncd reads from port_config.ini and updates PORT_TABLE in APP_DB

All other apps (intfsyncd) read from PORT_TABLE in APP_DB

