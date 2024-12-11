import os
import re
import sys
import time
import json
import pytest
import ipaddress

from swsscommon import swsscommon

class TestNextHopGroupBase(object):
    ASIC_NHS_STR = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    ASIC_NHG_STR = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP"
    ASIC_NHG_MAP_STR = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MAP"
    ASIC_NHGM_STR = ASIC_NHG_STR + "_MEMBER"
    ASIC_RT_STR = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
    ASIC_INSEG_STR = "ASIC_STATE:SAI_OBJECT_TYPE_INSEG_ENTRY"

    def get_route_id(self, prefix):
        for k in self.asic_db.get_keys(self.ASIC_RT_STR):
            if json.loads(k)['dest'] == prefix:
                return k

        return None

    def get_inseg_id(self, label):
        for k in self.asic_db.get_keys(self.ASIC_INSEG_STR):
            if json.loads(k)['label'] == label:
                return k

        return None

    def get_nhg_id(self, nhg_index):
        # Add a route with the given index, then retrieve the next hop group ID
        # from that route
        asic_rts_count = len(self.asic_db.get_keys(self.ASIC_RT_STR))

        fvs = swsscommon.FieldValuePairs([('nexthop_group', nhg_index)])
        prefix = '255.255.255.255/24'
        ps = swsscommon.ProducerStateTable(self.dvs.get_app_db().db_connection, swsscommon.APP_ROUTE_TABLE_NAME)
        ps.set(prefix, fvs)

        # Assert the route is created
        try:
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count + 1)
        except Exception as e:
            ps._del(prefix)
            return None
        else:
            # Get the route ID for the created route
            rt_id = self.get_route_id(prefix)
            assert rt_id != None

            # Get the NHGID
            nhgid = self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]
            ps._del(prefix)
            self.asic_db.wait_for_deleted_entry(self.ASIC_RT_STR, rt_id)

            return nhgid

    def get_nhgm_ids(self, nhg_index):
        nhgid = self.get_nhg_id(nhg_index)
        nhgms = []

        for k in self.asic_db.get_keys(self.ASIC_NHGM_STR):
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)

            # Sometimes some of the NHGMs have no fvs for some reason, so
            # we skip those
            try:
                if fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID'] == nhgid:
                    nhgms.append(k)
            except KeyError as e:
                pass

        return nhgms

    def get_nhg_map_id(self, nhg_map_index):
        nhg_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_NEXTHOP_GROUP_TABLE_NAME)
        cbf_nhg_ps = swsscommon.ProducerStateTable(self.app_db.db_connection,
                                            swsscommon.APP_CLASS_BASED_NEXT_HOP_GROUP_TABLE_NAME)

        asic_nhgs_count = len(self.asic_db.get_keys(self.ASIC_NHG_STR))

        # Create a NHG
        fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1'), ('ifname', 'Ethernet0')])
        nhg_ps.set('_testnhg', fvs)
        fvs = swsscommon.FieldValuePairs([('nexthop_group', '_testnhg')])
        nhg_ps.set('testnhg', fvs)

        # Add a CBF NHG pointing to the given map
        fvs = swsscommon.FieldValuePairs([('members', 'testnhg'), ('selection_map', nhg_map_index)])
        cbf_nhg_ps.set('testcbfnhg', fvs)
        time.sleep(1)

        # If the CBF NHG can't be created, the provided NHG map index is invalid
        try:
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        except Exception as e:
            # Remove the added NHGs
            cbf_nhg_ps._del('testcbfnhg')
            nhg_ps._del('testnhg')
            nhg_ps._del('_testnhg')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)
            return None

        # Get the CBF NHG ID
        cbf_nhg_id = self.get_nhg_id('testcbfnhg')
        assert cbf_nhg_id != None

        # Get the NHG map id
        nhg_map_id = self.asic_db.get_entry(self.ASIC_NHG_STR, cbf_nhg_id)['SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP']

        # Remove the added NHGs
        cbf_nhg_ps._del('testcbfnhg')
        nhg_ps._del('testnhg')
        nhg_ps._del('_testnhg')
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)

        return nhg_map_id

    def port_name(self, i):
        return "Ethernet" + str(i * 4)

    def port_ip(self, i):
        return "10.0.0." + str(i * 2)

    def port_ipv6(self, i):
        return "fc00::" + str(hex((i * 2)))[2:]
    
    def port_ipprefix(self, i):
        return self.port_ip(i) + "/31"

    def port_ipv6prefix(self, i):
        return self.port_ipv6(i) + "/126"

    def peer_ip(self, i):
        return "10.0.0." + str(i * 2 + 1)

    def peer_ipv6(self, i):
        return "fc00::" + str(hex((i * 2 + 1)))[2:]

    def port_mac(self, i):
        return "00:00:00:00:00:0" + str(i + 1)

    def config_intf(self, i, is_ipv6_needed=False):
        fvs = {'NULL': 'NULL'}

        self.config_db.create_entry("INTERFACE", self.port_name(i), fvs)
        self.config_db.create_entry("INTERFACE", "{}|{}".format(self.port_name(i), self.port_ipprefix(i)), fvs)
        if is_ipv6_needed:
            self.config_db.create_entry("INTERFACE", "{}|{}".format(self.port_name(i), self.port_ipv6prefix(i)), fvs)

        self.dvs.port_admin_set(self.port_name(i), "up")
        self.dvs.runcmd("arp -s {} {}".format(self.peer_ip(i), self.port_mac(i)))
        if is_ipv6_needed:
            command = "ip -6 neighbor replace {} lladdr {} dev {}".format(self.peer_ipv6(i), 
                                                                      self.port_mac(i), self.port_name(i))
            self.dvs.runcmd(command)
        assert self.dvs.servers[i].runcmd("ip link set down dev eth0") == 0
        assert self.dvs.servers[i].runcmd("ip link set up dev eth0") == 0

    def flap_intf(self, i, status):
        assert status in ['up', 'down']

        self.dvs.servers[i].runcmd("ip link set {} dev eth0".format(status)) == 0
        time.sleep(2)
        fvs = self.dvs.get_app_db().get_entry("PORT_TABLE", "Ethernet%d" % (i * 4))
        assert bool(fvs)
        assert fvs["oper_status"] == status

    # BFD utilities for static route BFD and ecmp acceleration -- begin
    def get_exist_bfd_session(self):
        return set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION"))

    def create_bfd_session(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.app_db.db_connection, "BFD_SESSION_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_bfd_session(self, key):
        tbl = swsscommon.ProducerStateTable(self.app_db.db_connection, "BFD_SESSION_TABLE")
        tbl._del(key)

    def check_asic_bfd_session_value(self, key, expected_values):
        fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", key)
        for k, v in expected_values.items():
            assert fvs[k] == v

    def check_state_bfd_session_value(self, key, expected_values):
        fvs = self.state_db.get_entry("BFD_SESSION_TABLE", key)
        for k, v in expected_values.items():
            assert fvs[k] == v

    def update_bfd_session_state(self, dvs, session, state):
        bfd_sai_state = {"Admin_Down":  "SAI_BFD_SESSION_STATE_ADMIN_DOWN",
                         "Down":        "SAI_BFD_SESSION_STATE_DOWN",
                         "Init":        "SAI_BFD_SESSION_STATE_INIT",
                         "Up":          "SAI_BFD_SESSION_STATE_UP"}

        ntf = swsscommon.NotificationProducer(self.asic_db.db_connection, "NOTIFICATIONS")
        fvp = swsscommon.FieldValuePairs()
        ntf_data = "[{\"bfd_session_id\":\""+session+"\",\"session_state\":\""+bfd_sai_state[state]+"\"}]"
        ntf.send("bfd_session_state_change", ntf_data, fvp)
    # BFD utilities for static route BFD and ecmp acceleration -- end

    def init_test(self, dvs, num_intfs, is_ipv6_needed=False):
        self.dvs = dvs
        self.app_db = self.dvs.get_app_db()
        self.asic_db = self.dvs.get_asic_db()
        self.config_db = self.dvs.get_config_db()
        self.state_db = self.dvs.get_state_db()
        self.nhg_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_NEXTHOP_GROUP_TABLE_NAME)
        self.rt_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_ROUTE_TABLE_NAME)
        self.lr_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_LABEL_ROUTE_TABLE_NAME)
        self.cbf_nhg_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_CLASS_BASED_NEXT_HOP_GROUP_TABLE_NAME)
        self.fc_to_nhg_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_FC_TO_NHG_INDEX_MAP_TABLE_NAME)
        self.switch_ps =  swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_SWITCH_TABLE_NAME)

        # Set switch FC capability to 63
        self.dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_MAX_NUMBER_OF_FORWARDING_CLASSES', '63')

        for i in range(num_intfs):
            self.config_intf(i, is_ipv6_needed)

        self.asic_nhgs_count = len(self.asic_db.get_keys(self.ASIC_NHG_STR))
        self.asic_nhgms_count = len(self.asic_db.get_keys(self.ASIC_NHGM_STR))
        self.asic_insgs_count = len(self.asic_db.get_keys(self.ASIC_INSEG_STR))
        self.asic_nhs_count = len(self.asic_db.get_keys(self.ASIC_NHS_STR))
        self.asic_rts_count = len(self.asic_db.get_keys(self.ASIC_RT_STR))
        self.asic_nhg_maps_count = len(self.asic_db.get_keys(self.ASIC_NHG_MAP_STR))

    def nhg_exists(self, nhg_index):
        return self.get_nhg_id(nhg_index) is not None

    def route_exists(self, rt_prefix):
        return self.get_route_id(rt_prefix) is not None

    def nhg_map_exists(self, nhg_map_index):
        return self.get_nhg_map_id(nhg_map_index) is not None

    def enable_ordered_ecmp(self):
        switch_fvs = swsscommon.FieldValuePairs([('ordered_ecmp', 'true')])
        self.switch_ps.set('switch', switch_fvs)
        self.state_db.wait_for_field_match("SWITCH_CAPABILITY", "switch", {"ORDERED_ECMP_CAPABLE": "true"})

    def disble_ordered_ecmp(self):
        switch_fvs = swsscommon.FieldValuePairs([('ordered_ecmp', 'false')])
        self.switch_ps.set('switch', switch_fvs)
        self.state_db.wait_for_field_match("SWITCH_CAPABILITY", "switch", {"ORDERED_ECMP_CAPABLE": "false"})

