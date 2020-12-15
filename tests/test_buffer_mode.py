import time
import json
import redis
import pytest
import re

from pprint import pprint
from swsscommon import swsscommon

class TestBufferModel(object):
    def test_bufferModel(self, dvs, testlog):
        config_db = dvs.get_config_db()
        metadata = config_db.get_entry("DEVICE_METADATA", "localhost")
        assert metadata["buffer_model"] == "traditional"
