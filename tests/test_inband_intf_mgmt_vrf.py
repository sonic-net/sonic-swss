import time
import pytest

from swsscommon import swsscommon

from dvslib.dvs_common import wait_for_result, PollingConfig

MGMT_VRF_NAME = 'mgmt'
INBAND_INTF_NAME = 'Ethernet4'

class TestInbandInterface(object):
    def setup_db(self, dvs):
        self.appl_db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.asic_db = dvs.get_asic_db()
        self.cfg_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def wait_for_table_entry(self, db, table_name, key, exists=True):
        tbl = swsscommon.Table(db, table_name)
        for _ in range(10):
            status, fvs = tbl.get(key)
            if status == exists:
                return status, fvs
            time.sleep(1)
        assert status == exists
        return status, fvs

    def wait_for_vrf_table_empty(self):
        tbl = swsscommon.Table(self.appl_db, 'VRF_TABLE')
        for _ in range(10):
            vrf_keys = tbl.getKeys()
            if len(vrf_keys) == 0:
                return
            time.sleep(1)
        assert len(vrf_keys) == 0

    def wait_for_table_empty(self, table_name):
        tbl = swsscommon.Table(self.appl_db, table_name)
        for _ in range(10):
            keys = tbl.getKeys()
            if len(keys) == 0:
                return
            time.sleep(1)
        assert len(keys) == 0

    def cleanup_mgmt_vrf(self, dvs):
        tbl = swsscommon.Table(self.cfg_db, 'MGMT_VRF_CONFIG')
        tbl._del('vrf_global')
        dvs.runcmd(["sh", "-c", "ip link show mgmt >/dev/null 2>&1 && ip link del mgmt || true"])
        self.wait_for_vrf_table_empty()

    def add_mgmt_vrf(self, dvs):
        self.cleanup_mgmt_vrf(dvs)
        initial_entries = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")) 
        dvs.runcmd("ip link add mgmt type vrf table 6000")
        dvs.runcmd("ifconfig mgmt up")
        time.sleep(2)

        # check application database 
        tbl = swsscommon.Table(self.appl_db, 'VRF_TABLE')
        vrf_keys = tbl.getKeys()
        assert len(vrf_keys) == 0

        tbl = swsscommon.Table(self.cfg_db, 'MGMT_VRF_CONFIG')
        fvs = swsscommon.FieldValuePairs([('mgmtVrfEnabled', 'true'), ('in_band_mgmt_enabled', 'true')])
        tbl.set('vrf_global', fvs)
        time.sleep(1)

        # check application database
        tbl = swsscommon.Table(self.appl_db, 'VRF_TABLE')
        vrf_keys = tbl.getKeys()
        assert len(vrf_keys) == 1
        assert vrf_keys[0] == MGMT_VRF_NAME

        # check SAI database info present in ASIC_DB 
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_entries) + 1)
        current_entries = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def del_inband_mgmt_vrf(self):
        tbl = swsscommon.Table(self.cfg_db, 'MGMT_VRF_CONFIG')
        fvs = swsscommon.FieldValuePairs([('mgmtVrfEnabled', 'true'), ('in_band_mgmt_enabled', 'false')])
        tbl.set('vrf_global', fvs)
        time.sleep(5)

        # check application database 
        self.wait_for_vrf_table_empty()

    def del_mgmt_vrf(self, dvs):
        tbl = swsscommon.Table(self.cfg_db, 'MGMT_VRF_CONFIG')
        tbl._del('vrf_global')
        dvs.runcmd(["sh", "-c", "ip link show mgmt >/dev/null 2>&1 && ip link del mgmt || true"])
        time.sleep(5)
        self.wait_for_vrf_table_empty()

    def create_inband_intf(self, interface):
        cfg_tbl = cfg_key = cfg_fvs = None
        if interface.startswith('PortChannel'):
            tbl_name = 'PORTCHANNEL_INTERFACE'
            cfg_tbl = 'PORTCHANNEL'
            cfg_key = interface
            cfg_fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                              ("mtu", "9100")])
        elif interface.startswith('Vlan'):
            tbl_name = 'VLAN_INTERFACE'
            cfg_tbl = 'VLAN'
            vlan_id = interface[len('Vlan'):]
            cfg_key = 'Vlan' + vlan_id
            cfg_fvs = swsscommon.FieldValuePairs([("vlanid", vlan_id)])
        elif interface.startswith('Loopback'):
            tbl_name = 'LOOPBACK_INTERFACE'
        else:
            tbl_name = 'INTERFACE'
        if cfg_tbl is not None:
            tbl = swsscommon.Table(self.cfg_db, cfg_tbl)
            tbl.set(cfg_key, cfg_fvs)
            time.sleep(1)
        fvs = swsscommon.FieldValuePairs([('vrf_name', MGMT_VRF_NAME)])
        tbl = swsscommon.Table(self.cfg_db, tbl_name)
        tbl.set(interface, fvs)
        time.sleep(1)

    def remove_inband_intf(self, interface):
        cfg_tbl = cfg_key = None
        if interface.startswith('PortChannel'):
            tbl_name = 'PORTCHANNEL_INTERFACE'
            cfg_tbl = 'PORTCHANNEL'
            cfg_key = interface
        elif interface.startswith('Vlan'):
            tbl_name = 'VLAN_INTERFACE'
            cfg_tbl = 'VLAN'
            vlan_id = interface[len('Vlan'):]
            cfg_key = 'Vlan' + vlan_id
        elif interface.startswith('Loopback'):
            tbl_name = 'LOOPBACK_INTERFACE'
        else:
            tbl_name = 'INTERFACE'
        tbl = swsscommon.Table(self.cfg_db, tbl_name)
        tbl._del(interface)
        time.sleep(1)
        if cfg_tbl is not None:
            tbl = swsscommon.Table(self.cfg_db, cfg_tbl)
            tbl._del(cfg_key)
            time.sleep(1)

    def test_MgmtVrf(self, dvs, testlog):
        self.setup_db(dvs)

        vrf_oid = self.add_mgmt_vrf(dvs)
        self.del_inband_mgmt_vrf()
        self.del_mgmt_vrf(dvs)

    @pytest.mark.parametrize('intf_name', ['Ethernet4', 'Vlan100', 'PortChannel5', 'Loopback1'])
    def test_InbandIntf(self, intf_name, dvs, testlog):
        self.setup_db(dvs)
        vrf_created = False
        intf_created = False

        try:
            vrf_oid = self.add_mgmt_vrf(dvs)
            vrf_created = True
            self.create_inband_intf(intf_name)
            intf_created = True

            # check application database. INTF_TABLE is populated asynchronously
            # by intfmgrd; PortChannel-backed interfaces add an extra teamd ->
            # kernel round-trip that can push the row beyond the default poll
            # window (observed in CI: ~5.1s). Poll up to 30s instead.
            tbl = swsscommon.Table(self.appl_db, 'INTF_TABLE')

            def _intf_table_ready():
                status, fvs = tbl.get(intf_name)
                return (bool(status), fvs)

            _, fvs = wait_for_result(
                _intf_table_ready,
                PollingConfig(polling_interval=1, timeout=30, strict=True),
                failure_message="INTF_TABLE entry for {} not created in time".format(intf_name),
            )
            for fv in fvs:
                if fv[0] == 'vrf_name':
                    assert fv[1] == MGMT_VRF_NAME

            if not intf_name.startswith('Loopback'):
                # check ASIC router interface database
                # one loopback router interface one port based router interface
                intf_entries = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 2)
                for key in intf_entries:
                    fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", key)
                    loopback = False
                    intf_vrf_oid = None
                    for k, v in fvs.items():
                        if k == 'SAI_ROUTER_INTERFACE_ATTR_TYPE' and v == 'SAI_ROUTER_INTERFACE_TYPE_LOOPBACK':
                            loopback = True
                            break
                        if k == 'SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID':
                            intf_vrf_oid = v
                    if loopback:
                        continue
                    assert intf_vrf_oid == vrf_oid
        finally:
            # Always tear down the mgmt VRF, even when a parametrized assertion
            # fails. Without this, a PortChannel5 failure leaks `mgmt` VRF state
            # and causes the subsequent Loopback1 parametrization to fail as a
            # cascade.
            try:
                if intf_created:
                    self.remove_inband_intf(intf_name)
                    time.sleep(1)
                    # check application database
                    self.wait_for_table_entry(self.appl_db, 'INTF_TABLE', intf_name, exists=False)
                    self.wait_for_table_empty('INTF_TABLE')

                    if not intf_name.startswith('Loopback'):
                        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

                if vrf_created:
                    self.del_inband_mgmt_vrf()
            finally:
                self.del_mgmt_vrf(dvs)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
