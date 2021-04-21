import pytest

class TestBufferModel(object):
    def test_bufferModel(self, dvs, testlog):
        config_db = dvs.get_config_db()
        metadata = config_db.get_entry("DEVICE_METADATA", "localhost")
        assert metadata["buffer_model"] == "traditional"
