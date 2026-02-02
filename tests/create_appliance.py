#!/usr/bin/python3

"""
    Connect to Dash orch with ZMQ and send create appliance request.
    usage:
        python3 create_appliance.py [appliance ID]
    Example:
        python3 create_appliance.py 1234
"""

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

# connect to Dash ZMQ endpoint
db_connection = swsscommon.DBConnector("DPU_APPL_DB", 0)
zmq_client = swsscommon.ZmqClient("tcp://127.0.0.1:8100", 1000)
app_dash_appliance_table = swsscommon.ZmqProducerStateTable(
                db_connection,
                "DASH_APPLIANCE_TABLE",
                zmq_client,
                True)

# prepare create appliance request
pairs_str = []
pb = Appliance()
pb.sip.ipv4 = socket.htonl(int(ipaddress.ip_address("10.0.0.1")))
pb.vm_vni = int(sys.argv[1])
pairs_str.append(("pb", pb.SerializeToString()))

# send create appliance request via ZMQ
app_dash_appliance_table.set("100", pairs_str)
