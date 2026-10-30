"""Sampled ERSPAN source binding integration tests (CONFIG_DB -> ASIC_DB)."""
import pytest

from dvslib.dvs_common import wait_for_result


@pytest.mark.usefixtures('testlog', 'dvs_mirror_manager', 'dvs_lag_manager')
class TestSampledMirror:
    @pytest.fixture(autouse=True)
    def setup(self, dvs):
        dvs.setup_db()
        self.dvs = dvs
        self.exports = []
        self.bindings = []
        self.lag_members = []
        self.lags = []
        self.route_up = False
        yield
        for key in self.bindings:
            port, direction = key.split('|')
            self.dvs_mirror.remove_sample_binding(port, direction)
        dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 0)
        for name in self.exports:
            self.dvs_mirror.remove_mirror_session(name)
            dvs.state_db.wait_for_deleted_entry('MIRROR_SESSION_TABLE', name)
        for lag, port in self.lag_members:
            self.dvs_lag.remove_port_channel_member(lag, port)
        for lag in self.lags:
            self.dvs_lag.remove_port_channel(lag)
        dvs.config_db.delete_entry('SFLOW', 'global')
        if self.route_up:
            self.remove_route()

    def add_route(self):
        dvs = self.dvs
        dvs.set_interface_status('Ethernet16', 'up')
        dvs.add_ip_address('Ethernet16', '10.0.0.0/30')
        dvs.add_neighbor('Ethernet16', '10.0.0.1', '02:04:06:08:10:12')
        dvs.add_route('2.2.2.2', '10.0.0.1')
        self.route_up = True

    def remove_route(self):
        dvs = self.dvs
        dvs.remove_route('2.2.2.2')
        dvs.remove_neighbor('Ethernet16', '10.0.0.1')
        dvs.remove_ip_address('Ethernet16', '10.0.0.0/30')
        dvs.set_interface_status('Ethernet16', 'down')
        self.route_up = False

    def export(self, name='SAMPLED_EXPORT'):
        self.dvs_mirror.create_erspan_session(name, '1.1.1.1', '2.2.2.2', '0x8949', '8', '64', '0')
        self.exports.append(name)
        return name

    def binding(self, name, port, direction='rx', rate='5000', truncate=None, admin='up'):
        key = port + '|' + direction
        self.dvs_mirror.create_sample_binding(name, port, direction, rate, truncate, admin)
        self.bindings.append(key)
        return key

    def attached(self, port, direction, rate, truncate=None):
        dvs = self.dvs
        port_oid = dvs.counters_db.get_entry('COUNTERS_PORT_NAME_MAP', '')[port]
        prefix = 'SAI_PORT_ATTR_' + ('INGRESS' if direction == 'rx' else 'EGRESS')

        def matches():
            fields = dvs.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_PORT', port_oid)
            sample = fields.get(prefix + '_SAMPLEPACKET_ENABLE', 'oid:0x0')
            mirrors = fields.get(prefix + '_SAMPLE_MIRROR_SESSION', '')
            if sample == 'oid:0x0' or not mirrors.startswith('1:'):
                return False, None
            values = dvs.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', sample)
            valid = (values.get('SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE') == rate and
                     values.get('SAI_SAMPLEPACKET_ATTR_TYPE') == 'SAI_SAMPLEPACKET_TYPE_MIRROR_SESSION')
            if truncate is not None:
                valid = (valid and values.get('SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE') == truncate and
                         values.get('SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE') == 'true')
            return valid, (sample, mirrors)

        _, result = wait_for_result(matches)
        return result

    def detached(self, port, direction='rx'):
        dvs = self.dvs
        port_oid = dvs.counters_db.get_entry('COUNTERS_PORT_NAME_MAP', '')[port]
        prefix = 'SAI_PORT_ATTR_' + ('INGRESS' if direction == 'rx' else 'EGRESS')

        def matches():
            fields = dvs.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_PORT', port_oid)
            return (fields.get(prefix + '_SAMPLEPACKET_ENABLE', 'oid:0x0') == 'oid:0x0' and
                    fields.get(prefix + '_SAMPLE_MIRROR_SESSION', '0:null') in ('0:null', '0:')), None

        wait_for_result(matches)

    def test_shared_export_and_independent_direction_policies(self):
        name = self.export()
        self.add_route()
        self.binding(name, 'Ethernet0', 'rx', '2', '64')
        self.binding(name, 'Ethernet4', 'rx', '5000', '9216')
        self.binding(name, 'Ethernet0', 'tx', '1000')
        first = self.attached('Ethernet0', 'rx', '2', '64')
        second = self.attached('Ethernet4', 'rx', '5000', '9216')
        third = self.attached('Ethernet0', 'tx', '1000')
        assert first[1] == second[1] == third[1]
        assert len({first[0], second[0], third[0]}) == 3
        self.dvs_mirror.remove_sample_binding('Ethernet0', 'rx')
        self.detached('Ethernet0')
        assert self.attached('Ethernet4', 'rx', '5000', '9216') == second
        assert self.attached('Ethernet0', 'tx', '1000') == third
        assert self.dvs.config_db.get_entry('MIRROR_SESSION', name)

    def test_disabled_binding_then_remove_and_recreate(self):
        name = self.export()
        self.add_route()
        self.binding(name, 'Ethernet0', admin='down')
        self.dvs.state_db.wait_for_field_match('MIRROR_SESSION_TABLE', name, {'status': 'active'})
        self.detached('Ethernet0')
        self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 0)
        self.dvs_mirror.remove_sample_binding('Ethernet0', 'rx')
        # Drain the deletion before reusing the same key; no in-place policy update.
        self.dvs.runcmd('sleep 2')
        self.binding(name, 'Ethernet0', rate='1000')
        self.attached('Ethernet0', 'rx', '1000')

    def test_binding_before_export_and_destination_recovery(self):
        self.binding('SAMPLED_EXPORT', 'Ethernet0')
        self.detached('Ethernet0')
        name = self.export()
        self.add_route()
        self.attached('Ethernet0', 'rx', '5000')
        self.remove_route()
        self.dvs.state_db.wait_for_field_match('MIRROR_SESSION_TABLE', name, {'status': 'inactive'})
        self.detached('Ethernet0')
        self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 0)
        self.add_route()
        self.attached('Ethernet0', 'rx', '5000')

    def test_portchannel_membership(self):
        self.dvs_lag.create_port_channel('10')
        self.lags.append('10')
        self.dvs_lag.create_port_channel_member('10', 'Ethernet0')
        self.lag_members.append(('10', 'Ethernet0'))
        self.dvs_lag.get_and_verify_port_channel_members(1)
        name = self.export()
        self.add_route()
        self.binding(name, 'PortChannel10')
        original = self.attached('Ethernet0', 'rx', '5000')
        self.dvs_lag.create_port_channel_member('10', 'Ethernet4')
        self.lag_members.append(('10', 'Ethernet4'))
        self.dvs_lag.get_and_verify_port_channel_members(2)
        assert self.attached('Ethernet4', 'rx', '5000') == original
        self.dvs_lag.remove_port_channel_member('10', 'Ethernet4')
        self.lag_members.remove(('10', 'Ethernet4'))
        self.detached('Ethernet4')
        assert self.attached('Ethernet0', 'rx', '5000') == original
        assert self.dvs.config_db.get_keys('PORT_MIRROR_SAMPLE') == ['PortChannel10|rx']

    def test_global_sflow_down_blocks_binding_until_removed(self):
        self.dvs.config_db.create_entry('SFLOW', 'global', {'admin_state': 'down'})
        name = self.export()
        self.add_route()
        self.binding(name, 'Ethernet0')
        self.dvs.state_db.wait_for_field_match('MIRROR_SESSION_TABLE', name, {'status': 'active'})
        self.dvs.runcmd('sleep 2')
        self.detached('Ethernet0')
        self.dvs.config_db.delete_entry('SFLOW', 'global')
        self.attached('Ethernet0', 'rx', '5000')
