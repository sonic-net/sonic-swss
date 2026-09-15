import time
import json
import random
import pytest

from swsscommon import swsscommon
from pprint import pprint
from dvslib.dvs_common import wait_for_result


class TestVrf(object):
    SENTINEL_METRIC = "4278198272"

    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def create_entry(self, tbl, key, pairs):
        fvs = swsscommon.FieldValuePairs(pairs)
        tbl.set(key, fvs)
        time.sleep(1)

    def create_entry_tbl(self, db, table, key, pairs):
        tbl = swsscommon.Table(db, table)
        self.create_entry(tbl, key, pairs)

    def delete_entry_tbl(self, db, table, key):
        tbl = swsscommon.Table(db, table)
        tbl._del(key)
        time.sleep(1)

    def how_many_entries_exist(self, db, table):
        tbl =  swsscommon.Table(db, table)
        return len(tbl.getKeys())

    def entries(self, db, table):
        tbl =  swsscommon.Table(db, table)
        return set(tbl.getKeys())

    def is_vrf_attributes_correct(self, db, table, key, expected_attributes):
        tbl =  swsscommon.Table(db, table)
        keys = set(tbl.getKeys())
        assert key in keys, "The created key wasn't found"

        status, fvs = tbl.get(key)
        assert status, "Got an error when get a key"

        # filter the fake 'NULL' attribute out
        fvs = [x for x in fvs if x != ('NULL', 'NULL')]

        attr_keys = {entry[0] for entry in fvs}
        assert attr_keys == set(expected_attributes.keys())

        for name, value in fvs:
            assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])

    def wait_for_kernel_link(self, dvs, vrf_name, present=True):
        def _access_function():
            status, output = dvs.runcmd(["ip", "link", "show", vrf_name])
            return (status == 0) == present, output

        wait_for_result(
            _access_function,
            failure_message="Kernel VRF {} was not {}".format(
                vrf_name, "created" if present else "removed"
            ),
        )

    def wait_for_sentinel(self, dvs, vrf_name, ipv6=False, present=True):
        command = ["ip"]
        if ipv6:
            command.append("-6")
        command.extend(["route", "show", "vrf", vrf_name])

        def _access_function():
            status, output = dvs.runcmd(command)
            found = False
            if status == 0:
                for line in output.splitlines():
                    fields = line.split()
                    if fields[:2] != ["unreachable", "default"]:
                        continue
                    found = any(
                        fields[index:index + 2] == ["metric", self.SENTINEL_METRIC]
                        for index in range(len(fields) - 1)
                    )
                    if found:
                        break
            return found == present, output

        wait_for_result(
            _access_function,
            failure_message="{} sentinel for {} was not {}".format(
                "IPv6" if ipv6 else "IPv4",
                vrf_name,
                "installed" if present else "removed",
            ),
        )

    def wait_for_unicast_default(self, dvs, vrf_name, interface, ipv6=False):
        command = ["ip"]
        if ipv6:
            command.append("-6")
        command.extend(["route", "show", "vrf", vrf_name])

        def _access_function():
            status, output = dvs.runcmd(command)
            if status == 0:
                for line in output.splitlines():
                    fields = line.split()
                    has_interface = any(
                        fields[index:index + 2] == ["dev", interface]
                        for index in range(len(fields) - 1)
                    )
                    has_metric = any(
                        fields[index:index + 2] == ["metric", "100"]
                        for index in range(len(fields) - 1)
                    )
                    if fields and fields[0] == "default" and \
                            has_interface and has_metric:
                        return True, output
            return False, output

        wait_for_result(
            _access_function,
            failure_message="{} unicast default for {} was not preserved".format(
                "IPv6" if ipv6 else "IPv4", vrf_name
            ),
        )

    def count_asic_link_local_routes(self):
        route_table = swsscommon.Table(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
        )
        count = 0
        for key in route_table.getKeys():
            try:
                if json.loads(key).get("dest") == "fe80::/10":
                    count += 1
            except (TypeError, ValueError):
                continue
        return count

    def wait_for_asic_link_local_routes(self, expected_count):
        def _access_function():
            count = self.count_asic_link_local_routes()
            return count == expected_count, count

        wait_for_result(
            _access_function,
            failure_message="ASIC link-local route count did not reach {}".format(
                expected_count
            ),
        )


    def vrf_create(self, dvs, vrf_name, attributes, expected_attributes):
        # check that the vrf wasn't exist before
        assert self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 1, "The initial state is incorrect"

        # read existing entries in the DB
        initial_entries = self.entries(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")

        # create a fake attribute if we don't have attributes in the request
        if len(attributes) == 0:
            attributes = [('empty', 'empty')]

        # create the VRF entry in Config DB
        self.create_entry_tbl(self.cdb, "VRF", vrf_name, attributes)

        # check vrf created in kernel
        (status, rslt) = dvs.runcmd("ip link show " + vrf_name)
        assert status == 0

        # check application database
        tbl = swsscommon.Table(self.pdb, "VRF_TABLE")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == vrf_name
        exp_attr = {}
        for an in range(len(attributes)):
            exp_attr[attributes[an][0]] = attributes[an][1]
        self.is_vrf_attributes_correct(self.pdb, "VRF_TABLE", vrf_name, exp_attr)

        # check that the vrf entry was created
        assert self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 2, "The vrf wasn't created"

        # find the id of the entry which was added
        added_entry_id = list(self.entries(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") - initial_entries)[0]

        # check correctness of the created attributes
        self.is_vrf_attributes_correct(
            self.adb,
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
            added_entry_id,
            expected_attributes,
        )

        state = {
            'initial_entries': initial_entries,
            'entry_id': added_entry_id,
        }

        return state


    def vrf_remove(self, dvs, vrf_name, state):
        # delete the created vrf entry
        self.delete_entry_tbl(self.cdb, "VRF", vrf_name)

        # check application database
        tbl = swsscommon.Table(self.pdb, "VRF_TABLE")
        intf_entries = tbl.getKeys()
        assert vrf_name not in intf_entries

        # check that the vrf entry was removed
        assert self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 1, "The vrf wasn't removed"

        # check that the correct vrf entry was removed
        assert state['initial_entries'] == self.entries(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"), "The incorrect entry was removed"

        # check vrf was removed from kernel
        (status, rslt) = dvs.runcmd("ip link show " + vrf_name)
        assert status != 0

    def vrf_update(self, vrf_name, attributes, expected_attributes, state):
        # update the VRF entry in Config DB
        self.create_entry_tbl(self.cdb, "VRF", vrf_name, attributes)

        # check correctness of the created attributes
        self.is_vrf_attributes_correct(
            self.adb,
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
            state['entry_id'],
            expected_attributes,
        )


    def boolean_gen(self):
        result = random.choice(['false', 'true'])
        return result, result


    def mac_addr_gen(self):
        ns = [random.randint(0, 255) for _ in range(6)]
        ns[0] &= 0xfe
        mac = ':'.join("%02x" % n for n in ns)
        return mac, mac.upper()


    def packet_action_gen(self):
        values = [
            ("drop",        "SAI_PACKET_ACTION_DROP"),
            ("forward",     "SAI_PACKET_ACTION_FORWARD"),
            ("copy",        "SAI_PACKET_ACTION_COPY"),
            ("copy_cancel", "SAI_PACKET_ACTION_COPY_CANCEL"),
            ("trap",        "SAI_PACKET_ACTION_TRAP"),
            ("log",         "SAI_PACKET_ACTION_LOG"),
            ("deny",        "SAI_PACKET_ACTION_DENY"),
            ("transit",     "SAI_PACKET_ACTION_TRANSIT"),
        ]

        r = random.choice(values)
        return r[0], r[1]

    def test_VRFMgr_Comprehensive(self, dvs, testlog):
        self.setup_db(dvs)

        attributes = [
            ('v4',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE',                     self.boolean_gen),
            ('v6',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE',                     self.boolean_gen),
            ('src_mac',       'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS',                    self.mac_addr_gen),
            ('ttl_action',    'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION',       self.packet_action_gen),
            ('ip_opt_action', 'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_IP_OPTIONS_PACKET_ACTION', self.packet_action_gen),
            ('l3_mc_action',  'SAI_VIRTUAL_ROUTER_ATTR_UNKNOWN_L3_MULTICAST_PACKET_ACTION', self.packet_action_gen),
        ]

        random.seed(int(time.time()))

        for n in range(2**len(attributes)):
            # generate testcases for all combinations of attributes
            req_attr = []
            exp_attr = {}
            vrf_name = "Vrf_%d" % n
            bmask = 0x1
            for an in range(len(attributes)):
                if (bmask & n) > 0:
                    req_res, exp_res = attributes[an][2]()
                    req_attr.append((attributes[an][0], req_res))
                    exp_attr[attributes[an][1]] = exp_res
                bmask <<= 1
            state = self.vrf_create(dvs, vrf_name, req_attr, exp_attr)
            self.vrf_remove(dvs, vrf_name, state)


    def test_VRFMgr(self, dvs, testlog):
        self.setup_db(dvs)

        state = self.vrf_create(dvs, "Vrf0",
            [
            ],
            {
            }
        )
        self.vrf_remove(dvs, "Vrf0", state)

        state = self.vrf_create(dvs, "Vrf1",
            [
                ('v4', 'true'),
                ('src_mac', '02:04:06:07:08:09'),
            ],
            {
                'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE':  'true',
                'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS': '02:04:06:07:08:09',
            }
        )
        self.vrf_remove(dvs, "Vrf1", state)

    def test_kernel_vrf_fallback_lifecycle(self, dvs, testlog):
        self.setup_db(dvs)
        fallback_table = swsscommon.Table(self.cdb, "KERNEL_VRF_FALLBACK")
        vrf_table = swsscommon.Table(self.cdb, "VRF")
        app_route_table = swsscommon.Table(self.pdb, "ROUTE_TABLE")
        asic_route_table = swsscommon.Table(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
        )
        vrfs = ["VrfFallback", "VrfFallbackNew"]
        initial_link_local_routes = self.count_asic_link_local_routes()

        fallback_table._del("GLOBAL")
        try:
            vrf_table.set(
                vrfs[0], swsscommon.FieldValuePairs([("fallback", "true")])
            )
            self.wait_for_kernel_link(dvs, vrfs[0])
            self.wait_for_sentinel(dvs, vrfs[0])
            self.wait_for_sentinel(dvs, vrfs[0], ipv6=True)
            self.wait_for_asic_link_local_routes(initial_link_local_routes + 1)

            app_routes_with_sentinels = set(app_route_table.getKeys())
            asic_routes_with_sentinels = set(asic_route_table.getKeys())

            fallback_table.set(
                "GLOBAL", swsscommon.FieldValuePairs([("status", "enabled")])
            )
            self.wait_for_sentinel(dvs, vrfs[0], present=False)
            self.wait_for_sentinel(dvs, vrfs[0], ipv6=True, present=False)
            assert set(app_route_table.getKeys()) == app_routes_with_sentinels
            assert set(asic_route_table.getKeys()) == asic_routes_with_sentinels

            vrf_table.set(
                vrfs[1], swsscommon.FieldValuePairs([("fallback", "false")])
            )
            self.wait_for_kernel_link(dvs, vrfs[1])
            self.wait_for_sentinel(dvs, vrfs[1], present=False)
            self.wait_for_sentinel(dvs, vrfs[1], ipv6=True, present=False)
            self.wait_for_asic_link_local_routes(initial_link_local_routes + 2)

            app_routes_without_sentinels = set(app_route_table.getKeys())
            asic_routes_without_sentinels = set(asic_route_table.getKeys())

            fallback_table._del("GLOBAL")
            for vrf_name in vrfs:
                self.wait_for_sentinel(dvs, vrf_name)
                self.wait_for_sentinel(dvs, vrf_name, ipv6=True)
            assert set(app_route_table.getKeys()) == app_routes_without_sentinels
            assert set(asic_route_table.getKeys()) == asic_routes_without_sentinels
        finally:
            fallback_table._del("GLOBAL")
            for vrf_name in reversed(vrfs):
                vrf_table._del(vrf_name)
            for vrf_name in vrfs:
                self.wait_for_kernel_link(dvs, vrf_name, present=False)
            self.wait_for_asic_link_local_routes(initial_link_local_routes)

    def test_kernel_vrf_fallback_preserves_unicast_defaults(self, dvs, testlog):
        self.setup_db(dvs)
        fallback_table = swsscommon.Table(self.cdb, "KERNEL_VRF_FALLBACK")
        vrf_table = swsscommon.Table(self.cdb, "VRF")
        vrf_name = "VrfFallbackRt"
        interface = "VrfFbDummy"
        initial_link_local_routes = self.count_asic_link_local_routes()

        fallback_table._del("GLOBAL")
        try:
            vrf_table.set(
                vrf_name, swsscommon.FieldValuePairs([("fallback", "false")])
            )
            self.wait_for_kernel_link(dvs, vrf_name)
            self.wait_for_sentinel(dvs, vrf_name)
            self.wait_for_sentinel(dvs, vrf_name, ipv6=True)
            self.wait_for_asic_link_local_routes(initial_link_local_routes + 1)

            commands = [
                ["ip", "link", "add", interface, "type", "dummy"],
                ["ip", "link", "set", interface, "master", vrf_name],
                ["sysctl", "-w", "net.ipv6.conf.{}.disable_ipv6=0".format(interface)],
                ["ip", "link", "set", interface, "up"],
                ["ip", "address", "replace", "192.0.2.1/24", "dev", interface],
                ["ip", "-6", "address", "replace", "2001:db8:ffff::1/64", "dev", interface],
                ["ip", "route", "replace", "vrf", vrf_name, "default", "dev", interface,
                 "metric", "100"],
                ["ip", "-6", "route", "replace", "vrf", vrf_name, "default", "dev",
                 interface, "metric", "100"],
            ]
            for command in commands:
                status, output = dvs.runcmd(command)
                assert status == 0, "Command failed: {}: {}".format(command, output)

            self.wait_for_unicast_default(dvs, vrf_name, interface)
            self.wait_for_unicast_default(dvs, vrf_name, interface, ipv6=True)

            fallback_table.set(
                "GLOBAL", swsscommon.FieldValuePairs([("status", "enabled")])
            )
            self.wait_for_sentinel(dvs, vrf_name, present=False)
            self.wait_for_sentinel(dvs, vrf_name, ipv6=True, present=False)
            self.wait_for_unicast_default(dvs, vrf_name, interface)
            self.wait_for_unicast_default(dvs, vrf_name, interface, ipv6=True)

            fallback_table.set(
                "GLOBAL", swsscommon.FieldValuePairs([("status", "disabled")])
            )
            self.wait_for_sentinel(dvs, vrf_name)
            self.wait_for_sentinel(dvs, vrf_name, ipv6=True)
            self.wait_for_unicast_default(dvs, vrf_name, interface)
            self.wait_for_unicast_default(dvs, vrf_name, interface, ipv6=True)
        finally:
            fallback_table._del("GLOBAL")
            dvs.runcmd(["ip", "link", "del", interface])
            vrf_table._del(vrf_name)
            self.wait_for_kernel_link(dvs, vrf_name, present=False)
            self.wait_for_asic_link_local_routes(initial_link_local_routes)

    def test_VRFMgr_Update(self, dvs, testlog):
        self.setup_db(dvs)

        attributes = [
            ('v4',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE',                     self.boolean_gen),
            ('v6',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE',                     self.boolean_gen),
            ('src_mac',       'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS',                    self.mac_addr_gen),
            ('ttl_action',    'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION',       self.packet_action_gen),
            ('ip_opt_action', 'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_IP_OPTIONS_PACKET_ACTION', self.packet_action_gen),
            ('l3_mc_action',  'SAI_VIRTUAL_ROUTER_ATTR_UNKNOWN_L3_MULTICAST_PACKET_ACTION', self.packet_action_gen),
        ]

        random.seed(int(time.time()))

        state = self.vrf_create(dvs, "Vrf_a",
            [
            ],
            {
            }
        )

        # try to update each attribute
        req_attr = []
        exp_attr = {}
        for attr in attributes:
            req_res, exp_res = attr[2]()
            req_attr.append((attr[0], req_res))
            exp_attr[attr[1]] = exp_res
            self.vrf_update("Vrf_a", req_attr, exp_attr, state)

        self.vrf_remove(dvs, "Vrf_a", state)

    @pytest.mark.xfail(reason="Test unstable, blocking PR builds")
    def test_VRFMgr_Capacity(self, dvs, testlog):
        self.setup_db(dvs)

        initial_entries_cnt = self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")

        maximum_vrf_cnt = 4096

        def create_entry(self, tbl, key, pairs):
            fvs = swsscommon.FieldValuePairs(pairs)
            tbl.set(key, fvs)
            time.sleep(1)

        def create_entry_tbl(self, db, table, key, pairs):
            tbl = swsscommon.Table(db, table)
            self.create_entry(tbl, key, pairs)

        # create the VRF entry in Config DB
        tbl = swsscommon.Table(self.cdb, "VRF")
        fvs = swsscommon.FieldValuePairs([('empty', 'empty')])
        for i in range(maximum_vrf_cnt):
            tbl.set("Vrf_%d" % i, fvs)

        # wait for all VRFs pushed to database and linux
        time.sleep(30)

        # check app_db
        intf_entries_cnt = self.how_many_entries_exist(self.pdb, "VRF_TABLE")
        assert intf_entries_cnt == maximum_vrf_cnt

        # check asic_db
        current_entries_cnt = self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        assert (current_entries_cnt - initial_entries_cnt) == maximum_vrf_cnt

        # check linux kernel
        (exitcode, num) = dvs.runcmd(['sh', '-c', "ip link show | grep Vrf | wc -l"])
        assert num.strip() == str(maximum_vrf_cnt)

        # remove VRF from Config DB
        for i in range(maximum_vrf_cnt):
            tbl._del("Vrf_%d" % i)

        # wait for all VRFs deleted
        time.sleep(120)

        # check app_db
        intf_entries_cnt = self.how_many_entries_exist(self.pdb, "VRF_TABLE")
        assert intf_entries_cnt == 0

        # check asic_db
        current_entries_cnt = self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        assert (current_entries_cnt - initial_entries_cnt) == 0

        # check linux kernel
        (exitcode, num) = dvs.runcmd(['sh', '-c', "ip link show | grep Vrf | wc -l"])
        assert num.strip() == '0'


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