class TestNhgExhaustBase(TestNextHopGroupBase):
    MAX_ECMP_COUNT = 512
    MAX_PORT_COUNT = 10

    def init_test(self, dvs):
        super().init_test(dvs, self.MAX_PORT_COUNT)
        self.r = 0

    def gen_nhg_fvs(self, binary):
        nexthop = []
        ifname = []

        for i in range(self.MAX_PORT_COUNT):
            if binary[i] == '1':
                nexthop.append(self.peer_ip(i))
                ifname.append(self.port_name(i))

        nexthop = ','.join(nexthop)
        ifname = ','.join(ifname)
        fvs = swsscommon.FieldValuePairs([("nexthop", nexthop), ("ifname", ifname)])

        return fvs

    def gen_valid_binary(self):
        while True:
            self.r += 1
            binary = self.gen_valid_binary.fmt.format(self.r)
            # We need at least 2 ports for a nexthop group
            if binary.count('1') <= 1:
                continue
            return binary
    gen_valid_binary.fmt = '{{0:0{}b}}'.format(MAX_PORT_COUNT)


class TestRotueOrchNhgExhaust(TestNhgExhaustBase):
    def test_route_nhg_exhaust(self, dvs, testlog):
        """
        Test the situation of exhausting ECMP group, assume SAI_SWITCH_ATTR_NUMBER_OF_ECMP_GROUPS is 512

        In order to achieve that, we will config
            1. 9 ports
            2. 512 routes with different nexthop group

        See Also
        --------
        SwitchStateBase::set_number_of_ecmp_groups()
        https://github.com/Azure/sonic-sairedis/blob/master/vslib/src/SwitchStateBase.cpp

        """

        # TODO: check ECMP 512

        def gen_ipprefix(r):
            """ Construct route like 2.X.X.0/24 """
            ip = ipaddress.IPv4Address(IP_INTEGER_BASE + r * 256)
            ip = str(ip)
            ipprefix = ip + "/24"
            return ipprefix

        def asic_route_nhg_fvs(k):
            fvs = self.asic_db.get_entry(self.ASIC_RT_STR, k)
            if not fvs:
                return None

            nhgid = fvs.get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
            if nhgid is None:
                return None

            fvs = self.asic_db.get_entry(self.ASIC_NHG_STR, nhgid)
            return fvs

        if sys.version_info < (3, 0):
            IP_INTEGER_BASE = int(ipaddress.IPv4Address(unicode("2.2.2.0")))
        else:
            IP_INTEGER_BASE = int(ipaddress.IPv4Address(str("2.2.2.0")))

        self.init_test(dvs)

        # Add first batch of routes with unique nexthop groups in AppDB
        route_count = 0
        while route_count < self.MAX_ECMP_COUNT:
            binary = self.gen_valid_binary()
            fvs = self.gen_nhg_fvs(binary)
            route_ipprefix = gen_ipprefix(route_count)
            self.rt_ps.set(route_ipprefix, fvs)
            route_count += 1

        # Wait and check ASIC DB the count of nexthop groups used
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

        # Wait and check ASIC DB the count of routes
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + self.MAX_ECMP_COUNT)
        self.asic_rts_count += self.MAX_ECMP_COUNT

        # Add a route with labeled NHs
        self.asic_nhs_count = len(self.asic_db.get_keys(self.ASIC_NHS_STR))
        route_ipprefix = gen_ipprefix(route_count)
        fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                    ('mpls_nh', 'push1,push3'),
                                    ('ifname', 'Ethernet0,Ethernet4')])
        self.rt_ps.set(route_ipprefix, fvs)
        route_count += 1

        # A temporary route should be created
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 1)

        # A NH should be elected as the temporary NHG and it should be created
        # as it doesn't exist.
        self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 1)

        # Delete the route.  The route and the added labeled NH should be
        # removed.
        self.rt_ps._del(route_ipprefix)
        route_count -= 1
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count)
        self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count)

        # Add second batch of routes with unique nexthop groups in AppDB
        # Add more routes with new nexthop group in AppDBdd
        route_ipprefix = gen_ipprefix(route_count)
        base_ipprefix = route_ipprefix
        base = route_count
        route_count = 0
        while route_count < 10:
            binary = self.gen_valid_binary()
            fvs = self.gen_nhg_fvs(binary)
            route_ipprefix = gen_ipprefix(base + route_count)
            self.rt_ps.set(route_ipprefix, fvs)
            route_count += 1
        last_ipprefix = route_ipprefix

        # Wait until we get expected routes and check ASIC DB on the count of nexthop groups used, and it should not increase
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 10)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

        # Check the route points to next hop group
        # Note: no need to wait here
        k = self.get_route_id("2.2.2.0/24")
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert fvs is not None

        # Check the second batch does not point to next hop group
        k = self.get_route_id(base_ipprefix)
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert not(fvs)

        # Remove first batch of routes with unique nexthop groups in AppDB
        route_count = 0
        self.r = 0
        while route_count < self.MAX_ECMP_COUNT:
            route_ipprefix = gen_ipprefix(route_count)
            self.rt_ps._del(route_ipprefix)
            route_count += 1
        self.asic_rts_count -= self.MAX_ECMP_COUNT

        # Wait and check the second batch points to next hop group
        # Check ASIC DB on the count of nexthop groups used, and it should not increase or decrease
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, 10)
        k = self.get_route_id(base_ipprefix)
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert fvs is not None
        k = self.get_route_id(last_ipprefix)
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert fvs is not None

        # Cleanup

        # Remove second batch of routes
        for i in range(10):
            route_ipprefix = gen_ipprefix(self.MAX_ECMP_COUNT + i)
            self.rt_ps._del(route_ipprefix)

        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, 0)
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count)


