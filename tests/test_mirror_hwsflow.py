"""HWSFLOW mirror session integration tests (CONFIG_DB -> ASIC_DB)."""
import pytest

from dvslib.dvs_common import wait_for_result


@pytest.mark.usefixtures('testlog', 'dvs_mirror_manager')
class TestHwsflowMirror:
    @pytest.fixture(autouse=True)
    def setup(self, dvs):
        dvs.setup_db()
        self.dvs = dvs
        self.sessions = []
        self.bindings = []
        self.route_up = False
        yield
        for key in self.bindings:
            port, direction = key.split('|')
            self.dvs_mirror.remove_sample_binding(port, direction)
        dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 0)
        for name in self.sessions:
            self.dvs_mirror.remove_mirror_session(name)
            dvs.state_db.wait_for_deleted_entry('MIRROR_SESSION_TABLE', name)
        dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', 0)
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

    def session(self, name='HWSFLOW_SESSION', sample_rate='50000', **kwargs):
        self.dvs_mirror.create_hwsflow_session(name, '1.1.1.1', '2.2.2.2', '8', '64',
                                               sample_rate=sample_rate, **kwargs)
        self.sessions.append(name)
        if not self.route_up:
            self.add_route()
        self.dvs.state_db.wait_for_field_match('MIRROR_SESSION_TABLE', name, {'status': 'active'})
        return name

    def binding(self, name, port, direction='rx', rate=None, admin='up'):
        key = port + '|' + direction
        self.dvs_mirror.create_sample_binding(name, port, direction, rate, admin_state=admin)
        self.bindings.append(key)
        return key

    def port_oid(self, port):
        return self.dvs.counters_db.get_entry('COUNTERS_PORT_NAME_MAP', '')[port]

    def port_attached(self, port, direction='rx'):
        dvs = self.dvs
        port_oid = self.port_oid(port)
        prefix = 'SAI_PORT_ATTR_' + ('INGRESS' if direction == 'rx' else 'EGRESS')

        def matches():
            fields = dvs.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_PORT', port_oid)
            sample = fields.get(prefix + '_SAMPLEPACKET_ENABLE', 'oid:0x0')
            mirrors = fields.get(prefix + '_SAMPLE_MIRROR_SESSION', '')
            if sample == 'oid:0x0' or not mirrors.startswith('1:'):
                return False, None
            values = dvs.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', sample)
            return values.get('SAI_SAMPLEPACKET_ATTR_TYPE') == 'SAI_SAMPLEPACKET_TYPE_MIRROR_SESSION', (values, mirrors[2:])

        _, result = wait_for_result(matches)
        return result

    def port_detached(self, port, direction='rx'):
        dvs = self.dvs
        port_oid = self.port_oid(port)
        prefix = 'SAI_PORT_ATTR_' + ('INGRESS' if direction == 'rx' else 'EGRESS')

        def matches():
            fields = dvs.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_PORT', port_oid)
            return (fields.get(prefix + '_SAMPLEPACKET_ENABLE', 'oid:0x0') == 'oid:0x0' and
                    fields.get(prefix + '_SAMPLE_MIRROR_SESSION', '0:null') in ('0:null', '0:')), None

        wait_for_result(matches)

    def test_capability_in_state_db(self):
        cap = self.dvs.state_db.wait_for_entry('SWITCH_CAPABILITY', 'switch')
        assert cap.get('MIRROR_SESSION_HWSFLOW_CAPABLE') == 'true'
        for field in ('MIRROR_SESSION_SAMPLE_RATE_CAPABLE',
                      'MIRROR_SESSION_TRUNCATION_CAPABLE',
                      'LAG_INGRESS_SAMPLE_MIRROR_CAPABLE',
                      'LAG_EGRESS_SAMPLE_MIRROR_CAPABLE'):
            assert cap.get(field) in ('true', 'false')

    def test_session_create_and_port_source(self):
        name = self.session()
        pmap = self.dvs.counters_db.get_entry('COUNTERS_PORT_NAME_MAP', '')
        src_mac = self.dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
        keys = self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', 1)
        assert self.dvs.asic_db.wait_for_entry('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', keys[0]) == {
            'SAI_MIRROR_SESSION_ATTR_MONITOR_PORT': pmap['Ethernet16'],
            'SAI_MIRROR_SESSION_ATTR_TYPE': 'SAI_MIRROR_SESSION_TYPE_SFLOW',
            'SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION': '4',
            'SAI_MIRROR_SESSION_ATTR_TOS': '32',
            'SAI_MIRROR_SESSION_ATTR_TTL': '64',
            'SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS': '1.1.1.1',
            'SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS': '2.2.2.2',
            'SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS': src_mac,
            'SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS': '02:04:06:08:10:12',
            'SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT': '6343',
            'SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT': '6343',
            'SAI_MIRROR_SESSION_ATTR_SAMPLE_RATE': '50000',
            'SAI_MIRROR_SESSION_ATTR_TRUNCATE_SIZE': '128',
        }

        self.binding(name, 'Ethernet0')
        sample, mirror = self.port_attached('Ethernet0')
        assert sample['SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE'] == '50000'
        assert mirror == keys[0]

        self.dvs_mirror.remove_sample_binding('Ethernet0', 'rx')
        self.bindings.remove('Ethernet0|rx')
        self.port_detached('Ethernet0')
        self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 0)
        self.dvs.state_db.wait_for_field_match('MIRROR_SESSION_TABLE', name, {'status': 'active'})

    def test_truncate_on_session_only(self):
        name = self.session(truncate_size='256')
        keys = self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', 1)
        entry = self.dvs.asic_db.wait_for_entry('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', keys[0])
        assert entry['SAI_MIRROR_SESSION_ATTR_TRUNCATE_SIZE'] == '256'

        self.binding(name, 'Ethernet0')
        sample, _ = self.port_attached('Ethernet0')
        assert 'SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE' not in sample
        assert 'SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE' not in sample

    def test_udp_ports(self):
        self.session(udp_src_port='6400', udp_dst_port='6401')
        keys = self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', 1)
        entry = self.dvs.asic_db.wait_for_entry('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', keys[0])
        assert entry['SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT'] == '6400'
        assert entry['SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT'] == '6401'

    def test_two_sessions_different_rates(self):
        first = self.session('HWSFLOW_A', sample_rate='1000')
        second = self.session('HWSFLOW_B', sample_rate='5000')
        keys = self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', 2)
        rates = {k: self.dvs.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', k)
                 ['SAI_MIRROR_SESSION_ATTR_SAMPLE_RATE'] for k in keys}
        assert sorted(rates.values()) == ['1000', '5000']

        self.binding(first, 'Ethernet0')
        self.binding(second, 'Ethernet4')
        _, mirror0 = self.port_attached('Ethernet0')
        _, mirror4 = self.port_attached('Ethernet4')
        assert rates[mirror0] == '1000'
        assert rates[mirror4] == '5000'

    def test_binding_admin_state(self):
        name = self.session()
        key = self.binding(name, 'Ethernet0', admin='down')
        self.port_detached('Ethernet0')
        self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 0)
        self.dvs.config_db.update_entry('PORT_MIRROR_SAMPLE', key, {'admin_state': 'up'})
        self.port_attached('Ethernet0')
        self.dvs.config_db.update_entry('PORT_MIRROR_SAMPLE', key, {'admin_state': 'down'})
        self.port_detached('Ethernet0')
        self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 0)
        self.dvs.state_db.wait_for_field_match('MIRROR_SESSION_TABLE', name, {'status': 'active'})

    def test_binding_with_rate_rejected(self):
        name = self.session()
        # MirrorOrch handles keys in order, so once the later binding attaches the earlier one was processed.
        self.binding(name, 'Ethernet0', rate='5000')
        self.binding(name, 'Ethernet4')
        self.port_attached('Ethernet4')
        self.port_detached('Ethernet0')
        self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET', 1)

    @pytest.mark.parametrize('bad_fields', [
        {'type': 'HWSFLOW'},
        {'type': 'HWSFLOW', 'sample_rate': '1'},
        {'type': 'HWSFLOW', 'sample_rate': '50000', 'gre_type': '0x8949'},
        {'type': 'HWSFLOW', 'sample_rate': '50000', 'queue': '0'},
        {'type': 'ERSPAN', 'gre_type': '0x8949', 'queue': '0', 'sample_rate': '50000'},
    ], ids=['no_sample_rate', 'sample_rate_1', 'gre_type', 'queue', 'erspan_with_sample_rate'])
    def test_invalid_session_rejected(self, bad_fields):
        entry = {'src_ip': '1.1.1.1', 'dst_ip': '2.2.2.2', 'dscp': '8', 'ttl': '64'}
        entry.update(bad_fields)
        self.dvs.config_db.create_entry('MIRROR_SESSION', 'HWSFLOW_BAD', entry)
        self.sessions.append('HWSFLOW_BAD')
        # MirrorOrch handles keys in order, so once the later valid session shows
        # up in STATE_DB the invalid one has already been processed.
        self.session('HWSFLOW_OK')
        assert 'HWSFLOW_BAD' not in self.dvs.state_db.get_keys('MIRROR_SESSION_TABLE')
        self.dvs.asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION', 1)
