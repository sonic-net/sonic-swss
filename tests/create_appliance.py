#!/usr/bin/python3

from swsscommon import swsscommon
from dash_api.appliance_pb2 import *
import typing
import ipaddress
import socket
import sys

def to_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    elif isinstance(value, bytes):
        return value
    return str(value)

class ZmqProduceStateTable(object):
    def __init__(self, table_name: str):
        self.db_connection = swsscommon.DBConnector("APPL_DB", 0)
        self.zmq_client = swsscommon.ZmqClient("tcp://127.0.0.1:8100")
        self.zmq_table = swsscommon.ZmqProducerStateTable(
            self.db_connection,
            table_name,
            self.zmq_client,
            True)
    def __setitem__(self, key: str, pairs: typing.Union[dict, list, tuple]):
        pairs_str = []
        if isinstance(pairs, dict):
            pairs = pairs.items()
        for k, v in pairs:
            pairs_str.append((to_string(k), to_string(v)))
        self.zmq_table.set(key, pairs_str)
    def __delitem__(self, key: str):
        self.zmq_table.delete(str(key))

vni = sys.argv[1]
app_dash_appliance_table = ZmqProduceStateTable("DASH_APPLIANCE_TABLE")
pb = Appliance()
pb.sip.ipv4 = socket.htonl(int(ipaddress.ip_address("10.0.0.1")))
pb.vm_vni = int(vni)
app_dash_appliance_table["100"] = {"pb": pb.SerializeToString()}
