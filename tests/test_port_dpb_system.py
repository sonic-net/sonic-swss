import pytest
import json
from port_dpb import Port
from port_dpb import DPB
from dvslib.dvs_common import wait_for_result, PollingConfig

ARP_FLUSH_POLLING = PollingConfig(polling_interval=0.01, timeout=10, strict=True)
ROUTE_CHECK_POLLING = PollingConfig(polling_interval=0.01, timeout=5, strict=True)
"""
Below prefix should be same as the one specified for Ethernet8
in port_breakout_config_db.json in sonic-buildimage/platform/vs/docker-sonic-vs/
"""
Ethernet8_IP = "10.0.0.8/31"
Ethernet8_IPME = "10.0.0.8/32"

@pytest.mark.usefixtures('dpb_setup_fixture')
@pytest.mark.usefixtures('dvs_vlan_manager')
class TestPortDPBSystem(object):

    def create_l3_intf(self, dvs, interface, vrf_name):
        dvs_asic_db = dvs.get_asic_db()
        initial_entries = set(dvs_asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"))

        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"

        if len(vrf_name) == 0:
            fvs = {'NULL':'NULL'}
        else:
            fvs = {'vrf_name':vrf_name}
        dvs.get_config_db().create_entry(tbl_name, interface, fvs)

        dvs_asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", len(initial_entries)+1)
        current_entries = set(dvs_asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_l3_intf(self, dvs, interface):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"

        dvs.get_config_db().delete_entry(tbl_name, interface)

    def add_ip_address(self, dvs, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"

        dvs.get_config_db().create_entry(tbl_name, interface+'|'+ip, {'NULL':'NULL'})

    def remove_ip_address(self, dvs, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"

        dvs.get_config_db().delete_entry(tbl_name, interface+'|'+ip)

    def clear_srv_config(self, dvs):
        dvs.servers[0].runcmd("ip address flush dev eth0")
        dvs.servers[1].runcmd("ip address flush dev eth0")
        dvs.servers[2].runcmd("ip address flush dev eth0")
        dvs.servers[3].runcmd("ip address flush dev eth0")

    def set_admin_status(self, dvs, interface, status):
        dvs_cfg_db = dvs.get_config_db()
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        dvs_cfg_db.create_entry(tbl_name, interface, {'admin_status':status})

    def verify_only_ports_exist(self, dvs, port_names):
        all_port_names = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        for port_name in all_port_names:
            p = Port(dvs, port_name)
            if port_name in port_names:
                assert(p.exists_in_config_db() == True)
                assert(p.exists_in_app_db() == True)
                assert(p.exists_in_asic_db() == True)
            else:
                assert(p.exists_in_config_db() == False)
                assert(p.exists_in_app_db() == False)
                assert(p.exists_in_asic_db() == False)

    '''
    |-----------------------------------------------------------------------------------------------------
    |                   | 1X40G | 1X100G | 4X10G | 4X25G | 2X50G | 2x25G(2)+1x50G(2) | 1x50G(2)+2x25G(2) |
    |-----------------------------------------------------------------------------------------------------
    | 1X40G             |  NA   |        |       |       |       |                   |                   |
    |-----------------------------------------------------------------------------------------------------
    | 1X100G            |       |   NA   |       |  P    |  P    |        P          |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 4X10G             |       |        |  NA   |       |       |                   |                   |
    |-----------------------------------------------------------------------------------------------------
    | 4X25G             |       |   P    |       |  NA   |  P    |        P          |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 2X50G             |       |   P    |       |  P    |  NA   |        P          |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 2x25G(2)+1x50G(2) |       |   P    |       |  P    |  P    |        NA         |        P          |
    |-----------------------------------------------------------------------------------------------------
    | 1x50G(2)+2x25G(2) |       |   P    |       |  P    |  P    |        P          |        NA         |
    |-----------------------------------------------------------------------------------------------------

    NA    --> Not Applicable
    P     --> Pass
    F     --> Fail
    Empty --> Not Tested
    '''
    @pytest.mark.parametrize('root_port, breakout_mode',  [
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '2x50G'),
        ('Ethernet0', '1x100G[40G]'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '4x25G[10G]'),
        ('Ethernet0', '1x100G[40G]'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '2x25G(2)+1x50G(2)'),
        ('Ethernet0', '1x100G[40G]'),
        ('Ethernet0', '1x50G(2)+2x25G(2)'),
        ('Ethernet0', '1x100G[40G]')
    ], scope="function")
    def test_port_breakout_simple(self, dvs, root_port, breakout_mode):
        dvs.setup_db()
        dpb = DPB()

        dvs.change_port_breakout_mode(root_port, breakout_mode)
        dpb.verify_port_breakout_mode(dvs, root_port, breakout_mode)
        expected_ports = dpb.get_child_ports(root_port, breakout_mode)
        self.verify_only_ports_exist(dvs, expected_ports)

    def test_port_breakout_with_vlan(self, dvs):
        dvs.setup_db()
        dpb = DPB()

        portName = "Ethernet0"
        vlanID = "100"
        breakoutMode1 = "1x100G[40G]"
        breakoutMode2 = "4x25G[10G]"
        breakoutOption = "-f" #Force breakout by deleting dependencies

        # Create VLAN
        self.dvs_vlan.create_vlan(vlanID)

        # Verify VLAN is created
        self.dvs_vlan.get_and_verify_vlan_ids(1)

        # Add port to VLAN
        self.dvs_vlan.create_vlan_member(vlanID, portName)

        # Verify VLAN member is created
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port from 1x100G[40G] --> 4x25G[10G]
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", breakoutMode1)
        dvs.change_port_breakout_mode("Ethernet0", breakoutMode2, breakoutOption)

        # Verify DPB is successful
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", breakoutMode2)

        # Verify port is removed from VLAN
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Delete VLAN
        self.dvs_vlan.remove_vlan(vlanID)

        # Verify VLAN is deleted
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        # Breakout port from 4x25G[10G] --> 1x100G[40G]
        dvs.change_port_breakout_mode("Ethernet0", breakoutMode1)

        # Verify DPB is successful
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", breakoutMode1)

    def test_port_breakout_with_acl(self, dvs, dvs_acl):
        dvs.setup_db()
        dpb = DPB()

        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        dvs_acl.create_acl_table("test", "L3", bind_ports)

        # Verify ACL table is created
        dvs_acl.verify_acl_table_count(1)

        # Verify that ACL group OID is created.
        # Just FYI: Usually one ACL group OID is created per port,
        #           even when port is bound to multiple ACL tables
        dvs_acl.verify_acl_table_groups(1)

        # Verify that port is correctly bound to table by looking into
        # ACL member table, which binds ACL group OID of a port and
        # ACL table OID.
        acl_table_ids = dvs_acl.get_acl_table_ids(1)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        # Verify current breakout mode, perform breakout without force dependency
        # delete option
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]")

        # Verify that breakout did NOT succeed
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")

        # Do breakout with force option, and verify that it succeeds
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]", "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "4x25G[10G]")

        # Verify port is removed from ACL table
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(0)

        # Verify child ports are created.
        self.verify_only_ports_exist(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])

        # Move back to 1x100G[40G] mode and verify current mode
        dvs.change_port_breakout_mode("Ethernet0", "1x100G[40G]", "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")

        # Remove ACL table and verify the same
        dvs_acl.remove_acl_table("test")
        dvs_acl.verify_acl_table_count(0)

    @pytest.mark.skip("DEBUG: When we have more than one child ports, operation status of all does NOT go down")
    def test_cli_command_with_force_option(self, dvs, dvs_acl):
        dvs.setup_db()
        dpb = DPB()

        portGroup = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        rootPortName = portGroup[0]
        vlanID = "100"
        aclTableName = "DPB_ACL_TBL_1"
        breakoutMode1x = "1x100G[40G]"
        breakoutMode2x = "2x50G"
        breakoutMode4x = "4x25G[10G]"
        breakoutOption = "-f" #Force breakout by deleting dependencies

        # Breakout port with no dependency using "-f" option
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)

        # Breakout port with VLAN and ACL dependency

        # Create ACL table and bind port
        dvs_acl.verify_acl_table_groups(0)
        bind_ports = []
        bind_ports.append(rootPortName)
        dvs_acl.create_acl_table(aclTableName, "L3", bind_ports)
        dvs_acl.verify_acl_table_groups(1)

        # Create VLAN and add port to VLAN
        self.dvs_vlan.create_vlan(vlanID)
        self.dvs_vlan.create_vlan_member(vlanID, rootPortName)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port and make sure it succeeds and associations are removed
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Add all ports to ACL and VLAN tables
        dvs_acl.update_acl_table_port_list(aclTableName, portGroup)
        for p in portGroup:
            self.dvs_vlan.create_vlan_member(vlanID, p)
        dvs_acl.verify_acl_table_groups(len(portGroup))
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(portGroup))

        # Breakout with "-f" option and ensure it succeeds and associations are removed
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Cleanup

        # Remove ACL and VLAN tables
        dvs_acl.remove_acl_table(aclTableName)
        self.dvs_vlan.remove_vlan(vlanID)

        # Verify cleanup
        dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        # check ASIC router interface database
        # one loopback router interface
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        # Bring up port
        self.set_admin_status(dvs, "Ethernet8", "up")

        # Create L3 interface
        self.create_l3_intf(dvs, "Ethernet8", "");

        # Configure IPv4 address on Ethernet8
        self.add_ip_address(dvs, "Ethernet8", Ethernet8_IP)

        # one loopback router interface and one port based router interface
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 2)

        def _check_route_present():
            routes = dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            subnet_found = False
            ip2me_found = False
            for route in routes:
                rt = json.loads(route)
                if rt["dest"] == Ethernet8_IP:
                    subnet_found = True
                if rt["dest"] == Ethernet8_IPME:
                    ip2me_found = True

            return ((subnet_found and ip2me_found), routes)
        # check ASIC route database
        status, result = wait_for_result(_check_route_present, ROUTE_CHECK_POLLING)
        assert status == True

        # Breakout Ethernet8 WITH "-f" option and ensure cleanup happened
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode2x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)

        # check ASIC router interface database
        # one loopback router interface
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        def _check_route_absent():
            routes = dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            for route in routes:
                rt = json.loads(route)
                if rt["dest"] == Ethernet8_IP or \
                   rt["dest"] == Ethernet8_IPME:
                    return (False, route)
            return (True, routes)
        # check ASIC database
        status, result = wait_for_result(_check_route_absent, ROUTE_CHECK_POLLING)
        assert status == True

        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode1x)
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)

    @pytest.mark.skip("DEBUG: When we have more than one child ports, operation status of all does NOT go down")
    def test_cli_command_with_load_port_breakout_config_option(self, dvs, dvs_acl):
        dvs.setup_db()
        dpb = DPB()

        # Note below definitions are dependent on port_breakout_config_db.json
        # That is vlanIDs, aclTableNames are all should match with
        # VLANs and ACL tables in port_breakout_config_db.json
        portGroup = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        rootPortName = portGroup[0]
        vlanIDs = ["100", "101"]
        aclTableNames = ["DPB_ACL_TBL_1", "DPB_ACL_TBL_2"]
        breakoutMode1x = "1x100G[40G]"
        breakoutMode2x = "2x50G"
        breakoutMode4x = "4x25G[10G]"
        breakoutOption = "-l"

        # Lets create ACL and VLAN tables
        bind_ports = []
        for aclTableName in aclTableNames:
            dvs_acl.create_acl_table(aclTableName, "L3", bind_ports)
        for vlanID in vlanIDs:
            self.dvs_vlan.create_vlan(vlanID)

        # Breakout port and expect that newly created ports are
        # automatically added to VLANs and ACL tables as per
        # port_breakout_config_db.json
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs_acl.verify_acl_table_groups(len(portGroup))
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(portGroup))

        # Breakout port and expect that root port remains in VLAN and ACL tables
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption + " -f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs_acl.verify_acl_table_groups(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port with "-f" and WITHOUT "-l" and expect that
        # breakout succeeds and root port gets removed from
        # VLAN and ACL table
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, "-f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        #--------------------------------------------------------------------
        #  Exercise port group spanned across different VLAN and ACl table  |
        #--------------------------------------------------------------------
        portGroup = ["Ethernet4", "Ethernet5", "Ethernet6", "Ethernet7"]
        rootPortName = portGroup[0]
        breakoutMode2x = "2x50G"

        # Breakout port and expect that newly created ports are
        # automatically added to VLANs and ACL tables as per
        # port_breakout_config_db.json
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs_acl.verify_acl_table_groups(len(portGroup))
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(portGroup))

        # Breakout port and expect that Ethernet4 and Ethernet6 remain in
        # ACL and VLAN where as Ethernet5 and Ethernet7 get removed from
        # ACL and VLAN table
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode2x, breakoutOption + " -f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode2x)
        dvs_acl.verify_acl_table_groups(2)
        self.dvs_vlan.get_and_verify_vlan_member_ids(2)

        # Breakout again and verify that only root port (Ethernet4) remains in
        # in VLAN and ACL and Ethernet6 gets removed.
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode2x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, breakoutOption + " -f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs_acl.verify_acl_table_groups(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port without "-l" option and ensure that root port
        # gets removed from VLAN and ACL
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode2x, "-f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode2x)
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        #Cleanup

        # Move both Ethernet0 and Ethernet4 back to default mode
        dvs.change_port_breakout_mode("Ethernet0", breakoutMode1x)
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", breakoutMode1x)
        dvs.change_port_breakout_mode("Ethernet4", breakoutMode1x)
        dpb.verify_port_breakout_mode(dvs, "Ethernet4", breakoutMode1x)

        # Delete VLANs and ACL tables
        bind_ports = []
        for aclTableName in aclTableNames:
            dvs_acl.remove_acl_table(aclTableName)
        for vlanID in vlanIDs:
            self.dvs_vlan.remove_vlan(vlanID)

        # Verify cleanup
        dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        ##### Interface dependency test ############

        # check ASIC router interface database
        # one loopback router interface
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        # Breakout Ethernet8 WITH "-l" option and ensure
        # ip address gets configured as per port_breakout_config_db.json
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode2x, breakoutOption)
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)

        # one loopback router interface and one port based router interface
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 2)

        def _check_route_present():
            routes = dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            subnet_found = False
            ip2me_found = False
            for route in routes:
                rt = json.loads(route)
                if rt["dest"] == Ethernet8_IP:
                    subnet_found = True
                if rt["dest"] == Ethernet8_IPME:
                    ip2me_found = True

            return ((subnet_found and ip2me_found), routes)
        # check ASIC route database
        status, result = wait_for_result(_check_route_present, ROUTE_CHECK_POLLING)
        assert status == True

        # Breakout Ethernet8 WITH "-f" option and ensure cleanup happened
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode2x)
        dvs.change_port_breakout_mode("Ethernet8", breakoutMode1x, "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet8", breakoutMode1x)

        # check ASIC router interface database
        # one loopback router interface
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        def _check_route_absent():
            routes = dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            for route in routes:
                rt = json.loads(route)
                if rt["dest"] == Ethernet8_IP or \
                   rt["dest"] == Ethernet8_IPME:
                    return (False, route)
            return (True, routes)
        # check ASIC database
        status, result = wait_for_result(_check_route_absent, ROUTE_CHECK_POLLING)
        assert status == True

    def test_cli_command_negative(self, dvs, dvs_acl):
        dvs.setup_db()
        dpb = DPB()

        portGroup = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        rootPortName = portGroup[0]
        vlanIDs = ["100", "101"]
        aclTableNames = ["DPB_ACL_TBL_1", "DPB_ACL_TBL_2"]
        breakoutMode1x = "1x100G[40G]"
        breakoutMode4x = "4x25G[10G]"

        # Create only one ACL table and one VLAN table
        bind_ports = []
        dvs_acl.create_acl_table(aclTableNames[0], "L3", bind_ports)
        self.dvs_vlan.create_vlan(vlanIDs[0])

        # Add root port to ACL and VLAN tables
        bind_ports = []
        bind_ports.append(rootPortName)
        dvs_acl.update_acl_table_port_list(aclTableNames[0], bind_ports)
        self.dvs_vlan.create_vlan_member(vlanIDs[0], rootPortName)

        # Breakout port WITHOUT "-f" option when dependencies exist
        # TBD: Verify the list of dependencies returned by CLI command
        dvs_acl.verify_acl_table_groups(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs_acl.verify_acl_table_groups(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Breakout port WITH "-f" option, and WITHOUT "-l" option
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode4x, "-f")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Delete VLAN table, ensure breakout WITH "-l" fails
        self.dvs_vlan.remove_vlan(vlanIDs[0])
        self.dvs_vlan.get_and_verify_vlan_ids(0)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, "-l")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Delete ACL table, Add back VLAN table and
        # ensure breakout WITH "-l" fails
        dvs_acl.remove_acl_table(aclTableNames[0])
        dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.create_vlan(vlanIDs[0])
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, "-l")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs_acl.verify_acl_table_groups(0)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Create both ACL tables (as per port_breakout_config_db.json,
        # Ethernet0 is in both ACL tables and one VLAN table)
        # and ensure, breakout succeeds
        bind_ports = []
        dvs_acl.create_acl_table(aclTableNames[0], "L3", bind_ports)
        dvs_acl.create_acl_table(aclTableNames[1], "L3", bind_ports)
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode4x)
        dvs.change_port_breakout_mode(rootPortName, breakoutMode1x, "-l")
        dpb.verify_port_breakout_mode(dvs, rootPortName, breakoutMode1x)
        dvs_acl.verify_acl_table_groups(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # Delete ACL and VLAN tables
        self.dvs_vlan.remove_vlan_member(vlanIDs[0], rootPortName)
        self.dvs_vlan.remove_vlan(vlanIDs[0])
        dvs_acl.remove_acl_table(aclTableNames[0])
        dvs_acl.remove_acl_table(aclTableNames[1])

        # TBD: Provide "-l" option without port_breakout_config_db.json file

        # Verify cleanup
        dvs_acl.verify_acl_table_count(0)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    def test_dpb_arp_flush(self, dvs):
        dvs.setup_db()
        dvs_asic_db = dvs.get_asic_db()
        dpb = DPB()

        portName = "Ethernet0"
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.clear_srv_config(dvs)

        # Create l3 interface
        rif_oid = self.create_l3_intf(dvs, portName, vrfName)

        # set ip address
        self.add_ip_address(dvs, portName, ipAddress)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 3 10.0.0.0")

        intf_entries = dvs_asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", 1)
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        dvs_asic_db.wait_for_exact_match("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                         intf_entries[0], \
                                         {"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":srv0MAC})

        # Breakout port and make sure NEIGHBOR entry is removed
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]", "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "4x25G[10G]")

        #Verify ARP/Neighbor entry is removed
        dvs_asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                           intf_entries[0], ARP_FLUSH_POLLING)

        dvs.change_port_breakout_mode("Ethernet0", "1x100G[40G]")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")

    def test_dpb_arp_flush_vlan(self, dvs):
        dvs.setup_db()
        dvs_asic_db = dvs.get_asic_db()
        dpb = DPB()

        self.clear_srv_config(dvs)
        vlanID = "100"
        portName = "Ethernet0"
        vlanName = "Vlan" + str(vlanID)
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.dvs_vlan.create_vlan(vlanID)

        self.dvs_vlan.create_vlan_member(vlanID, portName)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")
        self.set_admin_status(dvs, vlanName, "up")

        # create vlan interface
        rif_oid = self.create_l3_intf(dvs, vlanName, vrfName)

        # assign IP to interface
        self.add_ip_address(dvs, vlanName, ipAddress)

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.0")

        intf_entries = dvs_asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", 1)
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        dvs_asic_db.wait_for_exact_match("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                         intf_entries[0], \
                                         {"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":srv0MAC})

        # Breakout port and make sure NEIGHBOR entry is removed
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")
        dvs.change_port_breakout_mode("Ethernet0", "4x25G[10G]", "-f")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "4x25G[10G]")

        #Verify ARP/Neighbor entry is removed
        dvs_asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                           intf_entries[0], ARP_FLUSH_POLLING)

        dvs.change_port_breakout_mode("Ethernet0", "1x100G[40G]")
        dpb.verify_port_breakout_mode(dvs, "Ethernet0", "1x100G[40G]")

        # Remove IP from interface, and then remove interface
        self.remove_ip_address(dvs, vlanName, ipAddress)
        self.remove_l3_intf(dvs, vlanName)

        # Remove VLAN(note that member was removed during port breakout)
        self.dvs_vlan.remove_vlan(vlanID)

    def test_dpb_arp_flush_on_port_oper_shut(self, dvs):
        dvs.setup_db()
        dvs_asic_db = dvs.get_asic_db()
        dpb = DPB()

        self.clear_srv_config(dvs)
        vlanID = "100"
        portName = "Ethernet0"
        vlanName = "Vlan" + str(vlanID)
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.dvs_vlan.create_vlan(vlanID)

        self.dvs_vlan.create_vlan_member(vlanID, portName)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")
        self.set_admin_status(dvs, vlanName, "up")

        # create vlan interface
        rif_oid = self.create_l3_intf(dvs, vlanName, vrfName)

        # assign IP to interface
        self.add_ip_address(dvs, vlanName, ipAddress)

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 3 10.0.0.0")

        intf_entries = dvs_asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", 1)
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        dvs_asic_db.wait_for_exact_match("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                         intf_entries[0], \
                                         {"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":srv0MAC})

        # Bring link operation state down
        self.set_admin_status(dvs, portName, "down")
        dvs.servers[0].runcmd("ip link set dev eth0 down")

        #Verify ARP/Neighbor entry is removed
        dvs_asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                           intf_entries[0], ARP_FLUSH_POLLING)

        # Bring link operation state up
        self.set_admin_status(dvs, portName, "up")
        dvs.servers[0].runcmd("ip link set dev eth0 up")

        # Remove IP from interface, and then remove interface
        self.remove_ip_address(dvs, vlanName, ipAddress)
        self.remove_l3_intf(dvs, vlanName)

        # Remove VLAN member and VLAN
        self.dvs_vlan.remove_vlan_member(vlanID, portName)
        self.dvs_vlan.remove_vlan(vlanID)

    def test_dpb_arp_flush_on_vlan_member_remove(self, dvs):
        dvs.setup_db()
        dvs_asic_db = dvs.get_asic_db()
        dpb = DPB()

        self.clear_srv_config(dvs)
        vlanID = "100"
        portName = "Ethernet0"
        vlanName = "Vlan" + str(vlanID)
        vrfName = ""
        ipAddress = "10.0.0.0/31"
        srv0MAC = "00:00:00:00:01:11"

        self.dvs_vlan.create_vlan(vlanID)

        self.dvs_vlan.create_vlan_member(vlanID, portName)

        # bring up interface
        self.set_admin_status(dvs, portName, "up")
        self.set_admin_status(dvs, vlanName, "up")

        # create vlan interface
        rif_oid = self.create_l3_intf(dvs, vlanName, vrfName)

        # assign IP to interface
        self.add_ip_address(dvs, vlanName, ipAddress)

        # Set IP address and default route
        cmd = "ip link set eth0 address " + srv0MAC
        dvs.servers[0].runcmd(cmd)
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        # Get neighbor and ARP entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.0")

        intf_entries = dvs_asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", 1)
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.1"
        assert route["rif"] == rif_oid
        dvs_asic_db.wait_for_exact_match("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                         intf_entries[0], \
                                         {"SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":srv0MAC})

        # Remove port from VLAN
        self.dvs_vlan.remove_vlan_member(vlanID, portName)

        #Verify ARP/Neighbor entry is removed
        dvs_asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", \
                                           intf_entries[0], ARP_FLUSH_POLLING)

        # Remove IP from interface, and then remove interface
        self.remove_ip_address(dvs, vlanName, ipAddress)
        self.remove_l3_intf(dvs, vlanName)

        # Remove VLAN
        self.dvs_vlan.remove_vlan(vlanID)
