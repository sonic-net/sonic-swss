import json
import time

from swsscommon import swsscommon

CFG_DOT1P_TO_TC_MAP_TABLE_NAME =  "DOT1P_TO_TC_MAP"
CFG_DOT1P_TO_TC_MAP_KEY = "AZURE"
DOT1P_TO_TC_MAP = {
    "0": "0",
    "1": "6",
    "2": "5",
    "3": "3",
    "4": "4",
    "5": "2",
    "6": "1",
    "7": "7",
}

CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME =  "MPLS_TC_TO_TC_MAP"
CFG_MPLS_TC_TO_TC_MAP_KEY = "AZURE_MPLS_TC"
MPLS_TC_TO_TC_MAP = {
    "0": "0",
    "1": "4",
    "2": "1",
    "3": "3",
    "4": "5",
    "5": "2",
    "6": "7",
    "7": "6",
}

CFG_PORT_QOS_MAP_TABLE_NAME =  "PORT_QOS_MAP"
CFG_PORT_QOS_DOT1P_MAP_FIELD = "dot1p_to_tc_map"
CFG_PORT_QOS_MPLS_TC_MAP_FIELD = "mpls_tc_to_tc_map"
CFG_PORT_TABLE_NAME = "PORT"


class TestDot1p(object):
    def connect_dbs(self, dvs):
        self.asic_db = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)


    def create_dot1p_profile(self):
        tbl = swsscommon.Table(self.config_db, CFG_DOT1P_TO_TC_MAP_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs(list(DOT1P_TO_TC_MAP.items()))
        tbl.set(CFG_DOT1P_TO_TC_MAP_KEY, fvs)
        time.sleep(1)

    def find_dot1p_profile(self):
        found = False
        dot1p_tc_map_raw = None
        tbl = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST":
                    dot1p_tc_map_raw = fv[1]
                elif fv[0] == "SAI_QOS_MAP_ATTR_TYPE" and fv[1] == "SAI_QOS_MAP_TYPE_DOT1P_TO_TC":
                    found = True

            if found:
                break

        assert found == True

        return (key, dot1p_tc_map_raw)


    def apply_dot1p_profile_on_all_ports(self):
        tbl = swsscommon.Table(self.config_db, CFG_PORT_QOS_MAP_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([(CFG_PORT_QOS_DOT1P_MAP_FIELD, CFG_DOT1P_TO_TC_MAP_KEY)])
        ports = swsscommon.Table(self.config_db, CFG_PORT_TABLE_NAME).getKeys()
        for port in ports:
            tbl.set(port, fvs)

        time.sleep(1)


    def test_dot1p_cfg(self, dvs):
        self.connect_dbs(dvs)
        self.create_dot1p_profile()
        _, dot1p_tc_map_raw = self.find_dot1p_profile()

        dot1p_tc_map = json.loads(dot1p_tc_map_raw)
        for dot1p2tc in dot1p_tc_map['list']:
            dot1p = str(dot1p2tc['key']['dot1p'])
            tc = str(dot1p2tc['value']['tc'])
            assert tc == DOT1P_TO_TC_MAP[dot1p]


    def test_port_dot1p(self, dvs):
        self.connect_dbs(dvs)
        self.create_dot1p_profile()
        oid, _ = self.find_dot1p_profile()

        self.apply_dot1p_profile_on_all_ports()

        cnt = 0
        tbl = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_QOS_DOT1P_TO_TC_MAP":
                    cnt += 1
                    assert fv[1] == oid

        port_cnt = len(swsscommon.Table(self.config_db, CFG_PORT_TABLE_NAME).getKeys())
        assert port_cnt == cnt

class TestCbf(object):
    ASIC_QOS_MAP_STR = "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP"
    ASIC_PORT_STR = "ASIC_STATE:SAI_OBJECT_TYPE_PORT"

    def init_test(self, dvs):
        self.dvs = dvs
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.asic_qos_map_ids = self.asic_db.get_keys(self.ASIC_QOS_MAP_STR)
        self.asic_qos_map_count = len(self.asic_qos_map_ids)
        self.dscp_ps = swsscommon.Table(self.config_db.db_connection, swsscommon.CFG_DSCP_TO_FC_MAP_TABLE_NAME)
        self.exp_ps = swsscommon.Table(self.config_db.db_connection, swsscommon.CFG_EXP_TO_FC_MAP_TABLE_NAME)

        # Set switch FC capability to 63
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_MAX_NUMBER_OF_FORWARDING_CLASSES', '63')

    def get_qos_id(self):
        diff = set(self.asic_db.get_keys(self.ASIC_QOS_MAP_STR)) - set(self.asic_qos_map_ids)
        assert len(diff) <= 1
        return None if len(diff) == 0 else diff.pop()

    def test_dscp_to_fc(self, dvs):
        self.init_test(dvs)

        # Create a DSCP_TO_FC map
        dscp_map = [(str(i), str(i)) for i in range(0, 63)]
        self.dscp_ps.set("AZURE", swsscommon.FieldValuePairs(dscp_map))

        self.asic_db.wait_for_n_keys(self.ASIC_QOS_MAP_STR, self.asic_qos_map_count + 1)

        # Get the DSCP map ID
        dscp_map_id = self.get_qos_id()
        assert(dscp_map_id is not None)

        # Assert the expected values
        fvs = self.asic_db.get_entry(self.ASIC_QOS_MAP_STR, dscp_map_id)
        assert(fvs.get("SAI_QOS_MAP_ATTR_TYPE") == "SAI_QOS_MAP_TYPE_DSCP_TO_FORWARDING_CLASS")

        # Modify the map
        dscp_map = [(str(i), '0') for i in range(0, 63)]
        self.dscp_ps.set("AZURE", swsscommon.FieldValuePairs(dscp_map))
        time.sleep(1)

        # Assert the expected values
        fvs = self.asic_db.get_entry(self.ASIC_QOS_MAP_STR, dscp_map_id)
        sai_dscp_map = json.loads(fvs.get("SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST"))

        for dscp2fc in sai_dscp_map['list']:
            fc = str(dscp2fc['value']['fc'])
            assert fc == '0'

        # Delete the map
        self.dscp_ps._del("AZURE")
        self.asic_db.wait_for_deleted_entry(self.ASIC_QOS_MAP_STR, dscp_map_id)

        # Test validation
        maps = [
            ('-1', '0'), # negative DSCP
            ('64', '0'), # DSCP greater than max value
            ('0', '-1'), # negative FC
            ('0', '63'), # FC greater than max value
            ('a', '0'), # non-integer DSCP
            ('0', 'a'), # non-integet FC
        ]

        for fvs in maps:
            self.dscp_ps.set('AZURE', swsscommon.FieldValuePairs([fvs]))
            time.sleep(1)
            assert(self.asic_qos_map_count == len(self.asic_db.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP')))
            self.dscp_ps._del("AZURE")

        # Delete a map that does not exist.  Nothing should happen
        self.dscp_ps._del("AZURE")
        time.sleep(1)
        assert(len(self.asic_db.get_keys(self.ASIC_QOS_MAP_STR)) == self.asic_qos_map_count)

    def test_exp_to_fc(self, dvs):
        self.init_test(dvs)

        # Create a EXP_TO_FC map
        exp_map = [(str(i), str(i)) for i in range(0, 8)]
        self.exp_ps.set("AZURE", swsscommon.FieldValuePairs(exp_map))

        self.asic_db.wait_for_n_keys(self.ASIC_QOS_MAP_STR, self.asic_qos_map_count + 1)

        # Get the EXP map ID
        exp_map_id = self.get_qos_id()

        # Assert the expected values
        fvs = self.asic_db.get_entry(self.ASIC_QOS_MAP_STR, exp_map_id)
        assert(fvs.get("SAI_QOS_MAP_ATTR_TYPE") == "SAI_QOS_MAP_TYPE_MPLS_EXP_TO_FORWARDING_CLASS")

        # Modify the map
        exp_map = [(str(i), '0') for i in range(0, 8)]
        self.exp_ps.set("AZURE", swsscommon.FieldValuePairs(exp_map))
        time.sleep(1)

        # Assert the expected values
        fvs = self.asic_db.get_entry(self.ASIC_QOS_MAP_STR, exp_map_id)
        sai_exp_map = json.loads(fvs.get("SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST"))

        for exp2fc in sai_exp_map['list']:
            fc = str(exp2fc['value']['fc'])
            assert fc == '0'

        # Delete the map
        self.exp_ps._del("AZURE")
        self.asic_db.wait_for_deleted_entry(self.ASIC_QOS_MAP_STR, exp_map_id)

        # Test validation
        maps = [
            ('-1', '0'), # negative EXP
            ('8', '0'), # EXP greater than max value
            ('0', '-1'), # negative FC
            ('0', '63'), # FC greater than max value
            ('a', '0'), # non-integer EXP
            ('0', 'a'), # non-integet FC
        ]

        for fvs in maps:
            self.exp_ps.set('AZURE', swsscommon.FieldValuePairs([fvs]))
            time.sleep(1)
            assert(self.asic_qos_map_count == len(self.asic_db.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP')))
            self.exp_ps._del("AZURE")

        # Update the map with valid values
        exp_map = [(str(i), str(i + 10)) for i in range(0, 8)]
        self.exp_ps.set('AZURE', swsscommon.FieldValuePairs(exp_map))
        self.asic_db.wait_for_n_keys(self.ASIC_QOS_MAP_STR, self.asic_qos_map_count + 1)

        # Delete the map
        exp_map_id = self.get_qos_id()
        self.exp_ps._del("AZURE")
        self.asic_db.wait_for_deleted_entry(self.ASIC_QOS_MAP_STR, exp_map_id)

        # Delete a map that does not exist.  Nothing should happen
        self.exp_ps._del("AZURE")
        time.sleep(1)
        assert(len(self.asic_db.get_keys(self.ASIC_QOS_MAP_STR)) == self.asic_qos_map_count)

    def test_per_port_cbf_binding(self, dvs):
        self.init_test(dvs)

        # Create a DSCP_TO_FC map
        dscp_map = [(str(i), str(i)) for i in range(0, 63)]
        self.dscp_ps.set("AZURE", swsscommon.FieldValuePairs(dscp_map))
        self.asic_db.wait_for_n_keys(self.ASIC_QOS_MAP_STR, self.asic_qos_map_count + 1)
        dscp_map_id = self.get_qos_id()
        self.asic_qos_map_ids = self.asic_db.get_keys(self.ASIC_QOS_MAP_STR)

        # Create a EXP_TO_FC map
        exp_map = [(str(i), str(i)) for i in range(0, 8)]
        self.exp_ps.set("AZURE", swsscommon.FieldValuePairs(exp_map))
        self.asic_db.wait_for_n_keys(self.ASIC_QOS_MAP_STR, self.asic_qos_map_count + 2)
        exp_map_id = self.get_qos_id()

        tbl = swsscommon.Table(self.config_db.db_connection, swsscommon.CFG_PORT_QOS_MAP_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([('dscp_to_fc_map', "AZURE"),
                                    ('exp_to_fc_map', "AZURE")])
        keys = self.config_db.get_keys(swsscommon.CFG_PORT_TABLE_NAME)
        for key in keys:
            tbl.set(key, fvs)
        time.sleep(1)

        dscp_cnt = 0
        exp_cnt = 0
        for key in self.asic_db.get_keys(self.ASIC_PORT_STR):
            fvs = self.asic_db.get_entry(self.ASIC_PORT_STR, key)
            if 'SAI_PORT_ATTR_QOS_DSCP_TO_FORWARDING_CLASS_MAP' in fvs:
                assert fvs['SAI_PORT_ATTR_QOS_DSCP_TO_FORWARDING_CLASS_MAP'] == dscp_map_id
                dscp_cnt += 1
            if 'SAI_PORT_ATTR_QOS_MPLS_EXP_TO_FORWARDING_CLASS_MAP' in fvs:
                assert fvs['SAI_PORT_ATTR_QOS_MPLS_EXP_TO_FORWARDING_CLASS_MAP'] == exp_map_id
                exp_cnt += 1
        assert dscp_cnt == exp_cnt == len(keys)

class TestMplsTc(object):
    def connect_dbs(self, dvs):
        self.asic_db = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)


    def create_mpls_tc_profile(self):
        tbl = swsscommon.Table(self.config_db, CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs(list(MPLS_TC_TO_TC_MAP.items()))
        tbl.set(CFG_MPLS_TC_TO_TC_MAP_KEY, fvs)
        time.sleep(1)


    def find_mpls_tc_profile(self):
        found = False
        mpls_tc_tc_map_raw = None
        tbl = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST":
                    mpls_tc_tc_map_raw = fv[1]
                elif fv[0] == "SAI_QOS_MAP_ATTR_TYPE" and fv[1] == "SAI_QOS_MAP_TYPE_MPLS_EXP_TO_TC":
                    found = True

            if found:
                break

        assert found == True

        return (key, mpls_tc_tc_map_raw)


    def apply_mpls_tc_profile_on_all_ports(self):
        tbl = swsscommon.Table(self.config_db, CFG_PORT_QOS_MAP_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([(CFG_PORT_QOS_MPLS_TC_MAP_FIELD, CFG_MPLS_TC_TO_TC_MAP_KEY)])
        ports = swsscommon.Table(self.config_db, CFG_PORT_TABLE_NAME).getKeys()
        for port in ports:
            tbl.set(port, fvs)

        time.sleep(1)


    def test_mpls_tc_cfg(self, dvs):
        self.connect_dbs(dvs)
        self.create_mpls_tc_profile()
        _, mpls_tc_tc_map_raw = self.find_mpls_tc_profile()

        mpls_tc_tc_map = json.loads(mpls_tc_tc_map_raw)
        for mplstc2tc in mpls_tc_tc_map['list']:
            mpls_tc = str(mplstc2tc['key']['mpls_exp'])
            tc = str(mplstc2tc['value']['tc'])
            assert tc == MPLS_TC_TO_TC_MAP[mpls_tc]


    def test_port_mpls_tc(self, dvs):
        self.connect_dbs(dvs)
        self.create_mpls_tc_profile()
        oid, _ = self.find_mpls_tc_profile()

        self.apply_mpls_tc_profile_on_all_ports()

        cnt = 0
        tbl = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_QOS_MPLS_EXP_TO_TC_MAP":
                    cnt += 1
                    assert fv[1] == oid

        port_cnt = len(swsscommon.Table(self.config_db, CFG_PORT_TABLE_NAME).getKeys())
        assert port_cnt == cnt

class TestDscpToTcMap(object):
    ASIC_QOS_MAP_STR = "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP"
    ASIC_PORT_STR = "ASIC_STATE:SAI_OBJECT_TYPE_PORT"
    ASIC_SWITCH_STR = "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH"

    def init_test(self, dvs):
        dvs.setup_db()
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.asic_qos_map_ids = self.asic_db.get_keys(self.ASIC_QOS_MAP_STR)
        self.asic_qos_map_count = len(self.asic_qos_map_ids)
        self.dscp_to_tc_table = swsscommon.Table(self.config_db.db_connection, swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME)
        self.port_qos_table = swsscommon.Table(self.config_db.db_connection, swsscommon.CFG_PORT_QOS_MAP_TABLE_NAME)

    def get_qos_id(self):
        diff = set(self.asic_db.get_keys(self.ASIC_QOS_MAP_STR)) - set(self.asic_qos_map_ids)
        assert len(diff) <= 1
        return None if len(diff) == 0 else diff.pop()
    
    def test_dscp_to_tc_map_applied_to_switch(self, dvs):
        self.init_test(dvs)
        dscp_to_tc_map_id = None
        created_new_map = False
        try:
            existing_map = self.dscp_to_tc_table.getKeys()
            if "AZURE" not in existing_map: 
                # Create a DSCP_TO_TC map
                dscp_to_tc_map = [(str(i), str(i)) for i in range(0, 63)]
                self.dscp_to_tc_table.set("AZURE", swsscommon.FieldValuePairs(dscp_to_tc_map))

                self.asic_db.wait_for_n_keys(self.ASIC_QOS_MAP_STR, self.asic_qos_map_count + 1)

                # Get the DSCP_TO_TC map ID
                dscp_to_tc_map_id = self.get_qos_id()
                assert(dscp_to_tc_map_id is not None)

                # Assert the expected values
                fvs = self.asic_db.get_entry(self.ASIC_QOS_MAP_STR, dscp_to_tc_map_id)
                assert(fvs.get("SAI_QOS_MAP_ATTR_TYPE") == "SAI_QOS_MAP_TYPE_DSCP_TO_TC")
                created_new_map = True
            else:
                for id in self.asic_qos_map_ids:
                    fvs = self.asic_db.get_entry(self.ASIC_QOS_MAP_STR, id)
                    if fvs.get("SAI_QOS_MAP_ATTR_TYPE") == "SAI_QOS_MAP_TYPE_DSCP_TO_TC":
                        dscp_to_tc_map_id = id
                        break
            switch_oid = dvs.getSwitchOid()
            # Check switch level DSCP_TO_TC_MAP doesn't before PORT_QOS_MAP|global is created
            fvs = self.asic_db.get_entry(self.ASIC_SWITCH_STR, switch_oid)
            assert("SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP" not in fvs)

            # Insert switch level map entry 
            self.port_qos_table.set("global", [("dscp_to_tc_map", "AZURE")])
            time.sleep(1)

            # Check the switch level DSCP_TO_TC_MAP is applied
            fvs = self.asic_db.get_entry(self.ASIC_SWITCH_STR, switch_oid)
            assert(fvs.get("SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP") == dscp_to_tc_map_id)

            # Remove the global level DSCP_TO_TC_MAP
            self.port_qos_table._del("global")
            time.sleep(1)

            # Check the global level DSCP_TO_TC_MAP is set to SAI_
            fvs = self.asic_db.get_entry(self.ASIC_SWITCH_STR, switch_oid)
            assert(fvs.get("SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP") == "oid:0x0")
        finally:
            if created_new_map:
                self.dscp_to_tc_table._del("AZURE")
        

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
