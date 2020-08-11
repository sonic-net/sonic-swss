import pytest

from port_dpb import DPB

maxPorts = 32
maxBreakout = 4
maxRootPorts = maxPorts/maxBreakout
maxAclTables = 16


@pytest.mark.usefixtures('dpb_setup_fixture')
class TestPortDPBAcl(object):
    def test_acl_table_empty_port_list(self, dvs_acl):
        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = []
        dvs_acl.create_acl_table("test", "L3", bind_ports)
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(0)

        bind_ports = ["Ethernet0"]
        dvs_acl.update_acl_table_port_list("test", bind_ports)

        # Verify table, group, and member have been created
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(1)
        acl_table_ids = dvs_acl.get_acl_table_ids(1)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        bind_ports = []
        dvs_acl.update_acl_table_port_list("test", bind_ports)
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(0)

    def test_one_port_two_acl_tables(self, dvs_acl):
        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        dvs_acl.create_acl_table("test", "L3", bind_ports)
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(1)
        acl_table_ids = dvs_acl.get_acl_table_ids(1)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        # Create ACL table "test1" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        dvs_acl.create_acl_table("test1", "L3", bind_ports)
        dvs_acl.verify_acl_table_count(2)
        dvs_acl.verify_acl_table_groups(1)
        acl_table_ids = dvs_acl.get_acl_table_ids(2)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 2)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[1], bind_ports, 2)

        # Delete ACL tables
        dvs_acl.remove_acl_table("test")
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(1)

        dvs_acl.remove_acl_table("test1")
        dvs_acl.verify_acl_table_count(0)
        dvs_acl.verify_acl_table_groups(0)

    def test_one_acl_table_many_ports(self, dvs, dvs_acl):
        # Create ACL table and bind it to Ethernet0 and Ethernet4
        bind_ports = ["Ethernet0", "Ethernet4"]
        dvs_acl.create_acl_table("test", "L3", bind_ports)
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(2)
        acl_table_ids = dvs_acl.get_acl_table_ids(1)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        # Update bind list and verify
        bind_ports = ["Ethernet4"]
        dvs_acl.update_acl_table_port_list("test", bind_ports)
        dvs_acl.verify_acl_table_groups(1)
        acl_table_ids = dvs_acl.get_acl_table_ids(1)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        # Breakout Ethernet0
        dpb = DPB()
        dpb.breakout(dvs, "Ethernet0", maxBreakout)

        # Update bind list and verify
        bind_ports = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3", "Ethernet4"]
        dvs_acl.update_acl_table_port_list("test", bind_ports)
        dvs_acl.verify_acl_table_groups(5)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        # Update bind list and verify
        bind_ports = ["Ethernet4"]
        dvs_acl.update_acl_table_port_list("test", bind_ports)
        dvs_acl.verify_acl_table_groups(1)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        # Breakin Ethernet0, 1, 2, 3
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])

        # Update bind list and verify
        bind_ports = ["Ethernet0", "Ethernet4"]
        dvs_acl.update_acl_table_port_list("test", bind_ports)
        dvs_acl.verify_acl_table_groups(2)
        dvs_acl.verify_acl_table_port_binding(acl_table_ids[0], bind_ports, 1)

        # Delete ACL table
        dvs_acl.remove_acl_table("test")
        dvs_acl.verify_acl_table_groups(0)

    def test_one_port_many_acl_tables(self, dvs, dvs_acl):
        # Create 4 ACL tables and bind them to Ethernet0
        bind_ports = ["Ethernet0"]
        acl_tables = ["test1", "test2", "test3", "test4"]
        for acl_tbl in acl_tables:
            dvs_acl.create_acl_table(acl_tbl, "L3", bind_ports)

        dvs_acl.verify_acl_table_count(len(acl_tables))
        dvs_acl.verify_acl_table_groups(len(bind_ports))
        acl_table_ids = dvs_acl.get_acl_table_ids(len(acl_tables))
        for acl_tbl_id in acl_table_ids:
            dvs_acl.verify_acl_table_port_binding(acl_tbl_id, bind_ports, len(acl_tables))

        # Update bind list and verify
        bind_ports = []
        for acl_tbl in acl_tables:
            dvs_acl.update_acl_table_port_list(acl_tbl, bind_ports)

        dvs_acl.verify_acl_table_groups(0)

        # Breakout Ethernet0
        dpb = DPB()
        dpb.breakout(dvs, "Ethernet0", maxBreakout)

        # Breakin Ethernet0, 1, 2, 3
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])

        for acl_tbl in acl_tables:
            dvs_acl.remove_acl_table(acl_tbl)

    def test_many_ports_many_acl_tables(self, dvs, dvs_acl):
        # Prepare ACL table names
        aclTableNames = []
        for i in range(maxAclTables):
            aclTableNames.append("aclTable" + str(i+1))

        # Prepare all port names
        portNames = []
        for i in range(maxPorts):
            portNames.append("Ethernet" + str(i))

        # Prepare root port names
        rootPortNames = []
        for i in range(0, maxPorts, maxBreakout):
            rootPortNames.append("Ethernet" + str(i))

        # Create ACL tables and bind root ports
        for aclTable in aclTableNames:
            dvs_acl.create_acl_table(aclTable, "L3", rootPortNames)
        dvs_acl.verify_acl_table_groups(maxRootPorts)

        # Remove the dependency on all root ports by
        # unbinding them from all ACL tables.
        bind_ports = []
        for aclTable in aclTableNames:
            dvs_acl.update_acl_table_port_list(aclTable, bind_ports)
        dvs_acl.verify_acl_table_groups(0)

        # Breakout all root ports
        dpb = DPB()
        for pName in rootPortNames:
            dpb.breakout(dvs, pName, maxBreakout)

        # Add all ports to aclTable1
        dvs_acl.update_acl_table_port_list(aclTableNames[0], portNames)
        dvs_acl.verify_acl_table_groups(maxPorts)

        # Remove all ports from aclTable1
        bind_ports = []
        dvs_acl.update_acl_table_port_list(aclTableNames[0], bind_ports)
        dvs_acl.verify_acl_table_groups(0)

        # Breakin all ports
        for i in range(0, maxPorts, maxBreakout):
            dpb.breakin(dvs, portNames[i:i+maxBreakout])

        for aclTable in aclTableNames:
            dvs_acl.remove_acl_table(aclTable)
        dvs_acl.verify_acl_table_count(0)
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
