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

    def port_name(self, i):
        return "Ethernet" + str(i * 4)

    def port_ip(self, i):
        return "10.0.0." + str(i * 2)

    def port_ipprefix(self, i):
        return self.port_ip(i) + "/31"

    def peer_ip(self, i):
        return "10.0.0." + str(i * 2 + 1)

    def port_mac(self, i):
        return "00:00:00:00:00:0" + str(i)

    def config_intf(self, i):
        fvs = {'NULL': 'NULL'}

        self.config_db.create_entry("INTERFACE", self.port_name(i), fvs)
        self.config_db.create_entry("INTERFACE", "{}|{}".format(self.port_name(i), self.port_ipprefix(i)), fvs)
        self.dvs.runcmd("config interface startup " + self.port_name(i))
        self.dvs.runcmd("arp -s {} {}".format(self.peer_ip(i), self.port_mac(i)))
        assert self.dvs.servers[i].runcmd("ip link set down dev eth0") == 0
        assert self.dvs.servers[i].runcmd("ip link set up dev eth0") == 0

    def flap_intf(self, i, status):
        assert status in ['up', 'down']

        self.dvs.servers[i].runcmd("ip link set {} dev eth0".format(status)) == 0
        time.sleep(2)
        fvs = self.dvs.get_app_db().get_entry("PORT_TABLE", "Ethernet%d" % (i * 4))
        assert bool(fvs)
        assert fvs["oper_status"] == status

    def init_test(self, dvs, num_intfs):
        self.dvs = dvs
        self.app_db = self.dvs.get_app_db()
        self.asic_db = self.dvs.get_asic_db()
        self.config_db = self.dvs.get_config_db()
        self.nhg_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_NEXTHOP_GROUP_TABLE_NAME)
        self.rt_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_ROUTE_TABLE_NAME)
        self.lr_ps = swsscommon.ProducerStateTable(self.app_db.db_connection, swsscommon.APP_LABEL_ROUTE_TABLE_NAME)

        for i in range(num_intfs):
            self.config_intf(i)

        self.asic_nhgs_count = len(self.asic_db.get_keys(self.ASIC_NHG_STR))
        self.asic_nhgms_count = len(self.asic_db.get_keys(self.ASIC_NHGM_STR))
        self.asic_insgs_count = len(self.asic_db.get_keys(self.ASIC_INSEG_STR))
        self.asic_nhs_count = len(self.asic_db.get_keys(self.ASIC_NHS_STR))
        self.asic_rts_count = len(self.asic_db.get_keys(self.ASIC_RT_STR))

    def nhg_exists(self, nhg_index):
        return self.get_nhg_id(nhg_index) is not None

