import pytest
import time

class TestBufferModel(object):
    def test_bufferModel(self, dvs, testlog):
        config_db = dvs.get_config_db()
        metadata = config_db.get_entry("DEVICE_METADATA", "localhost")
        assert metadata["buffer_model"] == "traditional"

    def test_update_bufferModel(self, dvs, testlog):
        config_db = dvs.get_config_db()
        app_db = dvs.get_app_db()
        keys = app_db.get_keys("BUFFER_POOL_TABLE")
        num_keys =  len(keys)

        try:
            fvs = {'buffer_model' : 'dynamic'}
            config_db.update_entry("DEVICE_METADATA", "localhost", fvs)
            fvs = {'mode':'dynamic', 'type':'egress'}
            config_db.update_entry("BUFFER_POOL", "temp_pool", fvs)
            time.sleep(2)
            app_db.wait_for_n_keys("BUFFER_POOL_TABLE", num_keys)

        finally:
            config_db.delete_entry("BUFFER_POOL", "temp_pool")
            fvs = {'buffer_model' : 'traditional'}
            config_db.update_entry("DEVICE_METADATA", "localhost", fvs)
