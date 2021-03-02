import time
import json
import pytest
import pdb

from swsscommon import swsscommon

def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)

def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def delete_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    time.sleep(1)

def create_evpn_nvo(dvs, nvoname, tnlname):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
        ("source_vtep", tnlname),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_EVPN_NVO", '|', nvoname,
        attrs,
    )

def remove_evpn_nvo(dvs, nvoname):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    delete_entry_tbl(conf_db, "VXLAN_EVPN_NVO", nvoname, )

def set_admin_status(dvs, interface, status):
    tbl = swsscommon.Table(dvs.cdb, "PORT")
    fvs = swsscommon.FieldValuePairs([("admin_status", status)])
    tbl.set(interface, fvs)
    time.sleep(1)
    
tunnel_name = 'vxlan'
tunnel_vlan = 'Vlan20'
tunnel_device = 'vxlan-20'
tunnel_vlan_id = '20'
tunnel_vni = '10020'
tunnel_src_ip = '12.1.1.1'
tunnel_remote_ip = '10.0.0.7'
tunnel_remote_fdb = '00:11:01:00:00:01'
tunnel_remote_fdb2 = '12:34:55:12:34:56'
tunnel_remote_fdb_type = 'dynamic'
tunnel_remote_fdb_type_static = 'static'
state_db_name = 'FDB_TABLE'
local_intf = 'Ethernet8'
local_intf2 = 'Ethernet12'
tunnel_name_nvo = 'nvo'
app_fdb_name = 'VXLAN_FDB_TABLE:'
tunnel_remote_imet = '00:00:00:00:00:00'
tunnel_remote_imet_type = 'permanent'
app_imet_name = 'VXLAN_REMOTE_VNI_TABLE:'