class TestNextHopGroupExhaust(TestNextHopGroupBase):
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

    def test_nhgorch_nhg_exhaust(self, dvs, testlog):
        def gen_nhg_index(nhg_number):
            return "group{}".format(nhg_number)

        def create_temp_nhg():
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            nhg_index = gen_nhg_index(self.nhg_count)
            self.nhg_ps.set(nhg_index, nhg_fvs)
            self.nhg_count += 1

            return nhg_index, binary

        def delete_nhg():
            del_nhg_index = gen_nhg_index(self.first_valid_nhg)
            del_nhg_id = self.asic_nhgs[del_nhg_index]

            self.nhg_ps._del(del_nhg_index)
            self.asic_nhgs.pop(del_nhg_index)
            self.first_valid_nhg += 1

            return del_nhg_id

        # Test scenario:
        # - create a NHG and assert a NHG object doesn't get added to ASIC DB
        # - delete a NHG and assert the newly created one is created in ASIC DB and its SAI ID changed
        def test_temporary_group_promotion():
            # Add a new next hop group - it should create a temporary one instead
            prev_nhgs = self.asic_db.get_keys(self.ASIC_NHG_STR)
            nhg_index, _ = create_temp_nhg()

            # Save the temporary NHG's SAI ID
            time.sleep(1)
            nhg_id = self.get_nhg_id(nhg_index)

            # Assert no new group has been added
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

            # Assert the same NHGs are in ASIC DB
            assert prev_nhgs == self.asic_db.get_keys(self.ASIC_NHG_STR)

            # Delete an existing next hop group
            del_nhg_id = delete_nhg()

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
        def test_group_update():
            # Update a group
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            nhg_index = gen_nhg_index(self.first_valid_nhg)

            # Save the previous members
            prev_nhg_members = self.get_nhgm_ids(nhg_index)
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # Wait a second so the NHG members get updated
            time.sleep(1)

            # Assert the group was updated by checking it's members
            assert self.get_nhgm_ids(nhg_index) != prev_nhg_members

        # Test scenario:
        # - create and delete a NHG while the ASIC DB is full and assert nothing changes
        def test_create_delete_temporary():
            # Create a new temporary group
            nhg_index, _ = create_temp_nhg()
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
        def test_update_temporary_group():
            # Create a new temporary group
            nhg_index, binary = create_temp_nhg()

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
            del_nhg_id = delete_nhg()

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
        def test_route_nhg_update():
            # Add a route
            nhg_index = gen_nhg_index(self.first_valid_nhg)
            rt_fvs = swsscommon.FieldValuePairs([('nexthop_group', nhg_index)])
            self.rt_ps.set('2.2.2.0/24', rt_fvs)

            # Assert the route is created
            self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count + 1)

            # Save the previous NHG ID
            prev_nhg_id = self.asic_nhgs[nhg_index]

            # Create a new temporary group
            nhg_index, binary = create_temp_nhg()

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
            del_nhg_id = delete_nhg()

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
        def test_labeled_nhg_temporary_promotion():
            # Create a next hop group that contains labeled NHs that do not exist
            # in NeighOrch
            self.asic_nhs_count = len(self.asic_db.get_keys(self.ASIC_NHS_STR))
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('mpls_nh', 'push1,push3'),
                                                ('ifname', 'Ethernet0,Ethernet4')])
            nhg_index = gen_nhg_index(self.nhg_count)
            self.nhg_ps.set(nhg_index, fvs)
            self.nhg_count += 1

            # A temporary next hop should be elected to represent the group and
            # thus a new labeled next hop should be created
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 1)

            # Delete a next hop group
            delete_nhg()

            # The group should be promoted and the other labeled NH should also get
            # created
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)
            self.asic_db.wait_for_n_keys(self.ASIC_NHS_STR, self.asic_nhs_count + 2)

            # Save the promoted NHG index/ID
            self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

        # Test scenario:
        # - update route to own its NHG and assert no new NHG is added
        # - remove a NHG and assert the temporary NHG is promoted and added to ASIC DB
        def test_back_compatibility():
            # Update the route with a RouteOrch's owned NHG
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            self.rt_ps.set('2.2.2.0/24', nhg_fvs)

            # Assert no new group has been added
            time.sleep(1)
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

            # Delete a next hop group
            del_nhg_id = delete_nhg()
            self.asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

            # The temporary group should be promoted
            self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

        # Test scenario:
        # - create a NHG with all NHs not existing and assert the NHG is not created
        # - update the NHG to have valid NHs and assert a temporary NHG is created
        # - update the NHG to all invalid NHs again and assert the update is not performed and thus it has the same SAI
        #   ID
        # - delete the temporary NHG
        def test_invalid_temporary():
            # Create a temporary NHG that contains only NHs that do not exist
            nhg_fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.21,10.0.0.23'),
                                                    ('ifname', 'Ethernet40,Ethernet44')])
            nhg_index = gen_nhg_index(self.nhg_count)
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

        self.nhg_count = self.asic_nhgs_count
        self.first_valid_nhg = self.nhg_count
        self.asic_nhgs = {}

        # Add first batch of next hop groups to reach the NHG limit
        while self.nhg_count < self.MAX_ECMP_COUNT:
            binary = self.gen_valid_binary()
            nhg_fvs = self.gen_nhg_fvs(binary)
            nhg_index = gen_nhg_index(self.nhg_count)
            self.nhg_ps.set(nhg_index, nhg_fvs)

            # Save the NHG index/ID pair
            self.asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index)

            # Increase the number of NHGs in ASIC DB
            self.nhg_count += 1
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.MAX_ECMP_COUNT)

        test_temporary_group_promotion()
        test_group_update()
        test_create_delete_temporary()
        test_update_temporary_group()
        test_route_nhg_update()
        test_labeled_nhg_temporary_promotion()
        test_back_compatibility()
        test_invalid_temporary()

        # Cleanup

        # Delete the route
        self.rt_ps._del('2.2.2.0/24')
        self.asic_db.wait_for_n_keys(self.ASIC_RT_STR, self.asic_rts_count)

        # Delete the next hop groups
        for k in self.asic_nhgs:
            self.nhg_ps._del(k)
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count)


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
            binary = self.gen_valid_binary()
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

