"""Kernel pure-L3 peer MAC pins must reach ASIC and preserve local ownership."""
import json

import pytest
from dvslib.dvs_common import wait_for_result


PORTCHANNEL = "PortChannel331"
VLAN = "Vlan3101"
MAC = "02:31:00:00:00:01"
KEY = VLAN + ":" + MAC
APP_TABLE = "VXLAN_FDB_TABLE"
ASIC_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY"


def run(dvs, command):
    rc, output = dvs.runcmd(command)
    assert rc == 0, (command, output)
    return output


def kernel_pin(dvs, operation, local=False):
    # Use the kernel route socket, not APP_DB seeding: fdbsyncd must receive
    # the real bridge notification, including Zebra protocol and local master.
    script = '''import socket, struct, sys
po, mac, operation, local = sys.argv[1:]
index = socket.if_nametoindex(po)
with open('/sys/class/net/' + po + '/master/ifindex') as f:
    master = int(f.read())
def attr(kind, value):
    data = struct.pack('=HH', len(value) + 4, kind) + value
    return data + b'\\0' * ((-len(data)) % 4)
body = struct.pack('=BBHiHBB', 7, 0, 0, index, 2 if local == 'True' else 64, 4, 0)
body += attr(2, bytes.fromhex(mac.replace(':', '')))
body += attr(5, struct.pack('=H', 3101))
body += attr(9, struct.pack('=I', master))
body += attr(12, struct.pack('=B', 11))
kind, flags = (28, 1 | 4 | 0x100 | 0x400) if operation == 'add' else (29, 1 | 4)
message = struct.pack('=IHHII', len(body) + 16, kind, flags, 1, 0) + body
s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE)
s.settimeout(5)
s.bind((0, 0))
s.sendto(message, (0, 0))
reply = s.recv(65535)
assert struct.unpack_from('=H', reply, 4)[0] == 2, reply
assert struct.unpack_from('=i', reply, 16)[0] == 0, reply
if operation == 'add':
    # Mainline bridge kernels may ACK but discard NDA_PROTOCOL. Without it
    # this host cannot reproduce the FRR notification consumed by fdbsyncd.
    dump = struct.pack('=BBHiHBB', 7, 0, 0, 0, 0, 0, 0)
    s.sendto(struct.pack('=IHHII', len(dump) + 16, 30, 1 | 0x300, 2, 0) + dump, (0, 0))
    found = []
    done = False
    while not done:
        data = s.recv(65535)
        offset = 0
        while offset + 16 <= len(data):
            length, kind, flags, sequence, pid = struct.unpack_from('=IHHII', data, offset)
            assert length >= 16
            if kind == 3:
                done = True
                break
            if kind == 28:
                row = data[offset + 16:offset + length]
                position = 12
                attributes = {}
                while position + 4 <= len(row):
                    size, tag = struct.unpack_from('=HH', row, position)
                    assert size >= 4 and position + size <= len(row)
                    attributes[tag] = row[position + 4:position + size]
                    position += (size + 3) & ~3
                if (attributes.get(2) == bytes.fromhex(mac.replace(':', '')) and
                        struct.unpack_from('=i', row, 4)[0] == index):
                    found.append(attributes)
            offset += (length + 3) & ~3
    assert found, 'Kernel did not retain the requested bridge FDB entry'
    if any(attributes.get(12) != bytes([11]) for attributes in found):
        print('KERNEL_FDB_ZEBRA_PROTOCOL_UNAVAILABLE')
        sys.exit(77)
'''
    command = ["python3", "-c", script, PORTCHANNEL, MAC, operation, str(local)]
    rc, output = dvs.runcmd(command)
    if rc == 77 and "KERNEL_FDB_ZEBRA_PROTOCOL_UNAVAILABLE" in output:
        pytest.skip("Kernel bridge does not preserve NDA_PROTOCOL=Zebra; requires a compatible SONiC kernel")
    assert rc == 0, output


def hardware_entry(dvs):
    db = dvs.get_asic_db()
    entries = [db.get_entry(ASIC_TABLE, key) for key in db.get_keys(ASIC_TABLE)
               if json.loads(key).get("mac", "").lower() == MAC]
    assert len(entries) <= 1, entries
    return entries[0] if entries else {}