class TestFdbsync(object):
    

    def test_AddLocalFDB(self, dvs, testlog):
        dvs.setup_db()
        create_evpn_nvo(dvs, tunnel_name_nvo , tunnel_name)

        set_admin_status(dvs, local_intf, "up")

        # create vlan; create vlan member
        dvs.create_vlan(tunnel_vlan_id)
        dvs.create_vlan_member(tunnel_vlan_id, local_intf)

        create_entry_tbl(
            dvs.sdb,
            state_db_name, '|', tunnel_vlan + ':' + tunnel_remote_fdb,
            [
                ("port", local_intf),
                ("type", tunnel_remote_fdb_type),
            ]
        )
        # check that the FDB entries were inserted into State DB
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, state_db_name,
                        tunnel_vlan + ":.*",
                        [("port", local_intf),
                         ("type", tunnel_remote_fdb_type),
                        ]
        )
        assert ok, str(extra)

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb + " | wc -l"])
        num = int(output.strip())
        assert num == 1

        delete_entry_tbl(
            dvs.sdb,
            state_db_name, tunnel_vlan + ':' + tunnel_remote_fdb,
        )

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb])
        assert output == ''
        remove_evpn_nvo(dvs, tunnel_name_nvo)

    def test_AddLocalFDBWithOutNVO(self, dvs, testlog):
        dvs.setup_db()
        set_admin_status(dvs, local_intf, "up")

        # create vlan; create vlan member
        dvs.create_vlan(tunnel_vlan_id)
        dvs.create_vlan_member(tunnel_vlan_id, local_intf)

        create_entry_tbl(
            dvs.sdb,
            state_db_name, '|', tunnel_vlan + ':' + tunnel_remote_fdb,
            [
                ("port", local_intf),
                ("type", tunnel_remote_fdb_type),
            ]
        )

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb ])
        assert output == ''

        delete_entry_tbl(
            dvs.sdb,
            state_db_name, tunnel_vlan + ':' + tunnel_remote_fdb,
        )

    def test_AddLocalFDBPortUpdate(self, dvs, testlog):
        dvs.setup_db()
        create_evpn_nvo(dvs, tunnel_name_nvo, tunnel_name)

        set_admin_status(dvs, local_intf, "up")
        set_admin_status(dvs, local_intf2, "up")

        # create vlan; create vlan member
        dvs.create_vlan(tunnel_vlan_id)
        dvs.create_vlan_member(tunnel_vlan_id, local_intf)
        dvs.create_vlan_member(tunnel_vlan_id, local_intf2)

        #pdb.set_trace()

        create_entry_tbl(
            dvs.sdb,
            state_db_name, '|', tunnel_vlan + ':' + tunnel_remote_fdb,
            [
                ("port", local_intf),
                ("type", tunnel_remote_fdb_type),
            ]
        )

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb + " | wc -l"])
        num = int(output.strip())
        assert num == 1

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb])
        assert local_intf in output

        create_entry_tbl(
            dvs.sdb,
            state_db_name, '|', tunnel_vlan + ':' + tunnel_remote_fdb,
            [
                ("port", local_intf2),
                ("type", tunnel_remote_fdb_type),
            ]
        )
        # check that the FDB entries were inserted into State DB
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, state_db_name,
                        tunnel_vlan + ":.*",
                        [("port", local_intf2),
                         ("type", tunnel_remote_fdb_type),
                        ]
        )
        assert ok, str(extra)

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb + " | wc -l"])
        num = int(output.strip())
        assert num == 1

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb])
        assert local_intf2 in output


        delete_entry_tbl(
            dvs.sdb,
            state_db_name, tunnel_vlan + ':' + tunnel_remote_fdb,
        )

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb])
        assert output == ''
        remove_evpn_nvo(dvs, tunnel_name_nvo)

    def test_AddLocalStaticFDB(self, dvs, testlog):
        dvs.setup_db()
        create_evpn_nvo(dvs, tunnel_name_nvo, tunnel_name)

        set_admin_status(dvs, local_intf, "up")

        # create vlan; create vlan member
        dvs.create_vlan(tunnel_vlan_id)
        dvs.create_vlan_member(tunnel_vlan_id, local_intf)

        create_entry_tbl(
            dvs.sdb,
            state_db_name, '|', tunnel_vlan + ':' + tunnel_remote_fdb,
            [
                ("port", local_intf),
                ("type", tunnel_remote_fdb_type_static),
            ]
        )

        # check that the FDB entries were inserted into State DB
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, state_db_name,
                                              tunnel_vlan + ":.*",
                                              [("port", local_intf),
                                               ("type", tunnel_remote_fdb_type_static),
                                              ]
        )
        assert ok, str(extra)

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb + " | wc -l"])
        num = int(output.strip())
        assert num == 1

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb])
        assert tunnel_remote_fdb_type_static in output

        delete_entry_tbl(
            dvs.sdb,
            state_db_name, tunnel_vlan + ':' + tunnel_remote_fdb,
        )

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb])
        assert output == ''
        remove_evpn_nvo(dvs, tunnel_name_nvo)

    def test_AddVxlanFDB(self, dvs, testlog):
        dvs.setup_db()
        create_evpn_nvo(dvs, tunnel_name_nvo, tunnel_name)

        dvs.runcmd("ip link add {} type vxlan id {} local {}".format(tunnel_device, tunnel_vni, tunnel_src_ip))
        dvs.runcmd("ip link set up {}".format(tunnel_device))
        dvs.runcmd("bridge fdb add {} dev {} dst {} self {}".format(tunnel_remote_fdb2, tunnel_device, tunnel_remote_ip, tunnel_remote_fdb_type_static))

        # Check in the APP DB for the FDB entry to be present APP_VXLAN_FDB_TABLE_NAME "APP_VXLAN_FDB_TABLE_NAME"
        # check application database
        tbl = swsscommon.Table(dvs.pdb, app_fdb_name+tunnel_vlan)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == tunnel_remote_fdb2
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "remote_vtep":
                assert fv[1] == tunnel_remote_ip
            elif fv[0] == "type":
                assert fv[1] == tunnel_remote_fdb_type_static
            elif fv[0] == "vni":
                assert fv[1] == tunnel_vni
            else:
                assert False

        # Remove the fdb entry, and check the APP_DB
        dvs.runcmd("bridge fdb del {} dev {} dst {} self {}".format(tunnel_remote_fdb2, tunnel_device, tunnel_remote_ip, tunnel_remote_fdb_type_static))
        ebl = swsscommon.Table(dvs.pdb, app_fdb_name+tunnel_vlan)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

    def test_AddVxlanIMET(self, dvs, testlog):

        dvs.setup_db()
        create_evpn_nvo(dvs, tunnel_name_nvo, tunnel_name)

        dvs.runcmd("ip link add {} type vxlan id {} local {}".format(tunnel_device, tunnel_vni, tunnel_src_ip))
        dvs.runcmd("ip link set up {}".format(tunnel_device))
        dvs.runcmd("bridge fdb add {} dev {} dst {} self {}".format(tunnel_remote_imet, tunnel_device, tunnel_remote_ip, tunnel_remote_imet_type))

        # Check in the APP DB for the FDB entry to be present APP_VXLAN_FDB_TABLE_NAME "APP_VXLAN_FDB_TABLE_NAME"
        # check application database
        tbl = swsscommon.Table(dvs.pdb, app_imet_name+tunnel_vlan)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == tunnel_remote_ip 
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        assert len(fvs) == 1
        for fv in fvs:
            if fv[0] == "vni":
                assert fv[1] == tunnel_vni
            else:
                assert False

        # Remove the fdb entry, and check the APP_DB
        dvs.runcmd("bridge fdb del {} dev {} dst {} self {}".format(tunnel_remote_imet, tunnel_device, tunnel_remote_ip, tunnel_remote_imet_type))
        tbl = swsscommon.Table(dvs.pdb, app_imet_name+tunnel_vlan)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

    def test_VxlanFDBToLocal(self, dvs, testlog):
        dvs.setup_db()
        create_evpn_nvo(dvs, tunnel_name_nvo, tunnel_name)

        dvs.runcmd("ip link add {} type vxlan id {} local {}".format(tunnel_device, tunnel_vni, tunnel_src_ip))
        dvs.runcmd("ip link set up {}".format(tunnel_device))
        dvs.runcmd("bridge fdb add {} dev {} dst {} self {}".format(tunnel_remote_fdb2, tunnel_device, tunnel_remote_ip, tunnel_remote_fdb_type))

        # Check in the APP DB for the FDB entry to be present APP_VXLAN_FDB_TABLE_NAME "APP_VXLAN_FDB_TABLE_NAME"
        # check application database
        tbl = swsscommon.Table(dvs.pdb, app_fdb_name+tunnel_vlan)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == tunnel_remote_fdb2
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "remote_vtep":
                assert fv[1] == tunnel_remote_ip
            elif fv[0] == "type":
                assert fv[1] == tunnel_remote_fdb_type
            elif fv[0] == "vni":
                assert fv[1] == tunnel_vni
            else:
                assert False

        create_evpn_nvo(dvs, tunnel_name_nvo, tunnel_name)

        set_admin_status(dvs, local_intf, "up")

        # create vlan; create vlan member
        dvs.create_vlan(tunnel_vlan_id)
        dvs.create_vlan_member(tunnel_vlan_id, local_intf)

        create_entry_tbl(
            dvs.sdb,
            state_db_name, '|', tunnel_vlan + ':' + tunnel_remote_fdb2,
            [
                ("port", local_intf),
                ("type", tunnel_remote_fdb_type),
            ]
        )
        # check that the FDB entries were inserted into State DB
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, state_db_name,
                        tunnel_vlan + ":.*",
                        [("port", local_intf),
                         ("type", tunnel_remote_fdb_type),
                        ]
        )
        assert ok, str(extra)

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb2 + " | grep Ethernet8 | wc -l"])
        num = int(output.strip())
        assert num == 1

        delete_entry_tbl(
            dvs.sdb,
            state_db_name, tunnel_vlan + ':' + tunnel_remote_fdb2,
        )

    def test_LocalFDBToVxlan(self, dvs, testlog):
        dvs.setup_db()
        create_evpn_nvo(dvs, tunnel_name_nvo, tunnel_name)

        set_admin_status(dvs, local_intf, "up")

        # create vlan; create vlan member
        dvs.create_vlan(tunnel_vlan_id)
        dvs.create_vlan_member(tunnel_vlan_id, local_intf)

        create_entry_tbl(
            dvs.sdb,
            state_db_name, '|', tunnel_vlan + ':' + tunnel_remote_fdb,
            [
                ("port", local_intf),
                ("type", tunnel_remote_fdb_type),
            ]
        )
        # check that the FDB entries were inserted into State DB
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, state_db_name,
                        tunnel_vlan + ":.*",
                        [("port", local_intf),
                         ("type", tunnel_remote_fdb_type),
                        ]
        )
        assert ok, str(extra)

        (exitcode, output) = dvs.runcmd(['sh', '-c', "bridge fdb show  | grep " + tunnel_remote_fdb + " | wc -l"])
        num = int(output.strip())
        assert num == 1

        #pdb.set_trace()
        dvs.runcmd("ip link add {} type vxlan id {} local {}".format(tunnel_device, tunnel_vni, tunnel_src_ip))
        dvs.runcmd("ip link set up {}".format(tunnel_device))
        dvs.runcmd("bridge fdb add {} dev {} dst {} self {}".format(tunnel_remote_fdb, tunnel_device, tunnel_remote_ip, tunnel_remote_fdb_type))

        # Check in the APP DB for the FDB entry to be present APP_VXLAN_FDB_TABLE_NAME "APP_VXLAN_FDB_TABLE_NAME"
        # check application database
        tbl = swsscommon.Table(dvs.pdb, app_fdb_name+tunnel_vlan)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == tunnel_remote_fdb
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "remote_vtep":
                assert fv[1] == tunnel_remote_ip
            elif fv[0] == "type":
                assert fv[1] == tunnel_remote_fdb_type
            elif fv[0] == "vni":
                assert fv[1] == tunnel_vni
            else:
                assert False