class TestNextHopGroup(TestNextHopGroupBase):

    def test_route_nhg(self, dvs, dvs_route, testlog):
        self.init_test(dvs, 3)

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

        keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

        assert len(keys) == 3

        for k in keys:
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

            # verify weight attributes not in asic db
            assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT") is None

        # Remove route 2.2.2.0/24
        self.rt_ps._del(rtprefix)

        # Wait for route 2.2.2.0/24 to be removed
        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        # Negative test with nexthops with incomplete weight info
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"),
                                          ("ifname", "Ethernet0,Ethernet4,Ethernet8"),
                                          ("weight", "10,30")])
        self.rt_ps.set(rtprefix, fvs)

        # check if route was propagated to ASIC DB
        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        # assert the route points to next hop group
        fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", rtkeys[0])

        nhgid = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nhgid)

        assert bool(fvs)

        keys = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")

        assert len(keys) == 3

        for k in keys:
            fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

            # verify weight attributes not in asic db
            assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT") is None

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
        fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", rtkeys[0])

        nhgid = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nhgid)

        assert bool(fvs)

        keys = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")

        assert len(keys) == 3

        for k in keys:
            fvs = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

            # verify weight attributes in asic db
            nhid = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
            weight = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT"]

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

        keys = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")

        assert len(keys) == 2

        keys = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")

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
        for i in [0, 1, 2]:
            self.flap_intf(i, 'up')

            keys = self.asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == i + 1

            for k in keys:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, k)
                assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

        # Remove route 2.2.2.0/24
        self.rt_ps._del(rtprefix)

        # Wait for route 2.2.2.0/24 to be removed
        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

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
        def test_mainline_labeled_nhs():
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

            # Assert the weights of the NHGMs are the expected ones
            nhgm_ids = self.get_nhgm_ids('group1')
            weights = []
            for nhgm_id in nhgm_ids:
                fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
                weights.append(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT'])
            assert weights == ['0', '0']

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
        def test_routeorch_nhgorch_interop():
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

        test_mainline_labeled_nhs()
        test_routeorch_nhgorch_interop()

    def test_nhgorch_excp_group_cases(self, dvs, testlog):
        # Test scenario:
        # - remove a NHG that does not exist and assert the number of NHGs in ASIC DB remains the same
        def test_remove_inexistent_nhg():
            # Remove a group that does not exist
            self.nhg_ps._del("group1")
            time.sleep(1)
            assert len(self.asic_db.get_keys(self.ASIC_NHG_STR)) == self.asic_nhgs_count

        # Test scenario:
        # - create a NHG with a member which does not exist and assert no NHG is created
        # - update the NHG to contain all valid members and assert the NHG is created and it has 2 members
        def test_nhg_members_validation():
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
        def test_remove_referenced_nhg():
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
        def test_routeorch_nhgorch_interop():
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
        def test_identical_nhgs():
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
        def test_create_route_inexistent_nhg():
            # Add a route with a NHG that does not exist
            fvs = swsscommon.FieldValuePairs([('nexthop_group', 'group3')])
            self.rt_ps.set('2.2.3.0/24', fvs)
            time.sleep(1)
            assert self.get_route_id('2.2.3.0/24') is None

            # Remove the pending route
            self.rt_ps._del('2.2.3.0/24')

        self.init_test(dvs, 3)

        test_remove_inexistent_nhg()
        test_nhg_members_validation()
        test_remove_referenced_nhg()
        test_routeorch_nhgorch_interop()
        test_identical_nhgs()
        test_create_route_inexistent_nhg()

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
        # - create NHG 'group1' and assert it is being added to ASIC DB along with its members
        def test_create_nhg():
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
        def test_create_route_nhg():
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
        def test_link_flap():
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
        def test_validate_invalidate_group_member():
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
        def test_inexistent_group_member():
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
        def test_update_nhgm_count():
            # Update the NHG, adding two new members
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7'),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12")])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 4)
            assert len(self.get_nhgm_ids('group1')) == 4

            # Update the group to one NH only
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1'), ("ifname", "Ethernet0")])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 1)
            assert len(self.get_nhgm_ids('group1')) == 1

            # Update the group to 2 NHs
            fvs = swsscommon.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'), ("ifname", "Ethernet0,Ethernet4")])
            self.nhg_ps.set("group1", fvs)
            self.asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, self.asic_nhgms_count + 2)
            assert len(self.get_nhgm_ids('group1')) == 2

        self.init_test(dvs, 4)

        test_create_nhg()
        test_create_route_nhg()
        test_link_flap()
        test_validate_invalidate_group_member()
        test_inexistent_group_member()
        test_update_nhgm_count()

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

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