class TestNhgOrchNhgExhaust(TestNhgExhaustBase):

    def init_test(self, dvs):
        super().init_test(dvs)

        self.nhg_count = self.asic_nhgs_count
        self.first_valid_nhg = self.nhg_count
        self.asic_nhgs = {}

        # Add first batch of next hop groups to reach the NHG limit
        while self.nhg_count < self.MAX_ECMP_COUNT:
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            nhg_index = self.gen_nhg_index(self.nhg_count)
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # Save the NHG index/ID pair
            self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

            # Increase the number of NHGs in ASIC DB
            self.nhg_count += 1
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

    def gen_nhg_index(self, nhg_number):
            return "group{}".format(nhg_number)

    def create_temp_nhg(self):
        binary = self.gen_valid_binary()
        nhg_fvs = self.gen_nhg_fvs(binary)
        nhg_index = self.gen_nhg_index(self.nhg_count)
        self.nhg_ps.set(nhg_index, nhg_fvs)
        self.nhg_count += 1

        return nhg_index, binary

    def delete_nhg(self):
        del_nhg_index = self.gen_nhg_index(self.first_valid_nhg)
        del_nhg_id = self.asic_nhgs[del_nhg_index]

        self.nhg_ps._del(del_nhg_index)
        self.asic_nhgs.pop(del_nhg_index)
        self.first_valid_nhg += 1

        return del_nhg_id


    def test_nhgorch_nhg_exhaust(self, dvs, testlog):

        # Test scenario:
        # - create a NHG and assert a NHG object doesn't get added to ASIC DB
        # - delete a NHG and assert the newly created one is created in ASIC DB and its SAI ID changed
        def temporary_group_promotion_test():
            # Add a new next hop group - it should create a temporary one instead
            prev_nhgs = self.asic_db.get_keys(self.ASIC_NHG_STR)
            nhg_index, _ = self.create_temp_nhg()

            # Save the temporary NHG's SAI ID
            time.sleep(1)
            nhg_id = self.get_nhg_id(nhg_index)

            # Assert no new group has been added
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

            # Assert the same NHGs are in ASIC DB
            assert prev_nhgs == self.asic_db.get_keys(self.ASIC_NHG_STR)

            # Delete an existing next hop group
            del_nhg_id = self.delete_nhg()

            # Wait for the key to be deleted
            self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

            # Wait for the temporary group to be promoted and replace the deleted
            # NHG
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

            # Assert the SAI ID of the previously temporary NHG has been updated
            assert nhg_id != self.get_nhg_id(nhg_index)

            # Save the promoted NHG index/ID
            self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

        # Test scenario:
        # - update an existing NHG and assert the update is performed
        def group_update_test():
            # Update a group
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            nhg_index = self.gen_nhg_index(self.first_valid_nhg)

            # Save the previous members
            prev_nhg_members = self.get_nhgm_ids(nhg_index)
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # Wait a second so the NHG members get updated
            time.sleep(1)

            # Assert the group was updated by checking it's members
            assert self.get_nhgm_ids(nhg_index) != prev_nhg_members

        # Test scenario:
        # - create and delete a NHG while the ASIC DB is full and assert nothing changes
        def create_delete_temporary_test():
            # Create a new temporary group
            nhg_index, _ = self.create_temp_nhg()
            time.sleep(1)

            # Delete the temporary group
            self.nhg_ps._del(nhg_index)

            # Assert the NHG does not exist anymore
            assert not self.nhg_exists(nhg_index)

            # Assert the number of groups is the same
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

        # Test scenario:
        # - create a temporary NHG
        # - update the NHG with a different number of members
        # - delete a NHG and assert the new one is added and it has the updated number of members
        def update_temporary_group_test():
            # Create a new temporary group
            nhg_index, binary = self.create_temp_nhg()

            # Save the number of group members
            binary_count = binary.count('1')

            # Update the temporary group with a different number of members
            while True:
                binary = self.gen_valid_binary()
                if binary.count('1') == binary_count:
                    continue
                binary_count = binary.count('1')
                break
            nhg_fvs = self.gen_nhg_fvs(binary)
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # Delete a group
            del_nhg_id = self.delete_nhg()

            # Wait for the group to be deleted
            self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

            # The temporary group should be promoted
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

            # Save the promoted NHG index/ID
            self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

            # Assert it has the updated details by checking the number of members
            assert len(self.get_nhgm_ids(nhg_index)) == binary_count

        # Test scenario:
        # - create a route pointing to a NHG and assert it is added
        # - create a temporary NHG and update the route to point to it, asserting the route's SAI NHG ID changes
        # - update the temporary NHG to contain completely different members and assert the SAI ID changes
        # - delete a NHG and assert the temporary NHG is promoted and its SAI ID also changes
        def route_nhg_update_test():
            # Add a route
            nhg_index = self.gen_nhg_index(self.first_valid_nhg)
            rt_fvs = swsscommon.FieldValuePairs([('nexthop_group', nhg_index)])
            self.rt_ps.set('2.2.2.0/24', rt_fvs)

            # Assert the route is created
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 1)

            # Save the previous NHG ID
            prev_nhg_id = self.asic_nhgs[nhg_index]

            # Create a new temporary group
            nhg_index, binary = self.create_temp_nhg()

            # Get the route ID
            rt_id = self.get_route_id('2.2.2.0/24')
            assert rt_id != None

            # Update the route to point to the temporary NHG
            rt_fvs = swsscommon.FieldValuePairs([('nexthop_group', nhg_index)])
            self.rt_ps.set('2.2.2.0/24', rt_fvs)

            # Wait for the route to change its NHG ID
            self.asic_db.wait_for_field_negative_match(self.ASIC_RT_STR,
                                                    rt_id,
                                                    {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': prev_nhg_id})

            # Save the new route NHG ID
            prev_nhg_id = self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']

            # Update the temporary NHG with one that has different NHs

            # Create a new binary that uses the other interfaces than the previous
            # binary was using
            new_binary = []

            for i in range(len(binary)):
                if binary[i] == '1':
                    new_binary.append('0')
                else:
                    new_binary.append('1')

            binary = ''.join(new_binary)
            assert binary.count('1') > 1

            nhg_fvs = self.gen_nhg_fvs(binary)
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # The NHG ID of the route should change
            self.asic_db.wait_for_field_negative_match(self.ASIC_RT_STR,
                                                    rt_id,
                                                    {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': prev_nhg_id})

            # Delete a NHG.
            del_nhg_id = self.delete_nhg()

            # Wait for the NHG to be deleted
            self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

            # The temporary group should get promoted.
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

            # Save the promoted NHG index/ID
            self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

            # Assert the NHGID of the route changed due to temporary group being
            # promoted.
            self.asic_db.wait_for_field_match(self.ASIC_RT_STR,
                                            rt_id,
                                            {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': self.asic_nhgs[nhg_index]})

        # Test scenario:
        # - create a temporary NHG containing labeled NHs and assert a new NH is added to represent the group
        # - delete a NHG and assert the temporary NHG is promoted and all its NHs are added
        def labeled_nhg_temporary_promotion_test():
            # Create a next hop group that contains labeled NHs that do not exist
            # in NeighOrch
            self.asic_nhs_count = len(self.asic_db.get_keys(self.ASIC_NHS_STR))
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('mpls_nh', 'push1,push3'),
                                                ('ifname', 'Ethernet0,Ethernet4')])
            nhg_index = self.gen_nhg_index(self.nhg_count)
            self.nhg_ps.set(nhg_index, fvs)
            self.nhg_count += 1

            # A temporary next hop should be elected to represent the group and
            # thus a new labeled next hop should be created
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 1)

            # Delete a next hop group
            self.delete_nhg()

            # The group should be promoted and the other labeled NH should also get
            # created
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 2)

            # Save the promoted NHG index/ID
            self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

        # Test scenario:
        # - update route to own its NHG and assert no new NHG is added
        # - remove a NHG and assert the temporary NHG is promoted and added to ASIC DB
        def back_compatibility_test():
            # Update the route with a RouteOrch's owned NHG
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            self.rt_ps.set('2.2.2.0/24', nhg_fvs)

            # Assert no new group has been added
            time.sleep(1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

            # Delete a next hop group
            del_nhg_id = self.delete_nhg()
            self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

            # The temporary group should be promoted
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

        # Test scenario:
        # - create a NHG with all NHs not existing and assert the NHG is not created
        # - update the NHG to have valid NHs and assert a temporary NHG is created
        # - update the NHG to all invalid NHs again and assert the update is not performed and thus it has the same SAI
        #   ID
        # - delete the temporary NHG
        def invalid_temporary_test():
            # Create a temporary NHG that contains only NHs that do not exist
            nhg_fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.21,10.0.0.23'),
                                                    ('ifname', 'Ethernet40,Ethernet44')])
            nhg_index = self.gen_nhg_index(self.nhg_count)
            self.nhg_count += 1
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # Assert the group is not created
            time.sleep(1)
            assert not self.nhg_exists(nhg_index)

            # Update the temporary NHG to a valid one
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # Assert the temporary group was updated and the group got created
            nhg_id = self.get_nhg_id(nhg_index)
            assert nhg_id is not None

            # Update the temporary NHG to an invalid one again
            nhg_fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.21,10.0.0.23'),
                                                    ('ifname', 'Ethernet40,Ethernet44')])
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # The update should fail and the temporary NHG should still be pointing
            # to the old valid NH
            assert self.get_nhg_id(nhg_index) == nhg_id

            # Delete the temporary group
            self.nhg_ps._del(nhg_index)

        self.init_test(dvs)

        temporary_group_promotion_test()
        group_update_test()
        create_delete_temporary_test()
        update_temporary_group_test()
        route_nhg_update_test()
        labeled_nhg_temporary_promotion_test()
        back_compatibility_test()
        invalid_temporary_test()

        # Cleanup

        # Delete the route
        self.rt_ps._del('2.2.2.0/24')
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count)

        # Delete the next hop groups
        for k in self.asic_nhgs:
            self.nhg_ps._del(k)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)


    def test_cbf_nhg_exhaust(self, dvs, testlog):
        self.init_test(dvs)

        # Create an FC to NH index selection map
        fvs = swsscommon.FieldValuePairs([('0', '0')])
        self.fc_to_nhg_ps.set('cbfnhgmap1', fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 1)

        # Create a CBF NHG group - it should fail
        fvs = swsscommon.FieldValuePairs([('members', 'group2'),
                                    ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        assert(not self.nhg_exists('cbfgroup1'))

        # Delete a NHG
        del_nhg_id = self.delete_nhg()
        self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The CBF NHG should be created
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)
        assert(self.nhg_exists('cbfgroup1'))
        nhg_id = self.get_nhg_id('cbfgroup1')

        # Create a temporary NHG
        nhg_index, _ = self.create_temp_nhg()

        # Update the CBF NHG to contain the temporary NHG
        fvs = swsscommon.FieldValuePairs([('members', nhg_index),
                                    ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        nhgm_id = self.get_nhgm_ids('cbfgroup1')[0]
        fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
        assert(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID'] != self.get_nhg_id('group2'))

        # Coverage testing: CbfNhgHandler::hasSameMembers() returns false
        #
        # Update the group while it has a temporary NHG to contain one more member.
        #
        # Update the group keeping the same number of members, but changing the fully fledged NHG with another one.
        #
        # Update the group reversing the order of the 2 members.
        #
        # Update the CBF NHG back to its original form to resume testing.
        fvs = swsscommon.FieldValuePairs([('members', '{},group3'.format(nhg_index)),
                                    ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        fvs = swsscommon.FieldValuePairs([('members', '{},group4'.format(nhg_index)),
                                    ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        fvs = swsscommon.FieldValuePairs([('members', 'group4,{}'.format(nhg_index)),
                                    ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        fvs = swsscommon.FieldValuePairs([('members', nhg_index), ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)

        # Delete a NHG
        nh_id = self.get_nhg_id(nhg_index)
        del_nhg_id = self.delete_nhg()
        self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The NHG should be promoted and the CBF NHG member updated
        nhgm_id = self.get_nhgm_ids('cbfgroup1')[0]
        self.asic_db.wait_for_field_negative_match(self.ASIC_NHGM_STR,
                                                nhgm_id,
                        {'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID': nh_id})
        self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

        # Create a couple more temporary NHGs
        nhg_index1, _ = self.create_temp_nhg()
        nhg_index2, _ = self.create_temp_nhg()

        # Update the CBF NHG to contain both of them as well
        fvs = swsscommon.FieldValuePairs([('members', '{},{},{}'.format(nhg_index, nhg_index1, nhg_index2)),
                                    ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)

        # The previous CBF NHG member should be deleted
        self.asic_db.wait_for_deleted_entry(self.ASIC_NHGM_STR, nhgm_id)

        # Create an FC to NH index selection map
        fvs = swsscommon.FieldValuePairs([('0', '0')])
        self.fc_to_nhg_ps.set('cbfnhgmap2', fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 2)

        # Update the selection map of the group
        old_map = self.asic_db.get_entry(self.ASIC_NHG_STR, nhg_id)['SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP']
        nhgm_ids = self.get_nhgm_ids('cbfgroup1')
        members = {nhgm_id: self.asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id) for nhgm_id in nhgm_ids}
        fvs = swsscommon.FieldValuePairs([('members', '{},{},{}'.format(nhg_index, nhg_index1, nhg_index2)),
                                    ('selection_map', 'cbfnhgmap2')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)

        # Check that the map was updates, but the members weren't
        self.asic_db.wait_for_field_negative_match(self.ASIC_NHG_STR,
                                                nhg_id,
                                                {'SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP': old_map})
        values = {}
        for nhgm_id in nhgm_ids:
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
            assert(members[nhgm_id] == fvs)
            values[nhgm_id] = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']

        # Gradually delete temporary NHGs and check that the NHG members are
        # updated
        for i in range(2):
            # Delete a temporary NHG
            del_nhg_id = self.delete_nhg()
            self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)
            time.sleep(1)

            # Sleep 1 second to allow CBF NHG to update its member NH ID
            time.sleep(1)

            # Check that one of the temporary NHGs has been updated
            diff_count = 0
            for nhgm_id, nh_id in values.items():
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
                if nh_id != fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']:
                    values[nhgm_id] = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']
                    diff_count += 1
            assert(diff_count == 1)

        for index in [nhg_index1, nhg_index2]:
            self.asic_nhgs[index] = self.get_nhg_id(index)

        # Create a temporary NHG
        nhg_index, binary = self.create_temp_nhg()

        # Update the CBF NHG to point to the new NHG
        nhgm_ids = self.get_nhgm_ids('cbfgroup1')
        fvs = swsscommon.FieldValuePairs([('members', nhg_index), ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        self.asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)

        # Update the temporary NHG to force using a new designated NH
        new_binary = []

        for i in range(len(binary)):
            if binary[i] == '1':
                new_binary.append('0')
            else:
                new_binary.append('1')

        binary = ''.join(new_binary)
        assert binary.count('1') > 1

        nh_id = self.get_nhg_id(nhg_index)
        nhg_fvs = self.gen_nhg_fvs(binary)
        self.nhg_ps.set(nhg_index, nhg_fvs)

        # The CBF NHG member's NH attribute should be updated
        nhgm_id = self.get_nhgm_ids('cbfgroup1')[0]
        self.asic_db.wait_for_field_negative_match(self.ASIC_NHGM_STR,
                                                nhgm_id,
                                                {'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID': nh_id})

        # Delete a NHG
        del_nhg_id = self.delete_nhg()
        self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The temporary group should be promoted
        nh_id = self.get_nhg_id(nhg_index)
        self.asic_db.wait_for_field_match(self.ASIC_NHGM_STR,
                                    nhgm_id,
                                    {'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID': nh_id})
        self.asic_nhgs[nhg_index] = nh_id

        # Delete the NHGs
        self.cbf_nhg_ps._del('cbfgroup1')
        for k in self.asic_nhgs:
            self.nhg_ps._del(k)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

        # Delete the NHG maps
        self.fc_to_nhg_ps._del('cbfnhgmap1')
        self.fc_to_nhg_ps._del('cbfnhgmap2')
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count)

class TestNextHopGroup(TestNextHopGroupBase):

    @pytest.mark.parametrize('ordered_ecmp', ['false', 'true'])
    def test_route_nhg(self, ordered_ecmp, dvs, dvs_route, testlog):
        self.init_test(dvs, 3)
        nhip_seqid_map = {"10.0.0.1" : "1", "10.0.0.3" : "2" , "10.0.0.5" : "3" }

        if ordered_ecmp == 'true':
           self.enable_ordered_ecmp()

        rtprefix = "2.2.2.0/24"

        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        # nexthop group without weight
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"),
                                          ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        self.rt_ps.set(rtprefix, fvs)

        # check if route was propagated to ASIC DB
        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        # assert the route points to next hop group
        fvs = self.asic_db.get_entry(self.ASIC_RT_STR, rtkeys[0])

        nhgid = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        fvs = self.asic_db.get_entry(self.ASIC_NHG_STR, nhgid)

        assert bool(fvs)

        if ordered_ecmp == 'true':
            assert fvs["SAI_NEXT_HOP_GROUP_ATTR_TYPE"] == "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP"
        else:
            assert fvs["SAI_NEXT_HOP_GROUP_ATTR_TYPE"] == "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP"

        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

        assert len(keys) == 3

        for k in keys:
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

            # verify weight attributes not in asic db
            assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT") is None

            if ordered_ecmp == "true":
                nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                nh_fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)
                assert nhip_seqid_map[nh_fvs["SAI_NEXT_HOP_ATTR_IP"]] == fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID"]
            else:
                assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID") is None
     
        # BFD: test validate/invalidate nexthop group member when bfd state changes -- begin 
        bfdSessions = self.get_exist_bfd_session()
        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.2"}
        self.create_bfd_session("default:default:10.0.0.3", fieldValues)
        time.sleep(1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1
        session = createdSessions.pop()

        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.3",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.2"}
        self.check_state_bfd_session_value("default|default|10.0.0.3", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Down")
        time.sleep(1)
        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Down"
        self.check_state_bfd_session_value("default|default|10.0.0.3", expected_sdb_values)

        #check nexthop group member is removed
        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)
        assert len(keys) == 2

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(1)
        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.3", expected_sdb_values)

        #check nexthop group member is added back
        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)
        assert len(keys) == 3

        # Remove the BFD session
        self.remove_bfd_session("default:default:10.0.0.3")
        self.asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)
        # BFD: test validate/invalidate nexthop group member when bfd state changes -- end

        # Remove route 2.2.2.0/24
        self.rt_ps._del(rtprefix)

        # Wait for route 2.2.2.0/24 to be removed
        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        # Negative test with nexthops with incomplete weight info
        # To validate Order ECMP change the nexthop order
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.5,10.0.0.1,10.0.0.3"),
                                          ("ifname", "Ethernet8,Ethernet0,Ethernet4"),
                                          ("weight", "10,30")])
        self.rt_ps.set(rtprefix, fvs)

        # check if route was propagated to ASIC DB
        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        # assert the route points to next hop group
        fvs = self.asic_db.get_entry(self.ASIC_RT_STR, rtkeys[0])

        nhgid = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        fvs = self.asic_db.get_entry(self.ASIC_NHG_STR, nhgid)

        assert bool(fvs)

        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

        assert len(keys) == 3

        for k in keys:
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

            # verify weight attributes not in asic db
            assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT") is None
            
            if ordered_ecmp == "true":
                nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                nh_fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)
                assert nhip_seqid_map[nh_fvs["SAI_NEXT_HOP_ATTR_IP"]] == fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID"]
            else:
                assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID") is None
 

        # Remove route 2.2.2.0/24
        self.rt_ps._del(rtprefix)

        # Wait for route 2.2.2.0/24 to be removed
        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"),
                                          ("ifname", "Ethernet0,Ethernet4,Ethernet8"),
                                          ("weight", "10,30,50")])
        self.rt_ps.set(rtprefix, fvs)

        # check if route was propagated to ASIC DB
        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        # assert the route points to next hop group
        fvs = self.asic_db.get_entry(self.ASIC_RT_STR, rtkeys[0])

        nhgid = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        fvs = self.asic_db.get_entry(self.ASIC_NHG_STR, nhgid)

        assert bool(fvs)

        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

        assert len(keys) == 3

        for k in keys:
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

            # verify weight attributes in asic db
            nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
            weight = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT"]

            if ordered_ecmp == "true":
                nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                nh_fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)
                assert nhip_seqid_map[nh_fvs["SAI_NEXT_HOP_ATTR_IP"]] == fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID"]
            else:
                assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID") is None
 
            fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)
            nhip = fvs["SAI_NEXT_HOP_ATTR_IP"].split('.')
            expected_weight = int(nhip[3]) * 10

            assert int(weight) == expected_weight

        rtprefix2 = "3.3.3.0/24"

        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"),
                                          ("ifname", "Ethernet0,Ethernet4,Ethernet8"),
                                          ("weight", "20,30,40")])
        self.rt_ps.set(rtprefix2, fvs)

        # wait for route to be programmed
        time.sleep(1)

        keys = self.asic_db.get_keys(self.ASIC_NHG_STR)

        assert len(keys) == 2

        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

        assert len(keys) == 6

        # Remove route 3.3.3.0/24
        self.rt_ps._del(rtprefix2)

        # Wait for route 3.3.3.0/24 to be removed
        dvs_route.check_asicdb_deleted_route_entries([rtprefix2])


        # bring links down one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'down')

            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == 2 - i

        # bring links up one-by-one
        # Bring link up in random order to verify sequence id is as per order
        for i, val in enumerate([2,1,0]):
            self.flap_intf(i, 'up')

            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == i + 1

            for k in keys:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
                assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid
                if ordered_ecmp == "true":
                    nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                    nh_fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)
                    assert nhip_seqid_map[nh_fvs["SAI_NEXT_HOP_ATTR_IP"]] == fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID"]
                else:
                    assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID") is None
     
        # Remove route 2.2.2.0/24
        self.rt_ps._del(rtprefix)

        # Wait for route 2.2.2.0/24 to be removed
        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        # Cleanup by disabling to get default behaviour
        if ordered_ecmp == 'true':
           self.disble_ordered_ecmp()

    def test_label_route_nhg(self, dvs, testlog):
        self.init_test(dvs, 3)

        # add label route
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        self.lr_ps.set("10", fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, self.asic_insgs_count + 1)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
        self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 3)

        k = self.get_inseg_id('10')
        assert k is not None

        # assert the route points to next hop group
        fvs = self.asic_db.get_entry(self.ASIC_INSEG_STR, k)
        nhgid = fvs["SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID"]
        fvs = self.asic_db.get_entry(self.ASIC_NHG_STR, nhgid)
        assert bool(fvs)

        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)
        assert len(keys) == 3
        for k in keys:
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

        # bring links down one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'down')
            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)
            assert len(keys) == 2 - i

        # bring links up one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'up')
            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)
            assert len(keys) == i + 1
            for k in keys:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
                assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

        # Remove label route 10
        self.lr_ps._del("10")

        # Wait for label route 10 to be removed
        self.asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, self.asic_insgs_count)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

    def test_nhgorch_labeled_nhs(self, dvs, testlog):
        # Test scenario:
        # - create a NHG with all labeled and weighted NHs and assert 2 new NHs are created
        # - create a NHG with an existing label and assert no new NHs are created
        # - create a NHG with a new label and assert a new NH is created
        # - remove the third NHG and assert the NH is deleted
        # - delete the second group and assert no NH is deleted because it is still referenced by the first group
        # - remove the weights from the first NHG and change the labels, leaving one NH unlabeled; assert one NH is
        #   deleted
        # - delete the first NHG and perform cleanup
        def mainline_labeled_nhs_test():
            # Add a group containing labeled weighted NHs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('mpls_nh', 'push1,push3'),
                                                ('ifname', 'Ethernet0,Ethernet4'),
                                                ('weight', '2,4')])
            self.nhg_ps.set('group1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)

            # NhgOrch should create two next hops for the labeled ones
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 2)

            # Assert the weights are properly set
            nhgm_ids = self.get_nhgm_ids('group1')
            weights = []
            for k in nhgm_ids:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
                weights.append(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT'])
            assert set(weights) == set(['2', '4'])

            # Create a new single next hop with the same label
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1'),
                                            ('mpls_nh', 'push1'),
                                                ('ifname', 'Ethernet0')])
            self.nhg_ps.set('group2', fvs)

            # No new next hop should be added
            time.sleep(1)
            assert len(self.asic_db.get_keys(self.ASIC_NHS_STR)) == self.asic_nhs_count + 2

            # Create a new single next hop with a different label
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1'),
                                            ('mpls_nh', 'push2'),
                                                ('ifname', 'Ethernet0')])
            self.nhg_ps.set('group3', fvs)

            # A new next hop should be added
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 3)

            # Delete group3
            self.nhg_ps._del('group3')

            # Group3's NH should be deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 2)

            # Delete group2
            self.nhg_ps._del('group2')

            # The number of NHs should be the same as they are still referenced by
            # group1
            time.sleep(1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 2)

            # Update group1 with no weights and both labeled and unlabeled NHs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('mpls_nh', 'push2,na'),
                                                ('ifname', 'Ethernet0,Ethernet4')])
            self.nhg_ps.set('group1', fvs)

            # Group members should be replaced and one NH should get deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 1)

            # Delete group1
            self.nhg_ps._del('group1')

            # Wait for the group and it's members to be deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count)

            # The two next hops should also get deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count)

        # Test scenario:
        # - create a route with labeled and weighted NHs and assert a NHG and 2 NHs are created
        # - create a NHG with the same details as the one being used by the route and assert a NHG is created and no
        #   new NHs are added
        # - update the NHG by changing the first NH's label and assert a new NH is created
        # - remove the route and assert that only one (now unreferenced) NH is removed
        # - remove the NHG and perform cleanup
        def routeorch_nhgorch_interop_test():
            # Create a route with labeled NHs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                        ('mpls_nh', 'push1,push3'),
                                        ('ifname', 'Ethernet0,Ethernet4'),
                                        ('weight', '2,4')])
            self.rt_ps.set('2.2.2.0/24', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 1)

            # A NHG should be created
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)

            # Two new next hops should be created
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 2)

            # Create a NHG with the same details
            self.nhg_ps.set('group1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)

            # No new next hops should be created
            assert len(self.asic_db.get_keys(self.ASIC_NHS_STR)) == self.asic_nhs_count + 2

            # Update the group with a different NH
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('mpls_nh', 'push2,push3'),
                                                ('ifname', 'Ethernet0,Ethernet4'),
                                                ('weight', '2,4')])
            self.nhg_ps.set('group1', fvs)

            # A new next hop should be created
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 3)

            # group1 should be updated and a new NHG shouldn't be created
            time.sleep(1)
            assert len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count + 2

            # Remove the route
            self.rt_ps._del('2.2.2.0/24')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)

            # One NH should become unreferenced and should be deleted.  The other
            # one is still referenced by NhgOrch's owned NHG.
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 2)

            # Remove the group
            self.nhg_ps._del('group1')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

            # Both new next hops should be deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count)

        self.init_test(dvs, 2)

        mainline_labeled_nhs_test()
        routeorch_nhgorch_interop_test()

    def test_nhgorch_excp_group_cases(self, dvs, testlog):
        # Test scenario:
        # - remove a NHG that does not exist and assert the number of NHGs in ASIC DB remains the same
        def remove_inexistent_nhg_test():
            # Remove a group that does not exist
            self.nhg_ps._del("group1")
            time.sleep(1)
            assert len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count

        # Test scenario:
        # - create a NHG with a member which does not exist and assert no NHG is created
        # - update the NHG to contain all valid members and assert the NHG is created and it has 2 members
        def nhg_members_validation_test():
            # Create a next hop group with a member that does not exist - should fail
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.63'),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet124")])
            self.nhg_ps.set("group1", fvs)
            time.sleep(1)
            assert len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count

            # Issue an update for this next hop group that doesn't yet exist,
            # which contains only valid NHs.  This will overwrite the previous
            # operation and create the group.
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.5'),
                                            ("ifname", "Ethernet0,Ethernet8")])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)

            # Check the group has its two members
            assert len(self.get_nhgm_ids('group1')) == 2

        # Test scenario:
        # - create a route pointing to the NHG created in `test_nhg_members_validation` and assert it is being created
        # - remove the NHG and assert it fails as it is being referenced
        # - create a new NHG and assert it and its members are being created
        # - update the route to point to the new NHG and assert the first NHG is now deleted as it's not referenced
        #   anymore
        def remove_referenced_nhg_test():
        # Add a route referencing the new group
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'group1')])
            self.rt_ps.set('2.2.2.0/24', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 1)

            # Try removing the group while it still has references - should fail
            self.nhg_ps._del('group1')
            time.sleep(1)
            assert len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count + 1

            # Create a new group
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
            self.nhg_ps.set("group2", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 4)

            # Update the route to point to the new group
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'group2')])
            self.rt_ps.set('2.2.2.0/24', fvs)

            # The first group should have got deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)

            # The route's group should have changed to the new one
            assert self.asic_db.get_entry(self.ASIC_RT_STR, self.get_route_id('2.2.2.0/24'))['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] == self.get_nhg_id('group2')

        # Test scenario:
        # - update the route created in `test_remove_referenced_nhg` to own the NHG with the same details as the
        #   previous one and assert a new NHG and 2 new NHGMs are added
        # - update the route to point back to the original NHG and assert the routeOrch's owned NHG is deleted
        def routeorch_nhgorch_interop_test():
            rt_id = self.get_route_id('2.2.2.0/24')
            assert rt_id is not None

            # Update the route with routeOrch's owned next hop group
            nhgid = self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
            self.rt_ps.set('2.2.2.0/24', fvs)

            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 4)

            # Assert the next hop group ID changed
            time.sleep(1)
            assert self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] != nhgid
            nhgid = self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']

            # Update the route to point back to group2
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'group2')])
            self.rt_ps.set('2.2.2.0/24', fvs)

            # The routeOrch's owned next hop group should get deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)

            # Assert the route points back to group2
            assert self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] != nhgid

        # Test scenario:
        # - create a new NHG with the same details as the previous NHG and assert a new NHG and 2 new NHGMs are created
        # - update the route to point to the new NHG and assert its SAI NHG ID changes
        def identical_nhgs_test():
            rt_id = self.get_route_id('2.2.2.0/24')
            assert rt_id is not None

            # Create a new group with the same members as group2
            nhgid = self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 4)

            # Update the route to point to the new group
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'group1')])
            self.rt_ps.set('2.2.2.0/24', fvs)
            time.sleep(1)

            # Assert the next hop group ID changed
            assert self.asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] != nhgid

        # Test scenario:
        # - create a route referencing a NHG that does not exist and assert it is not created
        def create_route_inexistent_nhg_test():
            # Add a route with a NHG that does not exist
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'group3')])
            self.rt_ps.set('2.2.3.0/24', fvs)
            time.sleep(1)
            assert self.get_route_id('2.2.3.0/24') is None

            # Remove the pending route
            self.rt_ps._del('2.2.3.0/24')

        self.init_test(dvs, 3)

        remove_inexistent_nhg_test()
        nhg_members_validation_test()
        remove_referenced_nhg_test()
        routeorch_nhgorch_interop_test()
        identical_nhgs_test()
        create_route_inexistent_nhg_test()

        # Cleanup

        # Remove the route
        self.rt_ps._del('2.2.2.0/24')
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count)

        # Remove the groups
        self.nhg_ps._del('group1')
        self.nhg_ps._del('group2')
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)
        self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count)

    def test_nhgorch_nh_group(self, dvs, testlog):
        # Test scenario:
        # - create recursive nhg - rec_grp1 with two members - grp1 and grp2 only one of which exists
        # - create singleton nhg grp2 and check if the rec_grp1 is updated with both the members
        # - create a recursive nhg - rec_grp2 with another recursive nhg - rec_grp1 as member. Assert that the nhg is not created.
        def create_recursive_nhg_test():
            # create next hop group in APPL DB
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1'), ('ifname', 'Ethernet0')])
            self.nhg_ps.set("grp1", fvs)

            # create a recursive nexthop group with two members
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'grp1,grp2')])
            self.nhg_ps.set("rec_grp1", fvs)

            # check if group was propagated to ASIC DB with the existing member
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            assert self.nhg_exists('rec_grp1')

            # check if the existing member was propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 1)
            assert len(self.get_nhgm_ids('rec_grp1')) == 1

            # add another singleton nexthop group - grp2
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.3'), ('ifname', 'Ethernet4')])
            self.nhg_ps.set("grp2", fvs)

            # check if both the members were propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)
            assert len(self.get_nhgm_ids('rec_grp1')) == 2

            # update the recursive nexthop group with another member not yet existing
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'grp1,grp2,grp3')])
            self.nhg_ps.set("rec_grp1", fvs)

            # check if only two members were propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)
            assert len(self.get_nhgm_ids('rec_grp1')) == 2

            # add another singleton nexthop group - grp3
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.5'), ('ifname', 'Ethernet8')])
            self.nhg_ps.set("grp3", fvs)

            # check if all members were propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 3)
            assert len(self.get_nhgm_ids('rec_grp1')) == 3

            # create a recursive nhg with another recursive nhg as member
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'rec_grp1')])
            self.nhg_ps.set("rec_grp2", fvs)

            # check that the group was not propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            assert not self.nhg_exists('rec_grp2')

            self.nhg_ps._del("rec_grp2")
            self.nhg_ps._del("rec_grp1")
            self.nhg_ps._del("grp1")
            self.nhg_ps._del("grp2")
            self.nhg_ps._del("grp3")
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count)

        # Test scenario:
        # - create NHG 'group1' and assert it is being added to ASIC DB along with its members
        def create_nhg_test():
            # create next hop group in APPL DB
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
            self.nhg_ps.set("group1", fvs)

            # check if group was propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            assert self.nhg_exists('group1')

            # check if members were propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 3)
            assert len(self.get_nhgm_ids('group1')) == 3

        # Test scenario:
        # - create a route pointing to `group1` and assert it is being added to ASIC DB and pointing to its SAI ID
        # - delete the route and assert it is being removed
        def create_route_nhg_test():
            # create route in APPL DB
            fvs = swsscommon.FieldValuePairs([("nexthop_group", "group1")])
            self.rt_ps.set("2.2.2.0/24", fvs)

            # check if route was propagated to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 1)

            k = self.get_route_id('2.2.2.0/24')
            assert k is not None

            # assert the route points to next hop group
            fvs = self.asic_db.get_entry(self.ASIC_RT_STR, k)
            assert fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"] == self.get_nhg_id('group1')

            # Remove route 2.2.2.0/24
            self.rt_ps._del("2.2.2.0/24")
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count)

        # Test scenario:
        # - bring the links down one by one and assert the group1's members are subsequently removed and the group
        #   still exists
        # - bring the liks up one by one and assert the group1's members are subsequently added back
        def link_flap_test():
            # bring links down one-by-one
            for i in [0, 1, 2]:
                self.flap_intf(i, 'down')
                self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2 - i)
                assert len(self.get_nhgm_ids('group1')) == 2 - i
                assert self.nhg_exists('group1')

            # bring links up one-by-one
            for i in [0, 1, 2]:
                self.flap_intf(i, 'up')
                self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + i + 1)
                assert len(self.get_nhgm_ids('group1')) == i + 1

        # Test scenario:
        # - bring a link down and assert a NHGM of `group1` is removed
        # - create NHG `group2` which has a member pointing to the link being down and assert the group gets created
        #   but the member referencing the link is not added
        # - update `group1` by removing a member while having another member referencing the link which is down and
        #   assert it'll only have a member added in ASIC DB
        # - bring the link back up and assert the missing 2 members of `group1` and `group2` are added
        # - remove `group2` and assert it and its members are removed
        def validate_invalidate_group_member_test():
            # Bring an interface down
            self.flap_intf(1, 'down')

            # One group member will get deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)

            # Create a group that contains a NH that uses the down link
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ("ifname", "Ethernet0,Ethernet4")])
            self.nhg_ps.set('group2', fvs)

            # The group should get created, but it will not contained the NH that
            # has the link down
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 3)
            assert len(self.get_nhgm_ids('group2')) == 1

            # Update the NHG with one interface down
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.3,10.0.0.1'),
                                            ("ifname", "Ethernet4,Ethernet0")])
            self.nhg_ps.set("group1", fvs)

            # Wait for group members to update - the group will contain only the
            # members that have their links up
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)
            assert len(self.get_nhgm_ids('group1')) == 1

            # Bring the interface up
            self.flap_intf(1, 'up')

            # Check that the missing member of group1 and group2 is being added
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 4)

            # Remove group2
            self.nhg_ps._del('group2')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)

        # Test scenario:
        # - create NHG `group2` with a NH that does not exist and assert it isn't created
        # - update `group1` to contain the invalid NH and assert it remains only with the unremoved members
        # - configure the invalid NH's interface and assert `group2` gets created and `group1`'s NH is added
        # - delete `group` and assert it is being removed
        def inexistent_group_member_test():
            # Create group2 with a NH that does not exist
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.3,10.0.0.63'),
                                            ("ifname", "Ethernet4,Ethernet124")])
            self.nhg_ps.set("group2", fvs)

            # The groups should not be created
            time.sleep(1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)

            # Update group1 with a NH that does not exist
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.3,10.0.0.63'),
                                            ("ifname", "Ethernet4,Ethernet124")])
            self.nhg_ps.set("group1", fvs)

            # The update should fail, leaving group1 with only the unremoved
            # members
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 1)
            assert len(self.get_nhgm_ids('group1')) == 1

            # Configure the missing NH's interface
            self.config_intf(31)

            # A couple more routes will be added to ASIC DB
            self.asic_rts_count += 2

            # Group2 should get created and group1 should be updated
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 4)
            assert len(self.get_nhgm_ids('group1')) == 2
            assert len(self.get_nhgm_ids('group2')) == 2

            # Delete group2
            self.nhg_ps._del('group2')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)

        # Test scenario:
        # - update `group1` to have 4 members and assert they are all added
        # - update `group1` to have only 1 member and assert the other 3 are removed
        # - update `group1` to have 2 members and assert a new one is added
        def update_nhgm_count_test():
            # Update the NHG, adding two new members
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7'),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12")])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 4)
            assert len(self.get_nhgm_ids('group1')) == 4

            # Update the group to one NH only
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1'), ("ifname", "Ethernet0")])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count)
            assert len(self.get_nhgm_ids('group1')) == 0

            # Update the group to 2 NHs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'), ("ifname", "Ethernet0,Ethernet4")])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)
            assert len(self.get_nhgm_ids('group1')) == 2

        self.init_test(dvs, 4)

        create_recursive_nhg_test()
        create_nhg_test()
        create_route_nhg_test()
        link_flap_test()
        validate_invalidate_group_member_test()
        inexistent_group_member_test()
        update_nhgm_count_test()

        # Cleanup

        # Remove group1
        self.nhg_ps._del("group1")
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

    def test_nhgorch_label_route(self, dvs, testlog):
        self.init_test(dvs, 4)

        # create next hop group in APPL DB
        fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                        ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        self.nhg_ps.set("group1", fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)
        self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 3)

        # create label route in APPL DB pointing to the NHG
        fvs = swsscommon.FieldValuePairs([("nexthop_group", "group1")])
        self.lr_ps.set("20", fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, self.asic_insgs_count + 1)

        k = self.get_inseg_id('20')
        assert k is not None

        # assert the route points to next hop group
        fvs = self.asic_db.get_entry(self.ASIC_INSEG_STR, k)
        assert fvs["SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID"] == self.get_nhg_id('group1')

        # Remove label route 20
        self.lr_ps._del("20")
        self.asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, self.asic_insgs_count)

        # Remove group1
        self.nhg_ps._del("group1")
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

    def test_route_fallback_to_default_bothv4v6(self, dvs, dvs_route, testlog):
        self.init_test(dvs, 6, True)
        rtprefix_v6 = "2603:10b0::1/120"
        defaultprefix_v6 = "::/0"
        nexthop_str_v6 = "fc00::1,fc00::3,fc00::5"
        default_nexthop_str_v6 = "fc00::7,fc00::9,fc00::b"
        rtprefix = "3.3.3.0/24"
        defaultprefix = "0.0.0.0/0"
        nexthop_str = "10.0.0.1,10.0.0.3,10.0.0.5"
        default_nexthop_str = "10.0.0.7,10.0.0.9,10.0.0.11"

        dvs_route.check_asicdb_deleted_route_entries([rtprefix, rtprefix_v6])

        try:
            dvs.disable_fpmsyncd()
            # Program Regular Rouute with fallback to default 
            fvs = swsscommon.FieldValuePairs([("nexthop",nexthop_str),
                                              ("ifname", "Ethernet0,Ethernet4,Ethernet8"),
                                              ("fallback_to_default_route", "true")])
            self.rt_ps.set(rtprefix, fvs)
            time.sleep(1)

            fvs = swsscommon.FieldValuePairs([("nexthop",nexthop_str_v6),
                                              ("ifname", "Ethernet0,Ethernet4,Ethernet8"),
                                              ("fallback_to_default_route", "true")])
            self.rt_ps.set(rtprefix_v6, fvs)
            time.sleep(1)

            # Program default route
            fvs = swsscommon.FieldValuePairs([("nexthop", default_nexthop_str),
                                              ("ifname", "Ethernet12,Ethernet16,Ethernet20")])

            self.rt_ps.set(defaultprefix, fvs)
            time.sleep(1)

            fvs = swsscommon.FieldValuePairs([("nexthop", default_nexthop_str_v6),
                                              ("ifname", "Ethernet12,Ethernet16,Ethernet20")])

            self.rt_ps.set(defaultprefix_v6, fvs)
            time.sleep(1)

            # check if route was propagated to ASIC DB
            rtkeys = dvs_route.check_asicdb_route_entries([rtprefix, rtprefix_v6])

            # check if default route was propagated to ASIC DB
            defaultrtkeys = dvs_route.check_asicdb_route_entries([defaultprefix, defaultprefix_v6])

            default_nhgid = []
            default_nhops = set()
            default_nhgmids = set()
            default_nhopsids = set()
            default_parentnhgid = set()

            rt_nhgid = []
            rt_nhops = set()
            rt_nhopsids = set()
            rt_nhgmids = set()
            rt_parentnhgid = set()

            # assert the route points to next hop group
            flat_list = []
            for x in rtkeys:
                flat_list.append(x)
            for x in defaultrtkeys:
                flat_list.append(x)
            for idx, rtkey in enumerate(flat_list):
                fvs = self.asic_db.get_entry(self.ASIC_RT_STR, rtkey)
                if idx in [0,1]:
                    rt_nhgid.append(fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"])
                else:
                    default_nhgid.append(fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"])

                nhgid = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

                fvs = self.asic_db.get_entry(self.ASIC_NHG_STR, nhgid)

                assert bool(fvs)

            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == 12

            for k in keys:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
                nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                nh_fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)

                if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] in rt_nhgid:
                    rt_nhopsids.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"])
                    rt_nhops.add(nh_fvs["SAI_NEXT_HOP_ATTR_IP"])
                    rt_nhgmids.add(k)
                    rt_parentnhgid.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"])
                elif fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] in default_nhgid:
                    default_nhopsids.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"])
                    default_nhops.add(nh_fvs["SAI_NEXT_HOP_ATTR_IP"])
                    default_nhgmids.add(k)
                    default_parentnhgid.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"])

            assert len(rt_parentnhgid) == 2
            assert len(rt_nhopsids) == 6
            assert len(rt_nhops) == 6
            assert len(rt_nhgmids) == 6

            assert len(default_parentnhgid) == 2
            assert len(default_nhopsids) == 6
            assert len(default_nhops) == 6
            assert len(default_nhgmids) == 6

            assert rt_nhops != default_nhops
            assert rt_nhopsids != default_nhopsids
            assert rt_parentnhgid != default_parentnhgid
            assert rt_nhgmids != default_nhgmids

            # bring links down one-by-one
            for i in [0, 1, 2]:
                self.flap_intf(i, 'down')
                time.sleep(1)

                keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

                if i != 2:
                    assert len(keys) == 10 - (i * 2)
                else:
                    # Last Link down so we will fallback to default eoute 3 members
                    assert len(keys) == 12
                    rt_nhops.clear()
                    rt_nhgmids.clear()
                    rt_nhopsids.clear()
                    for k in keys:
                        fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
                        nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                        nh_fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)

                        if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] in rt_nhgid:
                            rt_nhopsids.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"])
                            rt_nhops.add(nh_fvs["SAI_NEXT_HOP_ATTR_IP"])
                            rt_nhgmids.add(k)
                            rt_parentnhgid.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"])
                        elif fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] in default_nhgid:
                            default_nhopsids.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"])
                            default_nhops.add(nh_fvs["SAI_NEXT_HOP_ATTR_IP"])
                            default_nhgmids.add(k)
                            default_parentnhgid.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"])

                    assert len(rt_parentnhgid) == 2
                    assert len(rt_nhopsids) == 6
                    assert len(rt_nhops) == 6
                    assert len(rt_nhgmids) == 6

                    assert len(default_parentnhgid) == 2
                    assert len(default_nhopsids) == 6
                    assert len(default_nhops) == 6
                    assert len(default_nhgmids) == 6

                    assert rt_nhops == default_nhops
                    assert rt_nhopsids == default_nhopsids
                    assert rt_nhgmids != default_nhgmids

            # bring links up one-by-one
            # Bring link up in random order to verify sequence id is as per order
            for i, val in enumerate([2,1,0]):
                self.flap_intf(i, 'up')
                time.sleep(1)

                keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

                assert len(keys) == 12

            for k in keys:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
                nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                nh_fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nhid)

                if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] in rt_nhgid:
                    rt_nhopsids.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"])
                    rt_nhops.add(nh_fvs["SAI_NEXT_HOP_ATTR_IP"])
                    rt_nhgmids.add(k)
                    rt_parentnhgid.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"])
                elif fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] in default_nhgid:
                    default_nhopsids.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"])
                    default_nhops.add(nh_fvs["SAI_NEXT_HOP_ATTR_IP"])
                    default_nhgmids.add(k)
                    default_parentnhgid.add(fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"])

            assert len(rt_parentnhgid) == 2
            assert len(rt_nhopsids) == 6
            assert len(rt_nhops) == 6
            assert len(rt_nhgmids) == 6

            assert len(default_parentnhgid) == 2
            assert len(default_nhopsids) == 6
            assert len(default_nhops) == 6
            assert len(default_nhgmids) == 6

            assert rt_nhops == default_nhops
            assert rt_nhopsids == default_nhopsids
            assert rt_nhgmids != default_nhgmids

            # Remove route 2.2.2.0/24
            self.rt_ps._del(rtprefix)
            time.sleep(1)

            self.rt_ps._del(rtprefix_v6)
            time.sleep(1)

            # Wait for route 2.2.2.0/24 to be removed
            dvs_route.check_asicdb_deleted_route_entries([rtprefix, rtprefix_v6])

            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == 6

            # Remove route 0.0.0.0/0
            self.rt_ps._del(defaultprefix)
            time.sleep(1)

            self.rt_ps._del(defaultprefix_v6)
            time.sleep(1)

            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == 0

        finally:
            dvs.start_fpmsyncd()

class TestCbfNextHopGroup(TestNextHopGroupBase):
    MAX_NHG_MAP_COUNT = 512

    def test_cbf_nhgorch(self, dvs, testlog):

        # Test scenario:
        # - create two NHGs and a NHG map
        # - create a CBF NHG and assert it has the expected details
        # - delete the CBF NHG
        def mainline_cbf_nhg_test():
            # Create two non-CBF NHGs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
            self.nhg_ps.set("group1", fvs)

            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                                ('ifname', 'Ethernet0,Ethernet4')])
            self.nhg_ps.set('group2', fvs)

            # Wait for the groups to appear in ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)

            # Create an FC to NH index selection map
            nhg_map = [(str(i), '0' if i < 4 else '1') for i in range(8)]
            fvs = swsscommon.FieldValuePairs(nhg_map)
            self.fc_to_nhg_ps.set('cbfnhgmap1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 1)

            # Create a CBF NHG
            fvs = swsscommon.FieldValuePairs([('members', 'group1,group2'),
                                                ('selection_map', 'cbfnhgmap1')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)

            # Check if the group makes it to ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)
            nhgid = self.get_nhg_id('cbfgroup1')
            assert(nhgid != None)
            fvs = self.asic_db.get_entry(self.ASIC_NHG_STR, nhgid)
            assert(fvs['SAI_NEXT_HOP_GROUP_ATTR_TYPE'] == 'SAI_NEXT_HOP_GROUP_TYPE_CLASS_BASED')

            # Check if the next hop group members get created
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 7)
            nhgm_ids = self.get_nhgm_ids('cbfgroup1')
            assert(len(nhgm_ids) == 2)
            nhg_ids = dict(zip([self.get_nhg_id(x) for x in ['group1', 'group2']], ['0', '1']))
            for nhgm_id in nhgm_ids:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
                nh_id = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']
                assert(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX'] == nhg_ids[nh_id])
                nhg_ids.pop(nh_id)

            # Delete the CBF next hop group
            self.cbf_nhg_ps._del('cbfgroup1')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)

        # Test scenario:
        # - try creating invalid NHG maps and assert they're not being created
        def data_validation_test():
            # Test the data validation
            fv_values = [
                ('', 'cbfnhgmap1'), # empty members
                ('group1,group2', ''), # non-existent selection map
                ('group1,group1', 'cbfnhgmap1'), # non-unique members
            ]

            for members, selection_map in fv_values:
                fvs = swsscommon.FieldValuePairs([('members', members),
                                                    ('selection_map', selection_map)])
                self.cbf_nhg_ps.set('cbfgroup1', fvs)
                time.sleep(1)
                assert(len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count + 2)

        # Test scenario:
        # - create a CBF NHG
        # - try deleting one of its members and assert it fails as it is being referenced
        # - update the group replacing the to-be-deleted member with another NHG and assert it is deleted
        # - update the CBF NHG reordering the members and assert the new details match
        def update_cbf_nhg_members_test():
            # Create a NHG with a single next hop
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1'), ('ifname', 'Ethernet0')])
            self.nhg_ps.set("_group3", fvs)
            fvs = swsscommon.FieldValuePairs([('nexthop_group','_group3')])
            self.nhg_ps.set("group3", fvs)

            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)

            # Create a CBF NHG
            fvs = swsscommon.FieldValuePairs([('members', 'group1,group3'),
                                                ('selection_map', 'cbfnhgmap1')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 4)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 8)

            # Remove group1 while being referenced - should not work
            self.nhg_ps._del('group1')
            time.sleep(1)
            assert(self.nhg_exists('group1'))

            # Update the CBF NHG replacing group1 with group2
            nhgm_ids = self.get_nhgm_ids('cbfgroup1')
            fvs = swsscommon.FieldValuePairs([('members', 'group2,group3'),
                                                ('selection_map', 'cbfnhgmap1')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            self.asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)
            # group1 should be deleted and the updated members added
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 5)

            # Update the CBF NHG changing the order of the members
            nhgm_ids = self.get_nhgm_ids('cbfgroup1')
            fvs = swsscommon.FieldValuePairs([('members', 'group3,group2'),
                                                ('selection_map', 'cbfnhgmap1')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            self.asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 5)
            assert(len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count + 3)

            indexes = {
                self.get_nhg_id('group3'): '0',
                self.get_nhg_id('group2'): '1'
            }
            nhgm_ids = self.get_nhgm_ids('cbfgroup1')
            for nhgm_id in nhgm_ids:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
                nh_id = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']
                assert(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX'] == indexes[nh_id])
                indexes.pop(nh_id)

        # Test scenario:
        # - update the selection map of the CBF NHG and assert it changes in ASIC DB
        def update_cbf_nhg_map_test():
            # Create an FC to NH index selection map
            fvs = swsscommon.FieldValuePairs([('0', '1'), ('1', '0')])
            self.fc_to_nhg_ps.set('cbfnhgmap2', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 2)

            # Update the CBF NHG class map
            nhg_id = self.get_nhg_id('cbfgroup1')
            old_map = self.get_nhg_map_id('cbfnhgmap1')
            fvs = swsscommon.FieldValuePairs([('members', 'group3,group2'),
                                                ('selection_map', 'cbfnhgmap2')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            self.asic_db.wait_for_field_negative_match(self.ASIC_NHG_STR,
                                                nhg_id,
                                                {'SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP': old_map})
            assert(len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count + 3)

        # Test scenario:
        # - create a route pointing to the CBF NHG
        # - try deleting the CBF NHG and assert it fails as it is being referenced
        # - delete the route and assert the CBF NHG also gets deleted
        def delete_referenced_cbf_nhg_test():
            # Create a route pointing to the CBF NHG
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'cbfgroup1')])
            self.lr_ps.set('10', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, self.asic_insgs_count + 1)

            # Try deleting the CBF NHG - should not work
            self.cbf_nhg_ps._del('cbfgroup1')
            time.sleep(1)
            assert(self.nhg_exists('cbfgroup1'))

            # Delete the label route - the CBF NHG should also get deleted
            self.lr_ps._del('10')
            self.asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, self.asic_insgs_count)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 3)

        # Test scenario:
        # - create a route pointing to a CBF NHG that doesn't exist and assert it isn't created
        # - create the CBF NHG and assert the route also gets created
        def create_route_inexistent_cbf_nhg_test():
            # Create a route pointing to a CBF NHG that does not yet exist
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'cbfgroup1')])
            self.rt_ps.set('2.2.2.0/24', fvs)
            time.sleep(1)
            assert(not self.route_exists('2.2.2.0/24'))

            # Create the CBF NHG
            fvs = swsscommon.FieldValuePairs([('members', 'group3,group2'),
                                                ('selection_map', 'cbfnhgmap2')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 5)
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 1)

        # Test scenario:
        # - try deleting the CBF NHG and assert it fails as it is being referenced
        # - update the CBF NHG
        # - delete the route pointing to the CBF NHG and assert the CBF NHG doesn't get deleted
        def update_deleting_cbf_nhg_test():
            # Try deleting the CBF group
            self.cbf_nhg_ps._del('cbfgroup1')
            time.sleep(1)
            assert(self.nhg_exists('cbfgroup1'))

            # Update the CBF group
            nhgm_ids = self.get_nhgm_ids('cbfgroup1')
            fvs = swsscommon.FieldValuePairs([('members', 'group2,group3'), ('selection_map', 'cbfnhgmap1')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            self.asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 5)

            # Delete the route
            self.rt_ps._del('2.2.2.0/24')
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count)
            # The update should have overwritten the delete for CBF NHG
            assert(self.nhg_exists('cbfgroup1'))

        # Test scenario:
        # - try updating the CBF NHG with a member that doesn't exist and assert the CBF NHG's
        # - create the missing NHG and assert the CBF NHG's member also gets created
        def update_cbf_nhg_inexistent_member_test():
            # Create an FC to NH index selection map that references just 1 group member
            fvs = swsscommon.FieldValuePairs([('0', '0')])
            self.fc_to_nhg_ps.set('cbfnhgmap4', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 3)

            # Update the CBF NHG referencing an NHG that doesn't exist. In the end, create the NHG and
            # make sure everything works fine.
            fvs = swsscommon.FieldValuePairs([('members', 'group2'), ('selection_map', 'cbfnhgmap4')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            time.sleep(1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)
            assert(len(self.asic_db.get_keys(self.ASIC_NHGM_STR)) == self.asic_nhgms_count)
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                                ('ifname', 'Ethernet0,Ethernet4')])
            self.nhg_ps.set('group2', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 3)

        # Test scenario:
        # - try updating the CBF NHG with a NHG map that doesn't exist and assert it does't change
        # - create the missing NHG map and assert the selection map also gets updated
        def update_cbf_nhg_inexistent_map_test():
            # Update the CBF NHG with an NHG map that doesn't exist, then update it with one that does exist.
            nhg_id = self.get_nhg_id('cbfgroup1')
            smap_id = self.asic_db.get_entry(self.ASIC_NHG_STR, nhg_id)['SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP']
            fvs = swsscommon.FieldValuePairs([('members', 'group2'), ('selection_map', 'a')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            time.sleep(1)
            assert(self.asic_db.get_entry(self.ASIC_NHG_STR, nhg_id)['SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP'] == smap_id)
            fvs = swsscommon.FieldValuePairs([('members', 'group2'), ('selection_map', 'cbfnhgmap4')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)

        # Test scenario:
        # - create a NHG that points to a map that refers to more members than the group has
        def create_cbf_invalid_nhg_map_test():
            # Create an FC to NH index selection map that references 3 group members
            fvs = swsscommon.FieldValuePairs([('0', '1'), ('1', '0'), ('2', '2')])
            self.fc_to_nhg_ps.set('cbfnhgmap3', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 2)

            # Create a group that references this map. It doesn't get created.
            fvs = swsscommon.FieldValuePairs([('members', 'group3,group2'),
                                                ('selection_map', 'cbfnhgmap3')])
            self.cbf_nhg_ps.set('cbfgroup3', fvs)
            time.sleep(1)
            assert(not self.nhg_exists('cbfgroup3'))

            # Cleanup
            self.cbf_nhg_ps._del('cbfgroup3')

        self.init_test(dvs, 4)

        mainline_cbf_nhg_test()
        data_validation_test()
        update_cbf_nhg_members_test()
        update_cbf_nhg_map_test()
        delete_referenced_cbf_nhg_test()
        create_route_inexistent_cbf_nhg_test()
        update_deleting_cbf_nhg_test()
        create_cbf_invalid_nhg_map_test()

        # Delete the NHGs
        self.cbf_nhg_ps._del('cbfgroup1')
        self.nhg_ps._del('group2')
        self.nhg_ps._del('group3')
        self.nhg_ps._del('_group3')

        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)
        self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count)

        update_cbf_nhg_inexistent_member_test()
        update_cbf_nhg_inexistent_map_test()

        # Coverage testing: Update the CBF NHG with a member that does not exist.
        fvs = swsscommon.FieldValuePairs([('members', 'group3'), ('selection_map', 'cbfnhgmap2')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)

        # Delete the NHGs
        self.cbf_nhg_ps._del('cbfgroup1')
        self.nhg_ps._del('group2')
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

        # Delete the NHG maps
        self.fc_to_nhg_ps._del('cbfnhgmap1')
        self.fc_to_nhg_ps._del('cbfnhgmap2')
        self.fc_to_nhg_ps._del('cbfnhgmap3')
        self.fc_to_nhg_ps._del('cbfnhgmap4')
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count)

        # Coverage testing: Delete inexistent CBF NHG
        self.cbf_nhg_ps._del('cbfgroup1')

    def test_nhg_map(self, dvs, testlog):

        # Test scenario:
        # - create a NHG map and assert the expected details
        # - update the map
        # - delete the map
        def mainline_nhg_map_test():
            # Create two non-CBF NHGs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
            self.nhg_ps.set("group1", fvs)

            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                                ('ifname', 'Ethernet0,Ethernet4')])
            self.nhg_ps.set('group2', fvs)

            # Wait for the groups to appear in ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)

            # Create an FC to NH index map
            fvs = swsscommon.FieldValuePairs([('0', '0')])
            self.fc_to_nhg_ps.set('cbfnhgmap1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 1)

            # Check the NHG map attributes
            nhg_map_id = self.get_nhg_map_id('cbfnhgmap1')
            assert nhg_map_id != None

            self.asic_db.wait_for_field_match(self.ASIC_NHG_MAP_STR,
                                        nhg_map_id,
                                        {'SAI_NEXT_HOP_GROUP_MAP_ATTR_TYPE': 'SAI_NEXT_HOP_GROUP_MAP_TYPE_FORWARDING_CLASS_TO_INDEX',
                                        'SAI_NEXT_HOP_GROUP_MAP_ATTR_MAP_TO_VALUE_LIST': '{\"count\":1,\"list\":[{\"key\":0,\"value\":0}]}'})

            # Create a CBF next hop group
            fvs = swsscommon.FieldValuePairs([('members', 'group1,group2'),
                                        ('selection_map', 'cbfnhgmap1')])
            self.cbf_nhg_ps.set('cbfgroup1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)

            # Update the map.
            fvs = swsscommon.FieldValuePairs([('0', '0'), ('1', '1')])
            self.fc_to_nhg_ps.set('cbfnhgmap1', fvs)

            self.asic_db.wait_for_field_match(self.ASIC_NHG_MAP_STR,
                                        nhg_map_id,
                                        {'SAI_NEXT_HOP_GROUP_MAP_ATTR_TYPE': 'SAI_NEXT_HOP_GROUP_MAP_TYPE_FORWARDING_CLASS_TO_INDEX',
                                        'SAI_NEXT_HOP_GROUP_MAP_ATTR_MAP_TO_VALUE_LIST': '{\"count\":2,\"list\":[{\"key\":1,\"value\":1},{\"key\":0,\"value\":0}]}'})

            # Delete the group
            self.cbf_nhg_ps._del('cbfgroup1')
            self.nhg_ps._del("group1")
            self.nhg_ps._del("group2")
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

            # Delete the map
            self.fc_to_nhg_ps._del('cbfnhgmap1')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count)

        # Test scenario:
        # - try creating different NHG maps with invalid data and assert they don't get created
        def data_validation_test():
            # Test validation errors
            nhg_maps = [
                ('-1', '0'), # negative FC
                ('63', '0'), # greater than max FC value
                ('a', '0'), # non-integer FC
                ('0', '-1'), # negative NH index
                ('0', 'a'), # non-integer NH index
            ]

            # Check that no such NHG map gets created
            for nhg_map in nhg_maps:
                fvs = swsscommon.FieldValuePairs([nhg_map])
                self.fc_to_nhg_ps.set('cbfnhgmap1', fvs)
                time.sleep(1)
                assert(not self.nhg_map_exists('cbfnhgmap1'))

        # Test scenario:
        # - create two CBF NHG and a NHG map
        # - try deleting the NHG map and assert it fails as it is being referenced
        # - delete a CBF NHG and assert the NHG map is still not deleted as it's still being referenced
        # - create a new NHG map and update the CBF NHG to point to it and assert the previous NHG map gets deleted
        def delete_referenced_nhg_map_test():
            # Create two non-CBF NHGs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
            self.nhg_ps.set("group1", fvs)

            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                                ('ifname', 'Ethernet0,Ethernet4')])
            self.nhg_ps.set('group2', fvs)

            # Wait for the groups to appear in ASIC DB
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)

            # Create an FC to NH index map
            fvs = swsscommon.FieldValuePairs([('0', '0')])
            self.fc_to_nhg_ps.set('cbfnhgmap1', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 1)

            # Create a couple of CBF NHGs
            for i in range(2):
                fvs = swsscommon.FieldValuePairs([('members', 'group{}'.format(i + 1)),
                                            ('selection_map', 'cbfnhgmap1')])
                self.cbf_nhg_ps.set('cbfgroup{}'.format(i), fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 4)
            # Try deleting the NHG map. It should fail as it is referenced.
            self.fc_to_nhg_ps._del('cbfnhgmap1')
            time.sleep(1)
            assert(self.nhg_map_exists('cbfnhgmap1'))

            # Delete a CBF NHG
            self.cbf_nhg_ps._del('cbfgroup1')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)

            # The NHG map is still referenced so shouldn't be deleted
            assert(self.nhg_map_exists('cbfnhgmap1'))

            # Create a new FC to NH index map
            fvs = swsscommon.FieldValuePairs([('0', '0')])
            self.fc_to_nhg_ps.set('cbfnhgmap2', fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 2)

            # Update the second CBF NHG to point to the new map
            fvs = swsscommon.FieldValuePairs([('members', 'group1'), ('selection_map', 'cbfnhgmap2')])
            self.cbf_nhg_ps.set('cbfgroup0', fvs)

            # The NHG map is no longer referenced and should have been deleted
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 1)

        # Test scenario:
        # - delete the NHG map and assert it fails as it's being referenced
        # - update the NHG map and assert for the new details
        # - delete the referencing CBF NHG and assert the NHG map isn't deleted as it was updated
        # - delete the NHG map
        def update_override_delete_test():
            # Delete the second NHG map. It fails as it is referenced.
            self.fc_to_nhg_ps._del('cbfnhgmap2')
            time.sleep(1)
            assert(self.nhg_map_exists('cbfnhgmap2'))

            # Update the NHG map
            nhg_map_id = self.get_nhg_map_id('cbfnhgmap2')
            fvs = swsscommon.FieldValuePairs([('1', '0'), ('2', '0')])
            self.fc_to_nhg_ps.set('cbfnhgmap2', fvs)
            self.asic_db.wait_for_field_match(self.ASIC_NHG_MAP_STR,
                                        nhg_map_id,
                                        {'SAI_NEXT_HOP_GROUP_MAP_ATTR_TYPE': 'SAI_NEXT_HOP_GROUP_MAP_TYPE_FORWARDING_CLASS_TO_INDEX',
                                        'SAI_NEXT_HOP_GROUP_MAP_ATTR_MAP_TO_VALUE_LIST': '{\"count\":2,\"list\":[{\"key\":2,\"value\":0},{\"key\":1,\"value\":0}]}'})

            # Delete the second CBF NHG
            self.cbf_nhg_ps._del('cbfgroup0')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)

            # The NHG map should not be deleted as it was updated
            time.sleep(1)
            assert(self.nhg_map_exists('cbfnhgmap2'))

            # Delete the NHG map
            self.fc_to_nhg_ps._del('cbfnhgmap2')
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count)

        # Test scenario:
        # - fill the ASIC with NHG maps
        # - try creating a new NHG map and assert it fails as the ASIC is full
        # - delete a NHG map and assert the to-be-created one is actually created
        def nhg_map_exhaust_test():
            # Create the maximum number of NHG maps allowed by the VS switch (512)
            fvs = swsscommon.FieldValuePairs([('0', '0')])
            for i in range(512 - self.asic_nhg_maps_count):
                nhg_maps.append('cbfnhgmap{}'.format(i))
                self.fc_to_nhg_ps.set('cbfnhgmap{}'.format(i), fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.MAX_NHG_MAP_COUNT)

            # Try creating a new NHG map. It should fail as there is no more space.
            nhg_maps.append('cbfnhgmap512')
            self.fc_to_nhg_ps.set('cbfnhgmap512', fvs)
            time.sleep(1)
            assert(not self.nhg_map_exists('cbfnhgmap512'))
            time.sleep(1)

            # Delete an existing NHG map. The pending NHG map should now be created
            del_nhg_map_index = nhg_maps.pop(0)
            del_nhg_map_id = self.get_nhg_map_id(del_nhg_map_index)
            self.fc_to_nhg_ps._del(del_nhg_map_index)
            self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_MAP_STR, del_nhg_map_id)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.MAX_NHG_MAP_COUNT)
            assert(self.nhg_map_exists('cbfnhgmap512'))

        # Test scenario:
        # - create a NHG map while the ASIC is full
        # - create a CBF NHG pointing to this NHG map and assert it isn't created
        # - delete a NHG map and assert the new NHG map and the CBF NHG map is created
        # - delete the CBF NHG
        def create_cbf_nhg_inexistent_map_test():
            # Create a new NHG map. It should remain pending.
            nhg_maps.append('cbfnhgmap513')
            fvs = swsscommon.FieldValuePairs([('0', '0')])
            self.fc_to_nhg_ps.set('cbfnhgmap513', fvs)

            # Create a CBF NHG which should reference this NHG map. It should fail creating it.
            fvs = swsscommon.FieldValuePairs([('members', 'group1'), ('selection_map', 'cbfnhgmap513')])
            self.cbf_nhg_ps.set('testcbfnhg', fvs)
            time.sleep(1)
            assert(not self.nhg_exists('testcbfnhg'))

            # Delete an existing NHG map. The new map and the CBF NHG should be created.
            self.fc_to_nhg_ps._del(nhg_maps.pop(0))
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.MAX_NHG_MAP_COUNT)

            self.cbf_nhg_ps._del('testcbfnhg')

        self.init_test(dvs, 4)
        nhg_maps = []

        mainline_nhg_map_test()
        data_validation_test()
        delete_referenced_nhg_map_test()
        update_override_delete_test()
        nhg_map_exhaust_test()
        create_cbf_nhg_inexistent_map_test()

        # Cleanup

        # Delete the NHGs
        for i in range(2):
            self.nhg_ps._del('group{}'.format(i + 1))
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)

        # Delete all the NHG maps
        while len(nhg_maps) > 0:
            self.fc_to_nhg_ps._del(nhg_maps.pop())
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count)

    # Test scenario:
    # - Create a CBF NHG that has a member which is not yet synced. It shouldn't be synced.
    # - Add the missing member and assert the CBF NHG is now synced.
    def test_cbf_sync_before_member(self, dvs, testlog):
        self.init_test(dvs, 2)

        # Create an FC to NH index selection map
        nhg_map = [(str(i), '0' if i < 4 else '1') for i in range(8)]
        fvs = swsscommon.FieldValuePairs(nhg_map)
        self.fc_to_nhg_ps.set('cbfnhgmap1', fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_MAP_STR, self.asic_nhg_maps_count + 1)

        # Create a non-CBF NHG
        fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
        self.nhg_ps.set('group1', fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 1)

        # Create a CBF NHG with a member that doesn't currently exist. Nothing should happen
        fvs = swsscommon.FieldValuePairs([('members', 'group1,group2'),
                                        ('selection_map', 'cbfnhgmap1')])
        self.cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        assert(len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count + 1)

        # Create the missing non-CBF NHG. This and the CBF NHG should be created.
        fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                        ("ifname", "Ethernet0,Ethernet4")])
        self.nhg_ps.set("group2", fvs)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 3)

        # Cleanup
        self.nhg_ps._del('cbfgroup1')
        self.nhg_ps._del('group1')
        self.nhg_ps._del('group2')
        self.nhg_ps._del('cbfnhgmap1')

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
