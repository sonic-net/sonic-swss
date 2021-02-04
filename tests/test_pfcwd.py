import redis
import time
import os
import pytest
import re
import json

from swsscommon import swsscommon

# Helper functions
def enable_device_pfcwd(config_db):
    device_meta = config_db.get_entry('DEVICE_METADATA', 'localhost')
    device_meta['default_pfcwd_status'] = 'enable'
    config_db.update_entry('DEVICE_METADATA', 'localhost', device_meta)
    time.sleep(20)

def setPortPfc(dvs, port_name, pfc_queues):
   cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

   port_qos_tbl = swsscommon.Table(cfg_db, 'PORT_QOS_MAP')
   fvs = swsscommon.FieldValuePairs([('pfc_enable', ",".join(str(q) for q in pfc_queues))])
   port_qos_tbl.set(port_name, fvs)

   time.sleep(1)

def getBitMaskStr(bits):
    mask = 0

    for b in bits:
        mask = mask | 1 << b

    return str(mask)

def getPortOid(dvs, port_name):
    cnt_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    port_map_tbl = swsscommon.Table(cnt_db, 'COUNTERS_PORT_NAME_MAP')

    for k in port_map_tbl.get('')[1]:
        if k[0] == port_name:
            return k[1]

    return ''

def getPortAttr(dvs, port_oid, port_attr):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    port_tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_PORT:{0}'.format(port_oid))

    for k in port_tbl.get('')[1]:
        if k[0] == port_attr:
            return k[1]

    return ''

def getQueueOid(dvs, queue):
   cnt_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.COUNTERS_DB,
                       encoding="utf-8", decode_responses=True)
   return  cnt_r.hget("COUNTERS_QUEUE_NAME_MAP", queue)

def setPfcStormQueueOid(dvs, queue_oid):
   cnt_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.COUNTERS_DB,
                       encoding="utf-8", decode_responses=True)
   cnt_r.hset("COUNTERS:"+queue_oid, "DEBUG_STORM","enabled")
   import pdb;pdb.set_trace()

def get_acl_table_id(self, dvs):
    tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
    keys = tbl.getKeys()

    for k in  dvs.asicdb.default_acl_tables:
        assert k in keys

    acl_tables = [k for k in keys if k not in dvs.asicdb.default_acl_tables]
    if len(acl_tables) == 1:
        return acl_tables[0]
    else:
        return None

def verify_acl_group_num(self, expt):
    atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
    acl_table_groups = atbl.getKeys()
    assert len(acl_table_groups) == expt

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

def verify_acl_group_member(self, acl_group_ids, acl_table_id):
    atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
    keys = atbl.getKeys()

    member_groups = []
    for k in keys:
        (status, fvs) = atbl.get(k)
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID":
                assert fv[1] in acl_group_ids
                member_groups.append(fv[1])
            elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID":
                assert fv[1] == acl_table_id
            elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY":
                assert True
            else:
                assert False

    assert set(member_groups) == set(acl_group_ids)

def verify_acl_port_binding(self, dvs, bind_ports):
    atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
    acl_table_groups = atbl.getKeys()
    assert len(acl_table_groups) == len(bind_ports)

    atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
    port_groups = []
    for p in [dvs.asicdb.portnamemap[portname] for portname in bind_ports]:
        (status, fvs) = atbl.get(p)
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_ACL":
                assert fv[1] in acl_table_groups
                port_groups.append(fv[1])

    assert len(port_groups) == len(bind_ports)
    assert set(port_groups) == set(acl_table_groups)

def Verify_AclTableCreation(self, dvs):
    # check acl table in asic db
    acl_table_id = get_acl_table_id(self, dvs)
    assert acl_table_id is not None

    # check acl table group in asic db
    verify_acl_group_num(2)

    # get acl table group ids and verify the id numbers
    atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
    acl_group_ids = atbl.getKeys()
    assert len(acl_group_ids) == 2

    # check acl table group member
    verify_acl_group_member(acl_group_ids, acl_table_id)

    # check port binding
    verify_acl_port_binding(dvs, "Ethernet0")

def Verify_AclRuleInPorts(self, dvs):
   # check acl rule table in asic db
   acl_table_id = self.get_acl_table_id(dvs)

   atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
   keys = atbl.getKeys()

   acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
   assert len(acl_entry) == 1

   (status, fvs) = atbl.get(acl_entry[0])
   assert status == True

   value = dict(fvs)
   assert value["SAI_ACL_ENTRY_ATTR_TABLE_ID"] == acl_table_id
   assert value["SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION"] == "SAI_PACKET_ACTION_DROP"
   in_ports = value["SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS"]
   assert dvs.asicdb.portnamemap["Ethernet0"] in in_ports


# PFCWD test class
class TestPfcWD(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

    @pytest.fixture(scope="module", autouse=True)
    def enable_queue_counters(self, dvs):
        dvs.runcmd("counterpoll queue enable")

    def test_ingress_pfcwd_handler(self, dvs):
        self.setup_db(dvs)
        config_db = dvs.get_config_db()
        
        #Enable PFC at global level
        enable_device_pfcwd(config_db)

        #Enable PFCWD on the ports
        port_list = ["Ethernet0","Ethernet4","Ethernet8","Ethernet12"]
        pfc_queues = [3,4]
        
        for port in port_list:
           setPortPfc(dvs, port, pfc_queues)
       
        # Get SAI object ID for the interface
        port_oid = getPortOid(dvs, 'Ethernet0')

        # Verify default PFC is set to configured value
        pfc = getPortAttr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL')
        assert pfc == getBitMaskStr(pfc_queues)

        # Find OID for the queue 3
        queue_oid = getQueueOid(dvs, "Ethernet0:3")

       # Enable PFC Storm on queue 3
        setPfcStormQueueOid(dvs, queue_oid)

       # Verify ASIC_DB for table creation
        Verify_AclTableCreation(self, dvs)

       #Verify ASIC_DB for rule creation
 #      Verify_AclRuleInPorts(dvs)
        
           
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
