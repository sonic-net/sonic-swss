import time
import os

from swsscommon import swsscommon
from flaky import flaky


@pytest.mark.flaky
class TestSflow(object):
    speed_rate_table = {
        "400000":"40000",
        "100000":"10000",
        "50000":"5000",
        "40000":"4000",
        "25000":"2500",
        "10000":"1000",
        "1000":"100"
    }
    def setup_sflow(self, dvs):
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        ctbl = swsscommon.Table(self.cdb, "SFLOW")

        fvs = swsscommon.FieldValuePairs([("admin_state", "up")])
        ctbl.set("global", fvs)

        time.sleep(1)

    def test_defaultGlobal(self, dvs, testlog):
        self.setup_sflow(dvs)
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

        assert sample_session != ""
        assert speed != ""

        rate = ""

        if speed in self.speed_rate_table:
            rate = self.speed_rate_table[speed]

        assert rate != ""

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        (status, fvs) = atbl.get(sample_session)

        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE":
                assert fv[1] == rate

        ctbl = swsscommon.Table(self.cdb, "SFLOW")
        fvs = swsscommon.FieldValuePairs([("admin_state", "down")])
        ctbl.set("global", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        speed = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session == "oid:0x0"

    def test_globalAll(self, dvs, testlog):
        self.setup_sflow(dvs)

        ctbl = swsscommon.Table(self.cdb, "SFLOW_SESSION")
        fvs = swsscommon.FieldValuePairs([("admin_state", "down")])
        ctbl.set("all", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        speed = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session == "oid:0x0"

        fvs = swsscommon.FieldValuePairs([("admin_state", "up")])
        ctbl.set("all", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session != ""
        assert sample_session != "oid:0x0"

        ctbl._del("all")

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session != ""
        assert sample_session != "oid:0x0"


    def test_InterfaceSet(self, dvs, testlog):
        self.setup_sflow(dvs)
        ctbl = swsscommon.Table(self.cdb, "SFLOW_SESSION")
        gtbl = swsscommon.Table(self.cdb, "SFLOW")
        fvs = swsscommon.FieldValuePairs([("admin_state", "up"),("sample_rate","1000")])
        ctbl.set("Ethernet0", fvs)

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

        fvs = swsscommon.FieldValuePairs([("admin_state", "down")])
        ctbl.set("all", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]
        assert sample_session != ""
        assert sample_session != "oid:0x0"

        fvs = swsscommon.FieldValuePairs([("admin_state", "down")])
        gtbl.set("global", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])

        assert status == True

        sample_session = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]

        assert sample_session == "oid:0x0"
        ctbl._del("all")
        ctbl._del("Ethernet0")

    def test_defaultRate(self, dvs, testlog):
        self.setup_sflow(dvs)
        ctbl = swsscommon.Table(self.cdb, "SFLOW_SESSION")
        fvs = swsscommon.FieldValuePairs([("admin_state", "up")])
        ctbl.set("Ethernet4", fvs)

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet4"])

        assert status == True

        sample_session = ""
        speed = ""
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE":
                sample_session = fv[1]
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                speed = fv[1]

        assert sample_session != ""
        assert speed != ""

        rate = ""

        if speed in self.speed_rate_table:
            rate = self.speed_rate_table[speed]

        assert rate != ""

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        (status, fvs) = atbl.get(sample_session)

        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE":
                assert fv[1] == rate

        ctbl._del("Ethernet4")

    def test_ConfigDel(self, dvs, testlog):
        self.setup_sflow(dvs)
        ctbl = swsscommon.Table(self.cdb, "SFLOW_SESSION_TABLE")
        fvs = swsscommon.FieldValuePairs([("admin_state", "up"),("sample_rate","1000")])
        ctbl.set("Ethernet0", fvs)

        time.sleep(1)

        ctbl._del("Ethernet0")

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
        assert sample_session != ""
        assert sample_session != "oid:0x0"

        rate = ""

        if speed in self.speed_rate_table:
            rate = self.speed_rate_table[speed]

        assert rate != ""

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        (status, fvs) = atbl.get(sample_session)

        assert status == True

        rf = False
        for fv in fvs:
            if fv[0] == "SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE":
                assert fv[1] == rate
                rf = True

        assert rf == True

    def test_Teardown(self, dvs, testlog):
        self.setup_sflow(dvs)
        ctbl = swsscommon.Table(self.cdb, "SFLOW")
        ctbl._del("global")

        time.sleep(1)


        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        assert len(atbl.getKeys()) == 0
