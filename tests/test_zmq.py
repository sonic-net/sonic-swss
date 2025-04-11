from swsscommon import swsscommon

from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *

import typing
import time
import binascii
import uuid
import ipaddress
import sys
import socket
import logging
import pytest

logging.basicConfig(level=logging.INFO)
zmq_logger = logging.getLogger(__name__)

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

class Table(object):
    def __init__(self, database, table_name: str):
        self.table_name = table_name
        self.table = swsscommon.Table(database.db_connection, self.table_name)

    def __getitem__(self, key: str):
        exists, result = self.table.get(str(key))
        if not exists:
            return None
        else:
            return dict(result)

    def get_keys(self):
        return self.table.getKeys()

    def get_newly_created_oid(self, old_oids):
        new_oids = self.asic_db.wait_for_n_keys(table, len(old_oids) + 1)
        oid = [ids for ids in new_oids if ids not in old_oids]
        return oid[0]

class DashZmq(object):
    def __init__(self, dvs):
        self.dvs = dvs
        self.asic_direction_lookup_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_DIRECTION_LOOKUP_ENTRY")
        self.asic_vip_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_VIP_ENTRY")

class TestZmqDash(object):
    @pytest.fixture(scope="class")
    def enable_orchagent_zmq(self, dvs):
        # change orchagent to use ZMQ
        # change orchagent to use custom create_switch_timeout
        dvs.runcmd("cp /usr/bin/orchagent.sh /usr/bin/orchagent.sh_zmq_ut_backup")
        dvs.runcmd("sed -i.bak 's/\/usr\/bin\/orchagent /\/usr\/bin\/orchagent -q tcp:\/\/127.0.0.1:8100 -t 60 /g' /usr/bin/orchagent.sh")
        dvs.stop_swss()
        dvs.start_swss()

        process_statue = dvs.runcmd("ps -ef")
        zmq_logger.debug("Process status: {}".format(process_statue))

        yield

        # revert change
        dvs.runcmd("cp /usr/bin/orchagent.sh_zmq_ut_backup /usr/bin/orchagent.sh")
        dvs.stop_swss()
        dvs.start_swss()

    @pytest.mark.usefixtures("enable_orchagent_zmq")
    def test_appliance(self, dvs):
        # upload test script to test container and create applicance with it
        dvs.copy_file("/", "create_appliance.py")
        dvs.runcmd(['sh', '-c', "python3 create_appliance.py {}".format(1234)])
        time.sleep(3)

        asic_direction_lookup_table = Table(
            dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_DIRECTION_LOOKUP_ENTRY")
        direction_entries = asic_direction_lookup_table.get_keys()
        zmq_logger.info("Keys from asic_direction_lookup_table: {}".format(direction_entries))

        assert direction_entries
        fvs = asic_direction_lookup_table[direction_entries[0]]
        zmq_logger.info("Data from asic_direction_lookup_table: {}={}".format(direction_entries[0], fvs))
        for fv in fvs.items():
            if fv[0] == "SAI_DIRECTION_LOOKUP_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_DIRECTION_LOOKUP_ENTRY_ACTION_SET_OUTBOUND_DIRECTION"

        asic_vip_table = Table(
            dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_VIP_ENTRY")
        vip_entries = asic_vip_table.get_keys()
        zmq_logger.info("Keys from asic_vip_table: {}".format(direction_entries))

        assert vip_entries
        fvs = asic_vip_table[vip_entries[0]]
        zmq_logger.info("Data from asic_vip_table: {}={}".format(vip_entries[0], fvs))
        for fv in fvs.items():
            if fv[0] == "SAI_VIP_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_VIP_ENTRY_ACTION_ACCEPT"

    def test_vrf(self, dvs):
        # Improve test code coverage, change orchagent to use VRF
        dvs.runcmd("cp /usr/bin/orchagent.sh /usr/bin/orchagent.sh_vrf_ut_backup")
        dvs.runcmd("sed -i.bak 's/\/usr\/bin\/orchagent /\/usr\/bin\/orchagent -v mgmt /g' /usr/bin/orchagent.sh")
        dvs.stop_swss()
        dvs.start_swss()

        # wait orchagent start
        time.sleep(3)
        process_statue = dvs.runcmd("ps -ef")
        zmq_logger.debug("Process status: {}".format(process_statue))

        # revert change
        dvs.runcmd("cp /usr/bin/orchagent.sh_vrf_ut_backup /usr/bin/orchagent.sh")
        dvs.stop_swss()
        dvs.start_swss()
