import pytest
from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
from dvslib.dvs_common import PollingConfig, wait_for_result

DVS_ENV = ["HWSKU=brcm_gearbox_breakout_vs"]


class GearboxBreakout(object):
    def __init__(self, dvs):
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        t = swsscommon.Table(db, "_GEARBOX_TABLE")
        assert len(t.getKeys()) > 0
        sr = t.getTableNameSeparator()

        self.phys = {}
        phy_table = swsscommon.Table(db, sr.join([t.getKeyName(""), "phy"]))
        for i in [x for x in phy_table.getKeys() if sr not in x]:
            (status, fvs) = phy_table.get(i)
            assert status == True
            self.phys[i] = {"attrs": dict(fvs)}

            port_table = swsscommon.Table(db, sr.join([phy_table.getKeyName(i), "ports"]))
            port_list = [x for x in port_table.getKeys() if sr not in x]
            self.phys[i]["port_table"] = port_table
            self.phys[i]["ports"] = {}
            for j in port_list:
                (status, fvs) = port_table.get(j)
                assert status == True
                self.phys[i]["ports"][j] = dict(fvs)

            lane_table = swsscommon.Table(db, sr.join([phy_table.getKeyName(i), "lanes"]))
            lane_list = [x for x in lane_table.getKeys() if sr not in x]
            self.phys[i]["lanes"] = {}
            for j in lane_list:
                (status, fvs) = lane_table.get(j)
                assert status == True
                self.phys[i]["lanes"][j] = dict(fvs)

        self.interfaces = {}
        intf_table = swsscommon.Table(db, sr.join([t.getKeyName(""), "interface"]))
        for i in [x for x in intf_table.getKeys() if sr not in x]:
            (status, fvs) = intf_table.get(i)
            assert status == True
            self.interfaces[i] = dict(fvs)


@pytest.fixture(scope="module")
def gearbox(dvs):
    return GearboxBreakout(dvs)


class TestGearboxBreakout(object):
    def test_AllInterfacesPresent(self, gearbox, testlog):
        """All three interfaces must be present despite two sharing an index."""
        assert len(gearbox.interfaces) == 3

    def test_InterfaceKeysAreAliases(self, gearbox, testlog):
        """Interface DB keys must match their alias field."""
        for key, fvs in gearbox.interfaces.items():
            assert "alias" in fvs
            assert key == fvs["alias"]

    def test_BreakoutSharedIndexDistinctKeys(self, gearbox, testlog):
        """Two interfaces sharing an index must have distinct DB keys."""
        index_to_keys = {}
        for key, fvs in gearbox.interfaces.items():
            idx = fvs.get("index")
            index_to_keys.setdefault(idx, []).append(key)

        shared = {idx: keys for idx, keys in index_to_keys.items() if len(keys) > 1}
        assert len(shared) > 0, "Expected at least one shared index for breakout test"
        for idx, keys in shared.items():
            assert len(keys) == len(set(keys)), \
                "Duplicate DB keys for index {}: {}".format(idx, keys)

    def test_PhyPortKeysAreAliases(self, gearbox, testlog):
        """Phy port DB keys must match their alias field."""
        for phy_id, phy in gearbox.phys.items():
            for port_key, port_fvs in phy["ports"].items():
                assert "alias" in port_fvs
                assert port_key == port_fvs["alias"]

    def test_PhyPortBreakoutDistinctKeys(self, gearbox, testlog):
        """Two phy ports sharing an index must have distinct DB keys."""
        for phy_id, phy in gearbox.phys.items():
            index_to_keys = {}
            for port_key, port_fvs in phy["ports"].items():
                idx = port_fvs.get("index")
                index_to_keys.setdefault(idx, []).append(port_key)

            shared = {idx: keys for idx, keys in index_to_keys.items() if len(keys) > 1}
            assert len(shared) > 0, \
                "Expected at least one shared port index for breakout test in phy {}".format(phy_id)
            for idx, keys in shared.items():
                assert len(keys) == len(set(keys)), \
                    "Duplicate port DB keys for index {} in phy {}: {}".format(idx, phy_id, keys)

    def test_SanityCheckWithAlias(self, gearbox, testlog):
        """Each interface's alias must exist as a key in the phy port table."""
        for intf_key, intf_fvs in gearbox.interfaces.items():
            phy_id = intf_fvs["phy_id"]
            assert phy_id in gearbox.phys

            alias = intf_fvs["alias"]
            assert alias in gearbox.phys[phy_id]["ports"], \
                "Interface alias '{}' not found in phy {} ports".format(alias, phy_id)

            for lane in intf_fvs["system_lanes"].split(','):
                assert lane in gearbox.phys[phy_id]["lanes"]
            for lane in intf_fvs["line_lanes"].split(','):
                assert lane in gearbox.phys[phy_id]["lanes"]


def test_nonflaky_dummy():
    pass
