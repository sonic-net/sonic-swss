import pytest
import time
from swsscommon import swsscommon

class TestBufferManager(object):
    def make_dict(self, input_list):
        return dict(input_list[1])

    def setup_db(self, dvs):
        self.config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.buffer_pg_table = swsscommon.Table(self.config_db, "BUFFER_PG")
        self.port_table = swsscommon.Table(self.config_db, "PORT")
        cable_length_table = swsscommon.Table(self.config_db, "CABLE_LENGTH")
        cable_length_key = cable_length_table.getKeys()[0]
        self.cable_lengths = self.make_dict(cable_length_table.get(cable_length_key))
        self.buffer_profile_table = swsscommon.Table(self.config_db, "BUFFER_PROFILE")

    def load_pg_profile_lookup(self, dvs):
        self.profile_lookup_info = {}
        lines = dvs.runcmd('cat /usr/share/sonic/hwsku/pg_profile_lookup.ini')[1].split('\n')

        SPEED = 0
        CABLE_LENGTH = 1
        SIZE = 2
        XON = 3
        XOFF = 4
        THRESHOLD = 5
        XON_OFFSET = 6

        for line in lines:
            if len(line) == 0 or line[0] == '#':
                continue
            tokens = line.split()
            self.profile_lookup_info[(tokens[SPEED], tokens[CABLE_LENGTH])] = {
                'size': tokens[SIZE],
                'xon': tokens[XON],
                'xoff': tokens[XOFF],
                'dynamic_th': tokens[THRESHOLD]
            }
            if XON_OFFSET < len(tokens):
                self.profile_lookup_info[(tokens[SPEED], tokens[CABLE_LENGTH])]['xon_offset'] = tokens[XON_OFFSET]

    def test_buffer_pg(self, dvs):
        self.setup_db(dvs)

        port = 'Ethernet0'
        pg = port + '|3-4'

        port_info = self.make_dict(self.port_table.get(port))
        if 'up' == port_info.get('admin_status'):
            # By default, all ports should be admin down on VM.
            # However, in case the port under test was admin up before the test, we just shut down it.
            dvs.runcmd('config interface shutdown {}'.format(port))
            time.sleep(1)

        # Make sure no lossless PG exists on an admin down port
        assert not self.buffer_pg_table.get(pg)[0]

        try:
            # Startup the port. The lossless PG should be created according to speed and cable length
            dvs.runcmd('config interface startup {}'.format(port))

            cable_length = self.cable_lengths.get(port)
            speed = port_info.get('speed')

            expected_profile_name = 'pg_lossless_{}_{}_profile'.format(speed, cable_length)
            buffer_profile_info = self.make_dict(self.buffer_profile_table.get(expected_profile_name))
            buffer_pg_info = self.make_dict(self.buffer_pg_table.get(pg))
            assert buffer_pg_info['profile'] == '[BUFFER_PROFILE|{}]'.format(expected_profile_name)

            self.load_pg_profile_lookup(dvs)
            expected_profile_info = self.profile_lookup_info[(speed, cable_length)]
            expected_profile_info['pool'] = '[BUFFER_POOL|ingress_lossless_pool]'
            assert buffer_profile_info == expected_profile_info
        finally:
            # Shutdown the port. The lossless PG should be removed
            dvs.runcmd('config interface shutdown {}'.format(port))
            time.sleep(1)
            assert not self.buffer_pg_table.get(pg)[0]
