from swsscommon import swsscommon
import time
import re
import json

class TestCopp(object):

    def create_copp_group_table(self, dvs, testlog):
        group_name = "trap.group.test"
        tbl = swsscommon.ProducerStateTable(dvs.pdb, "COPP_TABLE")
        fvs = swsscommon.FieldValuePairs([("trap_ids", "arp_resp"),
                                          ("trap_action", "copy"),
                                          ("trap_priority", "4"),
                                          ("queue","4"),
                                          ("meter_type", "packets"),
                                          ("mode", "tr_tcm"),
                                          ("cir", "600"),
                                          ("cbs", "600"),
                                          ("pir", "600"),
                                          ("pbs", "600"),
                                          ("red_action", "drop")])
        tbl.set(group_name, fvs)
        time.sleep(1)

    def remove_copp_group_table(self, dvs, testlog):
        group_name = "trap.group.test"
        tbl = swsscommon.ProducerStateTable(dvs.pdb, "COPP_TABLE")
        tbl._del(group_name)
        time.sleep(1)

    def verify_create_copp_group_table(self, dvs, testlog):
        tbl_trap_group = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP")
        keys = tbl_trap_group.getKeys()

        assert len(keys) == 2
        for k in keys:
            status, fvs = tbl_trap_group.get(k)
            for (field, value) in fvs:
                #ignore the default group
                if field == 'SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE':
                    assert value == '4'
                    test_group_key = k
                elif field == 'SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER':
                    assert value != 'NULL'
                else:
                    continue
        # Trap ids
        tbl_trap_ids = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP")
        keys = tbl_trap_ids.getKeys()
        assert len(keys) == 2
        for k in keys:
            status, fvs = tbl_trap_ids.get(k)
            for (field, value) in fvs:
                if field == 'SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP':
                    if value == test_group_key:
                        test_trap_table_fvs = fvs
                        break

        for (field, value) in test_trap_table_fvs:
            if field == 'SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE':
                assert value == 'SAI_HOSTIF_TRAP_TYPE_ARP_RESPONSE'
            elif field == 'SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION':
                assert value == 'SAI_PACKET_ACTION_COPY'
            elif field == 'SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY':
                assert value == '4'
            else:
                continue

    def verify_remove_copp_group_table(self, dvs, testlog):
        tbl_trap_group = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP")
        keys = tbl_trap_group.getKeys()
        #only the defaut group
        assert len(keys) == 1 
        for k in keys:
            status, fvs = tbl_trap_group.get(k)
            for (field, value) in fvs:
                assert field == 'NULL'
                assert value == 'NULL'
        # Trap ids
        tbl_trap_ids = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP")
        keys = tbl_trap_ids.getKeys()
        assert len(keys) == 1


    def test_add_copp_group_table(self, dvs, testlog):
        dvs.setup_db()
        self.create_copp_group_table(dvs, testlog)
        self.verify_create_copp_group_table(dvs, testlog)

    def test_remove_copp_group_table(self, dvs, testlog):
        dvs.setup_db()
        self.create_copp_group_table(dvs, testlog)
        self.verify_create_copp_group_table(dvs, testlog)
        self.remove_copp_group_table(dvs, testlog)
        self.verify_remove_copp_group_table(dvs, testlog)
