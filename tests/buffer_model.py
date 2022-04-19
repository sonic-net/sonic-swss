import re
import time

def initialize_lossless_parameters(config_db, cmd_runner):
    default_lossless_param = {'default_dynamic_th': '0'}
    config_db.update_entry('DEFAULT_LOSSLESS_BUFFER_PARAMETER', 'AZURE', default_lossless_param)

    lossless_traffic_pattern = {'mtu': '1024', 'small_packet_percentage': '100'}
    config_db.update_entry('LOSSLESS_TRAFFIC_PATTERN', 'AZURE', lossless_traffic_pattern)


lossless_profile_name_pattern = 'pg_lossless_([1-9][0-9]*000)_([1-9][0-9]*m)_profile'
def enable_dynamic_buffer(config_db, cmd_runner, full_init=True):
    # check whether it's already running dynamic mode
    device_meta = config_db.get_entry('DEVICE_METADATA', 'localhost')
    assert 'buffer_model' in device_meta, "'buffer_model' doesn't exist in DEVICE_METADATA|localhost"
    if device_meta['buffer_model'] == 'dynamic':
        return

    try:
        device_meta['buffer_model'] = 'dynamic'
        config_db.update_entry('DEVICE_METADATA', 'localhost', device_meta)

        # remove lossless PGs from BUFFER_PG table
        pg_keys = config_db.get_keys('BUFFER_PG')
        pgs = {}
        for key in pg_keys:
            pg = config_db.get_entry('BUFFER_PG', key)
            if re.search(lossless_profile_name_pattern, pg['profile']):
                pgs[key] = pg
                config_db.delete_entry('BUFFER_PG', key)

        # Remove the dynamically generated profiles
        profiles = config_db.get_keys('BUFFER_PROFILE')
        for key in profiles:
            if re.search(lossless_profile_name_pattern, key):
                config_db.delete_entry('BUFFER_PROFILE', key)

        # stop the buffermgrd in order to avoid it copying tables to appl db
        time.sleep(5)
        cmd_runner("supervisorctl stop buffermgrd")

        # update lossless PGs from BUFFER_PG table
        for key, pg in pgs.items():
            if re.search(lossless_profile_name_pattern, pg['profile']):
                pg['profile'] = 'NULL'
                config_db.update_entry('BUFFER_PG', key, pg)

        profiles = config_db.get_keys('BUFFER_PROFILE')
        for key in profiles:
            if re.search(lossless_profile_name_pattern, key):
                assert False, "dynamically generated profile still exists"

        if full_init:
            initialize_lossless_parameters(config_db, cmd_runner)
    finally:
        # restart daemon
        cmd_runner("supervisorctl restart buffermgrd")
        # make sure dynamic buffer manager fully starts
        time.sleep(20)


def disable_dynamic_buffer(config_db, cmd_runner):
    device_meta = config_db.get_entry('DEVICE_METADATA', 'localhost')
    assert 'buffer_model' in device_meta, "'buffer_model' doesn't exist in DEVICE_METADATA|localhost"
    if device_meta['buffer_model'] == 'traditional':
        return

    try:
        device_meta['buffer_model'] = 'traditional'
        config_db.update_entry('DEVICE_METADATA', 'localhost', device_meta)

        # Remove all the PGs referencing the lossless profiles
        pgs = config_db.get_keys('BUFFER_PG')
        for key in pgs:
            pg = config_db.get_entry('BUFFER_PG', key)
            if pg['profile'] != 'ingress_lossy_profile':
                config_db.delete_entry('BUFFER_PG', key)

        # Remove all the non-default profiles
        profiles = config_db.get_keys('BUFFER_PROFILE')
        for key in profiles:
            if re.search(lossless_profile_name_pattern, key):
                config_db.delete_entry('BUFFER_PROFILE', key)

        config_db.delete_entry('DEFAULT_LOSSLESS_BUFFER_PARAMETER', 'AZURE')
        config_db.delete_entry('LOSSLESS_TRAFFIC_PATTERN', 'AZURE')

        # wait a sufficient amount of time to make sure all tables are removed from APPL_DB
        time.sleep(20)

    finally:
        # restart daemon
        cmd_runner("supervisorctl restart buffermgrd")

        time.sleep(20)
