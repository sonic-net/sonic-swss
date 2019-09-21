from swsscommon import swsscommon
import os
import re
import time
import json
import redis
import pytest

class TestThreshold(object):

    def setup_db(self, dvs):
        self.cfgdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        self.asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        self.threstbl = swsscommon.Table(self.cfgdb, "THRESHOLD_TABLE")                

        ## clean up all cfg, asic db entries. 
        entries = self.threstbl.getKeys()
        for key in entries:
            self.threstbl._del(key)

        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT")
        entries = tbl.getKeys()
        for key in entries:
            tbl._del(key)

        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        entries = tbl.getKeys()
        for key in entries:
            tbl._del(key)

        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT_THRESHOLD")
        entries = tbl.getKeys()
        for key in entries:
            tbl._del(key)
    
        ## Clean up syslog.
        dvs.runcmd(['sh', '-c', "echo 0 > /var/log/syslog"])

    def get_cfg_db_key(self, buffer, buffer_type, port, index):
        key = buffer + "|" + buffer_type + "|" + port + "|" + index
        return key

    def set_threshold(self, key, threshold):
        ## Set the CONFIG_DB entry.
        fvs = swsscommon.FieldValuePairs([("threshold", str(threshold))])
        self.threstbl.set(key, fvs)
        time.sleep(1)

    def remove_threshold(self, key):
        ## Remove the configured CONFIG_DB entry.
        self.threstbl._del(key)
        time.sleep(1)

    def thresAsicDbValidateTamReport(self, type):
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_REPORT")
        entries = tbl.getKeys()
        assert len(entries) == 1

        for key in entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TAM_REPORT_ATTR_TYPE":
                    assert fv[1] == type
        return True

    def thresAsicDbValidateTamTransport(self, type, sport, dport):
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TRANSPORT")
        entries = tbl.getKeys()
        assert len(entries) == 1

        for key in entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE":
                    assert fv[1] == type
                elif fv[0] == "SAI_TAM_TRANSPORT_ATTR_SRC_PORT":
                    assert fv[1] == sport
                elif fv[0] == "SAI_TAM_TRANSPORT_ATTR_DST_PORT":
                    assert fv[1] == dport
                else:
                    assert False
        return True

    def thresAsicDbValidateTamCollector(self, sip, dip):
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_COLLECTOR")
        entries = tbl.getKeys()
        assert len(entries) == 1

        ## Get transport oid.
        transtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TRANSPORT")
        transoid = transtbl.getKeys()

        for key in entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TAM_COLLECTOR_ATTR_SRC_IP":
                    assert fv[1] == sip
                elif fv[0] == "SAI_TAM_COLLECTOR_ATTR_DST_IP":
                    assert fv[1] == dip
                elif fv[0] == "SAI_TAM_COLLECTOR_ATTR_TRANSPORT":
                    assert fv[1] == transoid[0]
                elif fv[0] == "SAI_TAM_COLLECTOR_ATTR_DSCP_VALUE":
                    assert fv[1] == "0"
                else:
                    assert False
                 
        return True

    def thresAsicDbValidateTamEventAction(self):
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT_ACTION")
        entries = tbl.getKeys()
        assert len(entries) == 1

        ## Get transport oid.
        reportbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_REPORT")
        reportOid = reportbl.getKeys()

        for key in entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TAM_EVENT_ACTION_ATTR_REPORT_TYPE":
                    assert fv[1] == reportOid[0]
                else:
                    assert False
        return True

    def thresAsicDbValidateTamEvent(self, type, threshOid):
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT")
        entries = tbl.getKeys()

        ## Get action oid.
        actiontbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT_ACTION")
        actionOid = actiontbl.getKeys()
        assert len(actionOid) == 1

        collectortbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_COLLECTOR")
        collectorOid = collectortbl.getKeys()
        assert len(collectorOid) == 1


        tameventOid = ""
        for key in entries:
            eventtypefound = False
            eventactionfound = False
            collectorfound = False
            thresholdfound = False
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TAM_EVENT_ATTR_TYPE":
                    if fv[1] == type:
                        eventtypefound = True
                elif fv[0] == "SAI_TAM_EVENT_ATTR_ACTION_LIST":
                    if fv[1] == "1" + ":" + actionOid[0]:
                        eventactionfound = True
                elif fv[0] == "SAI_TAM_EVENT_ATTR_COLLECTOR_LIST":
                    if fv[1] == "1" + ":" + collectorOid[0]:
                        collectorfound = True
                elif fv[0] == "SAI_TAM_EVENT_ATTR_THRESHOLD":
                    if fv[1] == threshOid:
                        thresholdfound = True
                else:
                    assert False

            if eventtypefound  == True and eventactionfound == True and collectorfound == True and thresholdfound == True:
                    tameventOid = key

        return tameventOid

    def thresAsicDbValidateTam(self, tameventOid, bindPoint):
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        entries = tbl.getKeys()

        tamOid = ""
        for key in entries:
            eventfound = False
            bindfound = False
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TAM_ATTR_EVENT_OBJECTS_LIST":
                    if fv[1] == "1" + ":" + tameventOid:
                        eventfound = True
                elif fv[0] == "SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST":
                    if fv[1] == "1" + ":" + bindPoint:
                        bindfound = True
                else:
                    assert False

                if eventfound is True and bindfound is True:
                    tamOid = key
        return tamOid

    def thresAsicDbValidateTamEventThreshold(self, value, mode):
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT_THRESHOLD")
        entries = tbl.getKeys()

        threshoid = ""
        for key in entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TAM_EVENT_THRESHOLD_ATTR_UNIT":
                    assert fv[1] == mode
                elif fv[0] == "SAI_TAM_EVENT_THRESHOLD_ATTR_ABS_VALUE":
                    if fv[1] == value:
                        threshoid = key
                else:
                    assert False

        return threshoid

    def thresAsicDbValidateCommon(self):
        self.thresAsicDbValidateTamReport("SAI_TAM_REPORT_TYPE_PROTO")
        self.thresAsicDbValidateTamEventAction()
        self.thresAsicDbValidateTamTransport("SAI_TAM_TRANSPORT_TYPE_UDP", "7070", "9171")
        #self.thresAsicDbValidateTamCollector("10.10.10.10", "127.0.0.1")
        self.thresAsicDbValidateTamCollector("10.10.10.10", "1.0.0.127")
        
        return True

    def test_ThresEntrySetReceived(self, dvs):
        
        self.setup_db(dvs)
        marker = dvs.add_log_marker()
        ## Set log level to DEBUG.
        dvs.runcmd("swssloglevel -l DEBUG -c orchagent")

        ## Set threshold entry.
        key = self.get_cfg_db_key("priority-group", "shared", "Ethernet20", "7")
        threshold = 20
        self.set_threshold(key, threshold)

        ## Check syslog to see if SET received by orch.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Received SET command for THRESHOLD_TABLE" | wc -l'] )
        assert num.strip() >= str(1)

        # cleanup
        self.remove_threshold(key)

    def test_ThresEntryDelReceived(self, dvs): 

        self.setup_db(dvs)
        ## Set log level to DEBUG.
        dvs.runcmd("swssloglevel -l DEBUG -c orchagent")

        ## Set and delete threshold entry.
        key = self.get_cfg_db_key("priority-group", "shared", "Ethernet20", "7")
        threshold = 20
        self.set_threshold(key, threshold)
        self.remove_threshold(key)

        ## Check syslog to see if SET received by orch.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Received DEL command for THRESHOLD_TABLE" | wc -l'] )
        assert num.strip() >= str(1)
   
    def test_ThresEntryInvalidBuffer(self, dvs):

        self.setup_db(dvs)

        ## Set and delete threshold entry.
        key = self.get_cfg_db_key("buffer-pool ", "shared", "Ethernet20", "7")
        threshold = 20
        self.set_threshold(key, threshold)

        ## Check syslog to see if orch detected an error on key.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Invalid buffer buffer-pool" | wc -l'] )
        assert num.strip() == str(1)

    def test_ThresEntryInvalidBufferType(self, dvs):

        self.setup_db(dvs)

        ## Set and delete threshold entry.
        key = self.get_cfg_db_key("priority-group", "unicast", "Ethernet20", "7")
        threshold = 30
        self.set_threshold(key, threshold)

        ## Check syslog to see if orch detected an error on key.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Invalid buffer/buffer_type priority-group/unicast" | wc -l'] )
        assert num.strip() == str(1)

    def test_ThresEntryInvalidBufferType1(self, dvs):

        self.setup_db(dvs)

        ## Set and delete threshold entry.
        key = self.get_cfg_db_key("queue", "shared", "Ethernet20", "7")
        threshold = 30
        self.set_threshold(key, threshold)

        ## Check syslog to see if orch detected an error on key.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Invalid buffer/buffer_type queue/shared" | wc -l'] )
        assert num.strip() == str(1)

    def test_ThresEntryInvalidNumKeys(self, dvs):

        self.setup_db(dvs)

        ## Set and delete threshold entry.
        key = "queue" + "|" + "unicast" + "|" + "Ethernet20" +"|" + "7" + "|" + "new"
        threshold = 30
        self.set_threshold(key, threshold)

        ## Check syslog to see if orch detected an error on key.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Wrong format of table CFG_THRESHOLD_TABLE_NAME" | wc -l'] )
        assert num.strip() == str(1)

    def test_ThresEntryInvalidIndex(self, dvs):

        self.setup_db(dvs)

        ## Set and delete threshold entry.
        key = self.get_cfg_db_key("queue", "unicast", "Ethernet20", "30")
        threshold = 30
        self.set_threshold(key, threshold)

        ## Check syslog to see if orch detected an error on key.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Invalid index 30 received in key" | wc -l'] )
        assert num.strip() == str(1)

    def test_ThresEntryInvalidThreshold(self, dvs):

        self.setup_db(dvs)

        ## Set and delete threshold entry.
        key = self.get_cfg_db_key("queue", "unicast", "Ethernet20", "3")
        threshold = 0
        self.set_threshold(key, threshold)

        ## Check syslog to see if orch detected an error on key.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Invalid threshold 0 received in key" | wc -l'] )
        assert num.strip() == str(1)

    def test_ThresEntryInvalidThresholdMax(self, dvs):

        self.setup_db(dvs)

        ## Set and delete threshold entry.
        key = self.get_cfg_db_key("queue", "unicast", "Ethernet20", "3")
        threshold = 101
        self.set_threshold(key, threshold)

        ## Check syslog to see if orch detected an error on key.
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep "Invalid threshold 101 received in key" | wc -l'] )
        assert num.strip() == str(1)

    def test_ThresPgSharedConfigure(self, dvs):
        self.setup_db(dvs)

        dvs.runcmd("swssloglevel -l NOTICE -c orchagent")
        ## Set CONFIG_DB entry.
        key = self.get_cfg_db_key("priority-group", "shared", "Ethernet40", "7")
        threshold = 10
        self.set_threshold(key, threshold)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object.
        tamEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_SHARED", threshOid)
        assert tamEventOid != ""

        ## Validate Tam object
        tamOid = self.thresAsicDbValidateTam(tamEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     if fv[1] == "1" + ":" + tamOid:
                         entryfound = True
      
        assert entryfound == True
        # cleanup
        self.remove_threshold(key)
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

    def test_ThresPgHeadroomConfigure(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry.
        key = self.get_cfg_db_key("priority-group", "headroom", "Ethernet40", "7")
        threshold = 40
        self.set_threshold(key, threshold)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object.
        tamEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_XOFF_ROOM", threshOid)
        assert tamEventOid != ""

        ## Validate Tam object
        tamOid = self.thresAsicDbValidateTam(tamEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     if fv[1] == "1" + ":" + tamOid:
                         entryfound = True
      
        assert entryfound == True

        # cleanup
        self.remove_threshold(key)
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

    def test_ThresQueueUnicastConfigure(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry.
        key = self.get_cfg_db_key("queue", "unicast", "Ethernet40", "1")
        threshold = 87
        self.set_threshold(key, threshold)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object.
        tamEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_QUEUE_THRESHOLD", threshOid)
        assert tamEventOid != ""

        ## Validate Tam object
        tamOid = self.thresAsicDbValidateTam(tamEventOid, "SAI_TAM_BIND_POINT_TYPE_QUEUE")
        assert tamOid != ""

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_QUEUE_ATTR_TAM_OBJECT":
                     if fv[1] == "1" + ":" + tamOid:
                         entryfound = True
      
        assert entryfound == True

        # cleanup
        self.remove_threshold(key)
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

    def test_ThresCfgDeletePgShared(self, dvs):
        self.setup_db(dvs)

        key = self.get_cfg_db_key("priority-group", "shared", "Ethernet32", "5")
        threshold = 56
        self.set_threshold(key, threshold)

        self.remove_threshold(key)

        ## Validate TAM objects in ASIC_DB.
        ## Common objects still remain.
        assert self.thresAsicDbValidateCommon() == True

        ## No entries for the other entries.
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid == ""

        ## Check for TAM event object. 
        tameventtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT")
        entries = tameventtbl.getKeys()
        assert len(entries) == 0

        ## Check for Tam object.
        tamtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        entries = tamtbl.getKeys()
        assert len(entries) == 0

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     print(key)
                     print(fvs)
                     if fv[1] != "0:null":
                         entryfound = True

        assert entryfound == False
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

    def test_ThresCfgDeletePgHeadroom(self, dvs):
        self.setup_db(dvs)

        key = self.get_cfg_db_key("priority-group", "headroom", "Ethernet32", "5")
        threshold = 65
        self.set_threshold(key, threshold)

        self.remove_threshold(key)

        ## Validate TAM objects in ASIC_DB.
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid == ""

        ## Check for TAM event object. 
        tameventtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT")
        entries = tameventtbl.getKeys()
        assert len(entries) == 0

        ## Check for Tam object.
        tamtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        entries = tamtbl.getKeys()
        assert len(entries) == 0

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     if fv[1] != "0:null":
                         entryfound = True

        assert entryfound == False
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

    def test_ThresCfgDeleteQueueUnicast(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("queue", "unicast", "Ethernet32", "5")
        threshold = 86
        self.set_threshold(key, threshold)

        self.remove_threshold(key)

        ## Validate TAM objects in ASIC_DB.
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid == ""

        ## Check for TAM event object. 
        tameventtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT")
        entries = tameventtbl.getKeys()
        assert len(entries) == 0

        ## Check for Tam object.
        tamtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        entries = tamtbl.getKeys()
        assert len(entries) == 0

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_QUEUE_ATTR_TAM_OBJECT":
                     if fv[1] != "0:null":
                         entryfound = True

        assert entryfound == False
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)



    def test_ThresCfgDeleteSameThresholdOnOneBuffer(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("queue", "unicast", "Ethernet32", "6")
        threshold = 86
        self.set_threshold(key, threshold)

        key1 = self.get_cfg_db_key("priority-group", "shared", "Ethernet32", "5")
        self.set_threshold(key1, threshold)

        self.remove_threshold(key)

        ## Validate TAM objects in ASIC_DB.
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Check for TAM event object. 
        tamqueueEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_QUEUE_THRESHOLD", threshOid)
        assert tamqueueEventOid == ""

        ## Check for TAM event object for PG. 
        tampgEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_SHARED", threshOid)
        assert tampgEventOid != ""

        ## Validate Tam object for PG
        tampgOid = self.thresAsicDbValidateTam(tampgEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")
        assert tampgOid != ""
    
        ## Validate the bind point for queue
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        entries = bindtbl.getKeys()
    
        entryfound = False
        cnt = 0
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_QUEUE_ATTR_TAM_OBJECT":
                    assert fv[1] == "0:null"
    

        entryfound = False

        ## Validate the bind point for IPG
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                    if fv[1] == "1" + ":" + tampgOid:
                        entryfound = True
 
        assert entryfound == True
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)


    def test_ThresCfgUpdateThreshold(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("priority-group", "shared", "Ethernet40", "5")
        threshold = 56
        self.set_threshold(key, threshold)

        threshold = 78
        self.set_threshold(key, threshold)

        time.sleep(5)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate that previous threshold object is not present.
        threshOldOid = self.thresAsicDbValidateTamEventThreshold(str(56), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOldOid == ""

        ## Validate Tam event object.
        tamEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_SHARED", threshOid)
        assert tamEventOid != ""

        ## Validate Tam object
        tamOid = self.thresAsicDbValidateTam(tamEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")
        assert tamOid != ""

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     print(key)
                     print(fvs)
                     if fv[1] == "1" + ":" + tamOid:
                         entryfound = True

        assert entryfound == True
        # cleanup
        self.remove_threshold(key)

    def test_ThresCfgSameThresholdOnDiffBuffers(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("queue", "unicast", "Ethernet40", "7")
        threshold = 34
        self.set_threshold(key, threshold)

        key1 = self.get_cfg_db_key("priority-group", "shared", "Ethernet40", "6")
        self.set_threshold(key1, threshold)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object for queue.
        tamqueueEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_QUEUE_THRESHOLD", threshOid)
        assert tamqueueEventOid != ""

        ## Validate Tam event object for PG.
        tampgEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_SHARED", threshOid)
        assert tampgEventOid != ""

        ## Validate Tam object for queue
        tamqueueOid = self.thresAsicDbValidateTam(tamqueueEventOid, "SAI_TAM_BIND_POINT_TYPE_QUEUE")
        assert tamqueueOid != ""

        ## Validate Tam object for PG
        tampgOid = self.thresAsicDbValidateTam(tampgEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")
        assert tampgOid != ""

        ## Validate the bind point for queue
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_QUEUE_ATTR_TAM_OBJECT":
                     if fv[1] == "1" + ":" + tamqueueOid:
                         entryfound = True
      
        assert entryfound == True


        ## Validate bind point for PG
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     if fv[1] == "1" + ":" + tampgOid:
                         entryfound = True

        assert entryfound == True

        # cleanup
        self.remove_threshold(key)
        self.remove_threshold(key1)


    def test_ThresCfgSameThresholdOnPGBuffer(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("priority-group", "shared", "Ethernet40", "4")
        threshold = 34
        self.set_threshold(key, threshold)

        key1 = self.get_cfg_db_key("priority-group", "headroom", "Ethernet32", "6")
        self.set_threshold(key1, threshold)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object for PG shared.
        tamsharedEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_SHARED", threshOid)
        assert tamsharedEventOid != ""

        ## Validate Tam event object for PG headroom.
        tamxoffEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_XOFF_ROOM", threshOid)
        assert tamxoffEventOid != ""

        ## Validate Tam object for PG shared
        tamsharedOid = self.thresAsicDbValidateTam(tamsharedEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")
        assert tamsharedOid != ""

        ## Validate Tam object for PG headroom
        tamxoffOid = self.thresAsicDbValidateTam(tamxoffEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")
        assert tamxoffOid != ""

        ## Validate the bind point for PG shared
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entrysharedfound = False
        entryxofffound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     if fv[1] == "1" + ":" + tamsharedOid:
                         entrysharedfound = True
                     elif fv[1] == "1" + ":" + tamxoffOid:
                         entryxofffound = True
      
        entryfound = entrysharedfound and entryxofffound
        assert entryfound == True

        # cleanup
        self.remove_threshold(key)
        self.remove_threshold(key1)

    def test_ThresCfgSameThresholdOnBufferBufferType(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("priority-group", "shared", "Ethernet32", "5")
        threshold = 43
        self.set_threshold(key, threshold)

        key1 = self.get_cfg_db_key("priority-group", "shared", "Ethernet32", "6")
        self.set_threshold(key1, threshold)


        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object for PG.
        tampgEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_IPG_SHARED", threshOid)
        assert tampgEventOid != ""

        ## Validate Tam object for PG
        tampgOid = self.thresAsicDbValidateTam(tampgEventOid, "SAI_TAM_BIND_POINT_TYPE_IPG")
        assert tampgOid != ""

        ## Validate bind point for PG
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        entries = bindtbl.getKeys()

        entryfound = False
        cnt = 0
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM":
                     if fv[1] == "1" + ":" + tampgOid:
                         entryfound = True
                         cnt += 1

        assert entryfound == True
        assert cnt == 2

        # cleanup
        self.remove_threshold(key)
        self.remove_threshold(key1)

"""

    def test_ThresCfgDeleteQueueMulticast(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("queue", "multicast", "Ethernet32", "5")
        threshold = 86
        self.set_threshold(key, threshold)

        self.remove_threshold(key)
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

        ## Validate TAM objects in ASIC_DB.
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid == ""

        ## Check for TAM event object. 
        tameventtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT")
        entries = tameventtbl.getKeys()
        assert len(entries) == 0

        ## Check for Tam object.
        tamtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        entries = tamtbl.getKeys()
        assert len(entries) == 0

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_QUEUE_ATTR_TAM_OBJECT":
                     if fv[1] != "0:null":
                         entryfound = True

        assert entryfound == False
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

    def test_ThresCfgDeleteSameThreshold(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("queue", "multicast", "Ethernet32", "5")
        threshold = 86
        self.set_threshold(key, threshold)

        key1 = self.get_cfg_db_key("queue", "unicast", "Ethernet32", "5")
        self.set_threshold(key1, threshold)

        self.remove_threshold(key)
        self.remove_threshold(key1)

        ## Validate TAM objects in ASIC_DB.
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid == ""

        ## Check for TAM event object. 
        tameventtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_EVENT")
        entries = tameventtbl.getKeys()
        assert len(entries) == 0

        ## Check for Tam object.
        tamtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        entries = tamtbl.getKeys()
        assert len(entries) == 0

        ## Validate the bind point
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        entries = bindtbl.getKeys()

        entryfound = False
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_QUEUE_ATTR_TAM_OBJECT":
                     if fv[1] != "0:null":
                         entryfound = True

        assert entryfound == False
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

    def test_ThresQueueMulticastConfigure(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry.
        key = self.get_cfg_db_key("queue", "multicast", "Ethernet40", "5")
        threshold = 34
        self.set_threshold(key, threshold)

        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object.
        tamEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_QUEUE_THRESHOLD", threshOid)
        assert tamEventOid != ""

        ## Validate Tam object. Bind cannot be tested on vs since get QUEUE_TYPE
        ## is not implemented on vs.Check if TAM object is created successfully.
        tamOid = self.thresAsicDbValidateTam(tamEventOid, "SAI_TAM_BIND_POINT_TYPE_QUEUE")
        assert tamOid != ""
        (exitcode, output) = dvs.runcmd("redis-cli -n 1 keys *TAM*")
        print(output)



    def test_ThresCfgSameThresholdOnBuffer(self, dvs):
        self.setup_db(dvs)

        ## Set CONFIG_DB entry to use the same threshold value for 2 different realms..
        key = self.get_cfg_db_key("queue", "multicast", "Ethernet40", "5")
        threshold = 34
        self.set_threshold(key, threshold)

        key = self.get_cfg_db_key("queue", "unicast", "Ethernet40", "6")
        self.set_threshold(key, threshold)

        ## Validate TAM objects in ASIC_DB.
        assert self.thresAsicDbValidateCommon() == True

        ## Validate threshold object
        threshOid = self.thresAsicDbValidateTamEventThreshold(str(threshold), "SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT")
        assert threshOid != ""

        ## Validate Tam event object for queue.
        tamqueueEventOid = self.thresAsicDbValidateTamEvent("SAI_TAM_EVENT_TYPE_QUEUE_THRESHOLD", threshOid)
        assert tamqueueEventOid != ""

        ## Validate Tam object for queue
        tamqueueOid = self.thresAsicDbValidateTam(tamqueueEventOid, "SAI_TAM_BIND_POINT_TYPE_QUEUE")
        assert tamqueueOid != ""

        ## Validate the bind point for queue
        bindtbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        entries = bindtbl.getKeys()

        entryfound = False
        cnt = 0
        for key in entries:
            (status, fvs) = bindtbl.get(key)
            assert status == True
            for fv in fvs:
                 if fv[0] == "SAI_QUEUE_ATTR_TAM_OBJECT":
                     if fv[1] == "1" + ":" + tamqueueOid:
                         entryfound = True
                         cnt += 1
      
        ## Bind cannot be tested on vs for multicast queue since get QUEUE_TYPE
        ## is not implemented on vs.Check if TAM object is created successfully.

        assert entryfound == True
        assert cnt == 1
"""
