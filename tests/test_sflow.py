from swsscommon import swsscommon

import time
import os

class TestSflow(object):
    def setup_sflow(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        ptbl = swsscommon.ProducerStateTable(self.pdb, "SFLOW_TABLE")
        fvs = swsscommon.FieldValuePairs([("admin_state", "enable")])
        ptbl.set("global", fvs)

        time.sleep(1)

    def test_SflowDisble(self, dvs, testlog):
        self.setup_sflow(dvs)
        ptbl = swsscommon.ProducerStateTable(self.pdb, "SFLOW_SESSION_TABLE")
        gtbl = swsscommon.ProducerStateTable(self.pdb, "SFLOW_TABLE")
        fvs = swsscommon.FieldValuePairs([("admin_state", "disable")])
        gtbl.set("global", fvs)

        time.sleep(1)
        fvs = swsscommon.FieldValuePairs([("admin_state", "enable"),("sample_rate","1000")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)


        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session == ""

        fvs = swsscommon.FieldValuePairs([("admin_state", "enable")])
        gtbl.set("global", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session != "oid:0x0"
        assert sample_session != ""

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        (status, fvs) = atbl.get(sample_session)
        
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE":
                assert fv[1] == "1000"

        ptbl._del("Ethernet0")

    def test_InterfaceSet(self, dvs, testlog):
        self.setup_sflow(dvs)
        ptbl = swsscommon.ProducerStateTable(self.pdb, "SFLOW_SESSION_TABLE")
        gtbl = swsscommon.ProducerStateTable(self.pdb, "SFLOW_TABLE")
        fvs = swsscommon.FieldValuePairs([("admin_state", "enable"),("sample_rate","1000")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session != ""

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        (status, fvs) = atbl.get(sample_session)
        
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE":
                assert fv[1] == "1000"

        ptbl._del("Ethernet0")

    def test_ConfigDel(self, dvs, testlog):
        self.setup_sflow(dvs)
        ptbl = swsscommon.ProducerStateTable(self.pdb, "SFLOW_SESSION_TABLE")
        fvs = swsscommon.FieldValuePairs([("admin_state", "enable"),("sample_rate","1000")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)

        ptbl._del("Ethernet0")

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        speed = ""

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                speed = fv[1]

        assert speed != ""
        assert sample_session == "oid:0x0"