@pytest.fixture
def peer_pin_portchannel(dvs):
    config = dvs.get_config_db()
    previous = config.get_entry("FDB_SYNC", "global")
    assert not config.get_entry("PORTCHANNEL", PORTCHANNEL)
    assert not config.get_entry("VLAN", VLAN)
    config.create_entry("FDB_SYNC", "global", {"mac_sync_mode": "kernel"})
    run(dvs, ["supervisorctl", "restart", "fpmsyncd"])
    dvs.get_state_db().wait_for_field_match("FDB_SYNC", "global", {"mac_sync_mode": "kernel"})
    run(dvs, ["config", "portchannel", "add", PORTCHANNEL])
    run(dvs, ["config", "portchannel", "member", "add", PORTCHANNEL, "Ethernet0"])
    run(dvs, ["config", "vlan", "add", "3101"])
    run(dvs, ["config", "vlan", "member", "add", "3101", PORTCHANNEL])
    wait_for_result(lambda: (dvs.runcmd(["test", "-e", "/sys/class/net/" + PORTCHANNEL + "/master"])[0] == 0, None))
    try:
        yield dvs
    finally:
        # The kernel entry may already have been withdrawn by the test.
        dvs.runcmd(["bridge", "fdb", "del", MAC, "dev", PORTCHANNEL, "master", "vlan", "3101"])
        run(dvs, ["config", "vlan", "member", "del", "3101", PORTCHANNEL])
        wait_for_result(lambda: (not hardware_entry(dvs), None))
        # Standard DVS table deletion avoids the host-only ndppd Docker hook.
        config.delete_entry("VLAN", VLAN)
        run(dvs, ["config", "portchannel", "member", "del", PORTCHANNEL, "Ethernet0"])
        run(dvs, ["config", "portchannel", "del", PORTCHANNEL])
        if previous:
            config.set_entry("FDB_SYNC", "global", previous)
        else:
            config.delete_entry("FDB_SYNC", "global")
        run(dvs, ["supervisorctl", "restart", "fpmsyncd"])


@pytest.mark.parametrize("local_handoff", [False, True])
def test_kernel_peer_pin_restart_and_withdrawal(peer_pin_portchannel, local_handoff):
    dvs = peer_pin_portchannel
    app = dvs.get_app_db()
    state = dvs.get_state_db()
    kernel_pin(dvs, "add")
    app.wait_for_field_match(APP_TABLE, KEY, {
        "ifname": PORTCHANNEL, "vni": "0", "type": "static",
        "kernel_l3vni_sync": "true", "l3vni_neigh_sync": "peer",
    })
    wait_for_result(lambda: (hardware_entry(dvs).get("SAI_FDB_ENTRY_ATTR_TYPE") == "SAI_FDB_ENTRY_TYPE_STATIC", None))
    before = hardware_entry(dvs)
    assert "SAI_FDB_ENTRY_ATTR_ENDPOINT_IP" not in before
    assert not state.get_entry("FDB_TABLE", KEY)

    run(dvs, ["supervisorctl", "restart", "fdbsyncd"])
    app.wait_for_field_match(APP_TABLE, KEY, {"kernel_l3vni_sync": "true", "type": "static"})
    assert hardware_entry(dvs) == before

    if local_handoff:
        kernel_pin(dvs, "add", local=True)
        state.wait_for_field_match("FDB_TABLE", KEY, {"port": PORTCHANNEL, "type": "dynamic"})
        app.wait_for_deleted_entry(APP_TABLE, KEY)
        wait_for_result(lambda: (
            hardware_entry(dvs).get("SAI_FDB_ENTRY_ATTR_TYPE") == "SAI_FDB_ENTRY_TYPE_DYNAMIC", None))
        kernel_pin(dvs, "delete", local=True)
        assert hardware_entry(dvs)["SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID"] == before["SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID"]
        assert state.get_entry("FDB_TABLE", KEY)["type"] == "dynamic"
    else:
        kernel_pin(dvs, "delete")
        app.wait_for_deleted_entry(APP_TABLE, KEY)
        wait_for_result(lambda: (not hardware_entry(dvs), None))
