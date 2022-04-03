import time
import pytest
from port_dpb import Port
from port_dpb import DPB

@pytest.mark.usefixtures('dpb_setup_fixture')
@pytest.mark.usefixtures('dvs_vlan_manager')
class TestPortDPBVlan(object):
    def check_syslog(self, dvs, marker, log, expected_cnt):
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, log)])
        assert num.strip() >= str(expected_cnt)

    def test_dependency(self, dvs):
        vlan = "100"
        p = Port(dvs, "Ethernet0")
        p.sync_from_config_db()

        # 1. Create VLAN100
        self.dvs_vlan.create_vlan(vlan)

        # 2. Add Ethernet0 to VLAN100
        self.dvs_vlan.create_vlan_member(vlan, p.get_name())

        # 3. Add log marker to syslog
        marker = dvs.add_log_marker()

        # 4. Delete Ethernet0 from config DB. Sleep for 2 seconds.
        p.delete_from_config_db()
        time.sleep(2)

        # 5. Verify that we are waiting in portsorch for the port
        #    to be removed from VLAN, by looking at the log
        self.check_syslog(dvs, marker, "Cannot remove port as bridge port OID is present", 1)

        # 6. Also verify that port is still present in ASIC DB.
        assert(p.exists_in_asic_db() == True)

        # 7. Remove port from VLAN
        self.dvs_vlan.remove_vlan_member(vlan, p.get_name())

        # 8. Verify that port is removed from ASIC DB
        assert(p.not_exists_in_asic_db() == True)

        # 9. Re-create port Ethernet0 and verify that it is
        #    present in CONFIG, APPL, and ASIC DBs
        p.write_to_config_db()
        p.verify_config_db()
        p.verify_app_db()
        p.verify_asic_db()

        # 10. Remove VLAN100 and verify that its removed.
        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.mark.skip(reason="Failing. Under investigation")
    def test_one_port_one_vlan(self, dvs):
        dpb = DPB()
        vlan = "100"

        # 1. Create VLAN100 and add Ethernet0 as member
        self.dvs_vlan.create_vlan(vlan)
        self.dvs_vlan.create_vlan_member(vlan, "Ethernet0")
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        # 2. Delete Ethernet0 from config DB. Verify that its deleted
        #    CONFIG and APPL DB and its still present in ASIC DB
        p = Port(dvs, "Ethernet0")
        p.sync_from_config_db()
        p.delete_from_config_db()
        assert(p.exists_in_config_db() == False)
        assert(p.exists_in_app_db() == False)
        assert(p.exists_in_asic_db() == True)

        # 3. Verify that Ethernet0 gets deleted from ASIC DB once
        #    its removed from VLAN100.
        self.dvs_vlan.remove_vlan_member(vlan, "Ethernet0")
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)
        assert(p.not_exists_in_asic_db() == True)

        # 4. To simulate port breakout, 1x100G --> 4x25G, create 4 ports
        dpb.create_child_ports(dvs, p, 4)

        # 5. Add all 4 ports to VLAN100
        port_names = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        vlan_member_count = 0
        for pname in port_names:
            self.dvs_vlan.create_vlan_member(vlan, pname)
            vlan_member_count = vlan_member_count + 1
            self.dvs_vlan.get_and_verify_vlan_member_ids(vlan_member_count)

        # 6. Delete 4 ports from CONFIG DB. Verify that they are all deleted
        #    from CONFIG and APPL DB but still present in ASIC DB
        child_ports = []
        for pname in port_names:
            cp = Port(dvs, pname)
            cp.sync_from_config_db()
            cp.delete_from_config_db()
            assert(cp.exists_in_config_db() == False)
            assert(cp.exists_in_app_db() == False)
            assert(cp.exists_in_asic_db() == True)
            child_ports.append(cp)

        # 7. Remove all 4 ports from VLAN100 and verify that they all get
        #    deleted from ASIC DB
        for cp in child_ports:
            self.dvs_vlan.remove_vlan_member(vlan, cp.get_name())
            vlan_member_count = vlan_member_count - 1
            self.dvs_vlan.get_and_verify_vlan_member_ids(vlan_member_count)
            assert(cp.not_exists_in_asic_db() == True)

        # 8. Re-create Ethernet0 and verify that its created all 3 DBs
        p.write_to_config_db()
        p.verify_config_db()
        p.verify_app_db()
        p.verify_asic_db()

        # 9. Remove VLAN100 and verify the same
        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.mark.skip(reason="Failing. Under investigation")
    def test_one_port_multiple_vlan(self, dvs):

        dpb = DPB()
        vlans = ["100", "101", "102"]

        # 1. Create 3 VLANs
        for vlan in vlans:
            self.dvs_vlan.create_vlan(vlan)

        # 2. Add Ethernet0 to all 3 VLANs and verify
        for vlan in vlans:
            self.dvs_vlan.create_vlan_member(vlan, "Ethernet0")
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(vlans))

        # 3. Delete Ethernet0 from CONFIG DB. Verify that it is deleted
        #    from CONFIG and APPl DB, whereas still present in ASIC DB.
        p = Port(dvs, "Ethernet0")
        p.sync_from_config_db()
        p.delete_from_config_db()
        assert(p.exists_in_config_db() == False)
        assert(p.exists_in_app_db() == False)
        assert(p.exists_in_asic_db() == True)

        # 4. Remove Ethernet0 from one of the VLANs and verify that
        #    its still present in ASIC DB
        self.dvs_vlan.remove_vlan_member(vlans[0], "Ethernet0")
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(vlans)-1)
        assert(p.exists_in_asic_db() == True)

        # 5. Remove Ethernet0 from one more VLAN and verify that
        #    its still present in ASIC DB
        self.dvs_vlan.remove_vlan_member(vlans[1], "Ethernet0")
        self.dvs_vlan.get_and_verify_vlan_member_ids(len(vlans)-2)
        assert(p.exists_in_asic_db() == True)

        # 6. Remove Ethernet0 from last VLAN as well and verify that
        #    its deleted from ASIC DB
        self.dvs_vlan.remove_vlan_member(vlans[2], "Ethernet0")
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)
        assert(p.not_exists_in_asic_db() == True)

        # 7. To Simulate 1x40G --> 4x10G, create 4 ports
        dpb.create_child_ports(dvs, p, 4)

        # 8. To Simulate 4x10G --> 1x40G, delete all 4 ports and re-create Ethernet0
        port_names = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"]
        for pname in port_names:
            cp = Port(dvs, pname)
            cp.sync_from_config_db()
            cp.delete_from_config_db()
            assert(cp.exists_in_config_db() == False)
            assert(cp.exists_in_app_db() == False)
            assert(cp.not_exists_in_asic_db() == True)

        p.write_to_config_db()
        p.verify_config_db()
        p.verify_app_db()
        p.verify_asic_db()

        # 9. Remove all 3 VLANs and verify the same
        self.dvs_vlan.remove_vlan("100")
        self.dvs_vlan.remove_vlan("101")
        self.dvs_vlan.remove_vlan("102")
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.mark.skip(reason="Failing. Under investigation")
    def test_all_port_10_vlans(self, dvs):
        num_vlans = 10
        start_vlan = 100
        num_ports = 32
        port_names = []
        vlan_names = []

        dvs.setup_db()
        for i in range(num_ports):
            port_names.append("Ethernet" + str(i*4))

        for i in range(num_vlans):
            vlan_names.append(str(start_vlan + i))

        # 1. Create 10 VLANs
        for vlan_name in vlan_names:
            self.dvs_vlan.create_vlan(vlan_name)

        # 2. Add all 32 ports to all 10 VLANs
        for port_name in port_names:
            for vlan_name in vlan_names:
                self.dvs_vlan.create_vlan_member(vlan_name, port_name, tagging_mode = "tagged")
        self.dvs_vlan.get_and_verify_vlan_member_ids(num_ports*num_vlans)

        # 3. Do the following for each port
        #    3.1. Delete port from CONFIG DB and verify that its deleted from
        #         CONFIG DB and APPL DB but not from ASIC DB.
        #    3.2. Remove the port from all 10 VLANs and verify that it
        #         gets deleted from ASIC DB
        ports = []
        for port_name in port_names:
            p = Port(dvs, port_name)
            ports.append(p)
            p.sync_from_config_db()
            p.delete_from_config_db()
            assert(p.exists_in_config_db() == False)
            assert(p.exists_in_app_db() == False)
            assert(p.exists_in_asic_db() == True)
            for vlan_name in vlan_names:
                self.dvs_vlan.remove_vlan_member(vlan_name, port_name)

            self.dvs_vlan.get_and_verify_vlan_member_ids((num_ports*num_vlans)-(len(ports)*num_vlans))
            assert(p.not_exists_in_asic_db() == True)

        # 4. Re-create all ports and verify the same
        for p in ports:
            p.write_to_config_db()
            p.verify_config_db()
            p.verify_app_db()
            p.verify_asic_db()

        # 5. Remove all VLANs and verify the same
        for vlan_name in vlan_names:
            self.dvs_vlan.remove_vlan(vlan_name)
        self.dvs_vlan.get_and_verify_vlan_ids(0)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
