from swsscommon import swsscommon


import time
import re
import json

class TestDtel(object):
    def test_DtelGlobalAttribs(self, dvs):
    
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)
    
        # create DTel global attributes in config db
        tbl = swsscommon.Table(db, "DTEL")

        fvs = swsscommon.FieldValuePairs([("SWITCH_ID", "1")])
        tbl.set("SWITCH_ID", fvs)

        fvs = swsscommon.FieldValuePairs([("FLOW_STATE_CLEAR_CYCLE", "10")])
        tbl.set("FLOW_STATE_CLEAR_CYCLE", fvs)

        fvs = swsscommon.FieldValuePairs([("LATENCY_SENSITIVITY", "100")])
        tbl.set("LATENCY_SENSITIVITY", fvs)

        fvs = swsscommon.FieldValuePairs([("INT_ENDPOINT", "TRUE")])
        tbl.set("INT_ENDPOINT", fvs)

        fvs = swsscommon.FieldValuePairs([("INT_TRANSIT", "TRUE")])
        tbl.set("INT_TRANSIT", fvs)

        fvs = swsscommon.FieldValuePairs([("POSTCARD", "TRUE")])
        tbl.set("POSTCARD", fvs)

        fvs = swsscommon.FieldValuePairs([("DROP_REPORT", "TRUE")])
        tbl.set("DROP_REPORT", fvs)

        fvs = swsscommon.FieldValuePairs([("QUEUE_REPORT", "TRUE")])
        tbl.set("QUEUE_REPORT", fvs)

        fvs = swsscommon.FieldValuePairs([("Ethernet0", "Ethernet0"), ("Ethernet4", "Ethernet4")])  
        tbl.set("SINK_PORT_LIST", fvs)

        fvs = swsscommon.FieldValuePairs([("INT_L4_DSCP_VALUE", "128"), ("INT_L4_DSCP_MASK", "255")])  
        tbl.set("INT_L4_DSCP", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_DTEL")
        keys = atbl.getKeys()
        assert len(keys) > 0
        
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True
            
            for fv in fvs:
                if fv[0] == "SAI_DTEL_ATTR_SWITCH_ID":
                    assert fv[1] == "1"
                elif fv[0] == "SAI_DTEL_ATTR_FLOW_STATE_CLEAR_CYCLE":
                    assert fv[1] == "10"
                elif fv[0] == "SAI_DTEL_ATTR_LATENCY_SENSITIVITY":
                    assert fv[1] == "100"
                elif fv[0] == "SAI_DTEL_ATTR_INT_ENDPOINT_ENABLE":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_ATTR_INT_TRANSIT_ENABLE":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_ATTR_POSTCARD_ENABLE":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_ATTR_DROP_REPORT_ENABLE":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_ATTR_QUEUE_REPORT_ENABLE":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_ATTR_INT_L4_DSCP":
                    assert fv[1] == "128&mask:0xff"
                elif fv[0] == "SAI_DTEL_ATTR_SINK_PORT_LIST":
                    assert True

        tbl._del("SWITCH_ID")
        tbl._del("FLOW_STATE_CLEAR_CYCLE")
        tbl._del("LATENCY_SENSITIVITY")
        tbl._del("INT_ENDPOINT")
        tbl._del("INT_TRANSIT")
        tbl._del("POSTCARD")
        tbl._del("DROP_REPORT")
        tbl._del("QUEUE_REPORT")
        tbl._del("SINK_PORT_LIST")
    
    def test_DtelReportSessionAttribs(self, dvs):
    
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)
    
        # create DTel report session attributes in config db
        tbl = swsscommon.Table(db, "DTEL_REPORT_SESSION")

        fvs = swsscommon.FieldValuePairs([("SRC_IP", "10.10.10.1"), 
                                          ("DST_IP_LIST", "20.20.20.1;20.20.20.2;20.20.20.3"), 
                                          ("VRF", "default"), 
                                          ("TRUNCATE_SIZE", "256"), 
                                          ("UDP_DEST_PORT", "2000")])  
        tbl.set("RS-1", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_DTEL_REPORT_SESSION")
        keys = atbl.getKeys()
        assert len(keys) > 0
        
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True
            
            for fv in fvs:
                if fv[0] == "SAI_DTEL_REPORT_SESSION_ATTR_SRC_IP":
                    assert fv[1] == "10.10.10.1"
                elif fv[0] == "SAI_DTEL_REPORT_SESSION_ATTR_DST_IP_LIST":
                    assert fv[1] == "3:20.20.20.1,20.20.20.2,20.20.20.3"
                elif fv[0] == "SAI_DTEL_REPORT_SESSION_ATTR_VIRTUAL_ROUTER_ID":
                    assert True
                elif fv[0] == "SAI_DTEL_REPORT_SESSION_ATTR_TRUNCATE_SIZE":
                    assert fv[1] == "256"
                elif fv[0] == "SAI_DTEL_REPORT_SESSION_ATTR_UDP_DST_PORT":
                    assert fv[1] == "2000"
                else:
                    assert False

        tbl._del("RS-1")

    def test_DtelINTSessionAttribs(self, dvs):
    
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)
    
        # create DTel INT session attributes in config db
        tbl = swsscommon.Table(db, "DTEL_INT_SESSION")

        fvs = swsscommon.FieldValuePairs([("MAX_HOP_COUNT", "50"), 
                                          ("COLLECT_SWITCH_ID", "TRUE"), 
                                          ("COLLECT_INGRESS_TIMESTAMP", "TRUE"), 
                                          ("COLLECT_EGRESS_TIMESTAMP", "TRUE"), 
                                          ("COLLECT_SWITCH_PORTS", "TRUE"), 
                                          ("COLLECT_QUEUE_INFO", "TRUE")])  
        tbl.set("INT-1", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_DTEL_INT_SESSION")
        keys = atbl.getKeys()
        assert len(keys) > 0
        
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True
            
            for fv in fvs:
                if fv[0] == "SAI_DTEL_INT_SESSION_ATTR_MAX_HOP_COUNT":
                    assert fv[1] == "50"
                elif fv[0] == "SAI_DTEL_INT_SESSION_ATTR_COLLECT_SWITCH_ID":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_INT_SESSION_ATTR_COLLECT_INGRESS_TIMESTAMP":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_INT_SESSION_ATTR_COLLECT_EGRESS_TIMESTAMP":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_INT_SESSION_ATTR_COLLECT_SWITCH_PORTS":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_INT_SESSION_ATTR_COLLECT_QUEUE_INFO":
                    assert fv[1] == "true"
                else:
                    assert False

        tbl._del("INT-1")

    def test_DtelQueueReportAttribs(self, dvs):
    
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)
    
        # create DTel queue report attributes in config db
        tbl = swsscommon.Table(db, "DTEL_QUEUE_REPORT")

        fvs = swsscommon.FieldValuePairs([("QUEUE_DEPTH_THRESHOLD", "1000"), 
                                          ("QUEUE_LATENCY_THRESHOLD", "2000"), 
                                          ("THRESHOLD_BREACH_QUOTA", "3000"), 
                                          ("REPORT_TAIL_DROP", "TRUE")])  
        tbl.set("Ethernet0|0", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_DTEL_QUEUE_REPORT")
        keys = atbl.getKeys()
        assert len(keys) > 0
        
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True
            
            for fv in fvs:
                if fv[0] == "SAI_DTEL_QUEUE_REPORT_ATTR_DEPTH_THRESHOLD":
                    assert fv[1] == "1000"
                elif fv[0] == "SAI_DTEL_QUEUE_REPORT_ATTR_LATENCY_THRESHOLD":
                    assert fv[1] == "2000"
                elif fv[0] == "SAI_DTEL_QUEUE_REPORT_ATTR_BREACH_QUOTA":
                    assert fv[1] == "3000"
                elif fv[0] == "SAI_DTEL_QUEUE_REPORT_ATTR_TAIL_DROP":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_DTEL_QUEUE_REPORT_ATTR_QUEUE_ID":
                    assert True
                else:
                    assert False

        tbl._del("Ethernet0|0")
    

    def test_DtelEventAttribs(self, dvs):
    
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)
    
        # first create DTel report session in config db
        rtbl = swsscommon.Table(db, "DTEL_REPORT_SESSION")

        fvs = swsscommon.FieldValuePairs([("SRC_IP", "10.10.10.1"), 
                                          ("DST_IP_LIST", "20.20.20.1;20.20.20.2;20.20.20.3"), 
                                          ("VRF", "default"), 
                                          ("TRUNCATE_SIZE", "256"), 
                                          ("UDP_DEST_PORT", "2000")])  
        rtbl.set("RS-1", fvs)

        # create DTel event attributes in config db
        tbl = swsscommon.Table(db, "DTEL_EVENT")

        fvs = swsscommon.FieldValuePairs([("EVENT_REPORT_SESSION", "RS-1"), 
                                          ("EVENT_DSCP_VALUE", "65")])  
        tbl.set("EVENT_TYPE_FLOW_STATE", fvs)

        fvs = swsscommon.FieldValuePairs([("EVENT_REPORT_SESSION", "RS-1"), 
                                          ("EVENT_DSCP_VALUE", "64")])  
        tbl.set("EVENT_TYPE_FLOW_REPORT_ALL_PACKETS", fvs)

        fvs = swsscommon.FieldValuePairs([("EVENT_REPORT_SESSION", "RS-1"), 
                                          ("EVENT_DSCP_VALUE", "63")])  
        tbl.set("EVENT_TYPE_FLOW_TCPFLAG", fvs)

        fvs = swsscommon.FieldValuePairs([("EVENT_REPORT_SESSION", "RS-1"), 
                                          ("EVENT_DSCP_VALUE", "62")])  
        tbl.set("EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH", fvs)

        fvs = swsscommon.FieldValuePairs([("EVENT_REPORT_SESSION", "RS-1"), 
                                          ("EVENT_DSCP_VALUE", "61")])  
        tbl.set("EVENT_TYPE_QUEUE_REPORT_TAIL_DROP", fvs)

        fvs = swsscommon.FieldValuePairs([("EVENT_REPORT_SESSION", "RS-1"), 
                                          ("EVENT_DSCP_VALUE", "60")])  
        tbl.set("EVENT_TYPE_DROP_REPORT", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_DTEL_EVENT")
        keys = atbl.getKeys()
        assert len(keys) > 0
        
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True
           
        expected_dscp = None 
        actual_dscp = None 
        for fv in fvs:
            if fv[0] == "SAI_DTEL_EVENT_ATTR_TYPE":
                if fv[1] == "SAI_DTEL_EVENT_TYPE_QUEUE_REPORT_TAIL_DROP":
                    expected_dscp = "61"
                elif fv[1] == "SAI_DTEL_EVENT_TYPE_DROP_REPORT":
                    expected_dscp = "60"
                elif fv[1] == "SAI_DTEL_EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH":
                    expected_dscp = "62"
                elif fv[1] == "SAI_DTEL_EVENT_TYPE_FLOW_TCPFLAG":
                    expected_dscp = "63"
                elif fv[1] == "SAI_DTEL_EVENT_TYPE_FLOW_REPORT_ALL_PACKETS":
                    expected_dscp = "64"
                elif fv[1] == "SAI_DTEL_EVENT_TYPE_FLOW_STATE":
                    expected_dscp = "65"
            elif fv[0] == "SAI_DTEL_EVENT_ATTR_REPORT_SESSION":
                assert True
            elif fv[0] == "SAI_DTEL_EVENT_ATTR_DSCP_VALUE":
                actual_dscp = fv[1]

        assert actual_dscp == expected_dscp

        rtbl._del("RS-1")
        tbl._del("EVENT_TYPE_FLOW_STATE")  
        tbl._del("EVENT_TYPE_FLOW_REPORT_ALL_PACKETS")
        tbl._del("EVENT_TYPE_FLOW_TCPFLAG")
        tbl._del("EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH")
        tbl._del("EVENT_TYPE_QUEUE_REPORT_TAIL_DROP")
        tbl._del("EVENT_TYPE_DROP_REPORT")
