import time
import json
import random
import pytest

from swsscommon import swsscommon
from flaky import flaky
from pprint import pprint


@pytest.mark.flaky
class TestVrf(object):
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
        fvs = filter(lambda x : x != ('NULL', 'NULL'), fvs)

        attr_keys = {entry[0] for entry in fvs}
        assert attr_keys == set(expected_attributes.keys())

        for name, value in fvs:
            assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])


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
        for an in xrange(len(attributes)):
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
        ns = [random.randint(0, 255) for _ in xrange(6)]
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

        random.seed(int(time.clock()))

        for n in xrange(2**len(attributes)):
            # generate testcases for all combinations of attributes
            req_attr = []
            exp_attr = {}
            vrf_name = "Vrf_%d" % n
            bmask = 0x1
            for an in xrange(len(attributes)):
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

        random.seed(int(time.clock()))

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

    def test_VRFMgr_Capacity(self, dvs, testlog):
        self.setup_db(dvs)

        initial_entries_cnt = self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")

        maximum_vrf_cnt = 999

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
