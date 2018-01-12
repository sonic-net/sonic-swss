from swsscommon import swsscommon
import time
import re
import json

class TestAcl(object):
    def get_acl_table_id(self, dvs, adb):
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        assert dvs.asicdb.default_acl_table in keys
        acl_tables = [k for k in keys if k not in dvs.asicdb.default_acl_table]
    
        assert len(acl_tables) == 1 

        return acl_tables[0]
 
    def test_AclTableCreation(self, dvs):
    
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
    
        bind_ports = ["Ethernet0", "Ethernet4"]
        # create ACL_TABLE in config db
        tbl = swsscommon.Table(db, "ACL_TABLE", '|')
        fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", ",".join(bind_ports))])
        tbl.set("test", fvs)
    
        time.sleep(1)
    
        # check acl table in asic db
        test_acl_table_id = self.get_acl_table_id(dvs, adb)
   
        # check acl table group in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_groups = atbl.getKeys()
        assert len(acl_table_groups) == 2
    
        for k in acl_table_groups:
            (status, fvs) = atbl.get(k)
            assert status == True
    
            for fv in fvs:
                if fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE":
                    assert fv[1] == "SAI_ACL_STAGE_INGRESS"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST":
                    assert fv[1] == "1:SAI_ACL_BIND_POINT_TYPE_PORT"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_TYPE":
                    assert fv[1] == "SAI_ACL_TABLE_GROUP_TYPE_PARALLEL"
                else:
                    assert False
    
        # check acl table group member
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        keys = atbl.getKeys()
        assert len(keys) == 2
    
        member_groups = []
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True
           
            assert len(fvs) == 3
            for fv in fvs:
                if fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID":
                    assert fv[1] in acl_table_groups
                    member_groups.append(fv[1])
                elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID":
                    assert fv[1] == test_acl_table_id
                elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY":
                    assert True
                else:
                    assert False
    
        assert set(member_groups) == set(acl_table_groups)
    
        # check port binding 
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
    
        port_groups = []
        for p in [dvs.asicdb.portnamemap[portname] for portname in bind_ports]:
            (status, fvs) = atbl.get(p)
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_INGRESS_ACL":
                    assert fv[1] in acl_table_groups
                    port_groups.append(fv[1])
    
        assert set(port_groups) == set(acl_table_groups)
    
    def test_AclRuleL4SrcPort(self, dvs):
        """
        hmset ACL_RULE|test|acl_test_rule priority 55 PACKET_ACTION FORWARD L4_SRC_PORT 65000
        """
    
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
    
        # create acl rule  
        tbl = swsscommon.Table(db, "ACL_RULE", '|')
        fvs = swsscommon.FieldValuePairs([("priority", "55"), ("PACKET_ACTION", "FORWARD"), ("L4_SRC_PORT", "65000")])
        tbl.set("test|acl_test_rule", fvs)
    
        time.sleep(1)
    
        test_acl_table_id = self.get_acl_table_id(dvs, adb)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()
    
        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1
    
        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "55"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT":
                assert fv[1] == "65000&mask:0xffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_FORWARD"
            else:
                assert False
    
        # remove acl rule
        tbl._del("test|acl_test_rule")
    
        time.sleep(1)
    
        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False
