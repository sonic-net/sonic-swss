import time
import re
import json
import pytest
import pdb
import os

from swsscommon import swsscommon


# FIXME: These tests depend on changes in sonic-buildimage, we need to reenable
# them once those changes are merged.
@pytest.mark.skip(reason="Depends on changes in sonic-buildimage")
class TestNatFeature(object):
    def setup_db(self, dvs):
        self.appdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.asicdb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.configdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def set_interfaces(self, dvs):
        intf_tbl = swsscommon.Table(self.configdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0|67.66.65.1/24", fvs)
        intf_tbl.set("Ethernet4|18.18.18.1/24", fvs)
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        dvs.runcmd("ifconfig Ethernet0 up")
        dvs.runcmd("ifconfig Ethernet4 up")

        dvs.servers[0].runcmd("ip link set down dev eth0") == 0
        dvs.servers[0].runcmd("ip link set up dev eth0") == 0
        dvs.servers[0].runcmd("ifconfig eth0 67.66.65.2/24")
        dvs.servers[0].runcmd("ip route add default via 67.66.65.1")

        dvs.servers[1].runcmd("ip link set down dev eth0") == 0
        dvs.servers[1].runcmd("ip link set up dev eth0") == 0
        dvs.servers[1].runcmd("ifconfig eth0 18.18.18.2/24")
        dvs.servers[1].runcmd("ip route add default via 18.18.18.1")

        ps = swsscommon.ProducerStateTable(self.appdb, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","18.18.18.2"), \
                                   ("ifname", "Ethernet0")])

        pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE_ROUTE_ENTRY")

        dvs.runcmd("config nat add interface Ethernet0 -nat_zone 1")

        time.sleep(1)

    def clear_interfaces(self, dvs):
        dvs.servers[0].runcmd("ifconfig eth0 0.0.0.0")

        dvs.servers[1].runcmd("ifconfig eth0 0.0.0.0")
        # dvs.servers[1].runcmd("ip route del default")

        time.sleep(1)

    def test_NatGlobalTable(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # enable NAT feature
        dvs.runcmd("config nat feature enable")
        dvs.runcmd("config nat set timeout 450")
        dvs.runcmd("config nat set udp-timeout 360")
        dvs.runcmd("config nat set tcp-timeout 900")

        # check NAT global values in appdb
        tbl = swsscommon.Table(self.appdb, "NAT_GLOBAL_TABLE")
        values = tbl.getKeys()

        assert len(values) == 1

        (status, fvs) = tbl.get("Values")

        assert fvs==(('admin_mode', 'enabled'), ('nat_timeout', '450'), ('nat_udp_timeout', '360'), ('nat_tcp_timeout', '900'))

    def test_NatInterfaceZone(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)
        self.set_interfaces(dvs)

        # check NAT zone is set for interface in app db
        tbl = swsscommon.Table(self.appdb, "INTF_TABLE")
        keys  = tbl.getKeys()

        (status, fvs) = tbl.get("Ethernet0")

        assert fvs==(('NULL', 'NULL'), ('nat_zone', '1'))


    def test_AddNatStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 18.18.18.2")

        # add a static nat entry
        dvs.runcmd("config nat add static basic 67.66.65.1 18.18.18.2")

        # check the entry in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAT")
        entry = tbl.getKeys()
        assert len(entry) == 1

        (status, fvs) = tbl.get("67.66.65.1")

        assert fvs==(('local_ip', '18.18.18.2'),)

        # check the entry in app db
        tbl = swsscommon.Table(self.appdb, "NAT_TABLE")
        entry = tbl.getKeys()
        assert len(entry) == 2

        (status, fvs) = tbl.get("67.66.65.1")

        assert fvs== (('translated_ip', '18.18.18.2'), ('nat_type', 'dnat'), ('entry_type', 'static'))

        #check the entry in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        keys = tbl.getKeys()
        assert len(keys) == 2

        for key in keys:
           if (key.find("dst_ip:67.66.65.1")) or (key.find("src_ip:18.18.18.2")):
               assert True
           else:
               assert False

    def test_DelNatStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # delete a static nat entry
        dvs.runcmd("config nat remove static basic 67.66.65.1 18.18.18.2")

        # check the entry is no there in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAT")
        entry = tbl.getKeys()
        assert entry == ()

        # check the entry is not there in app db
        tbl = swsscommon.Table(self.appdb, "NAT_TABLE")
        entry = tbl.getKeys()
        assert entry == ()

        #check the entry is not there in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        key = tbl.getKeys()
        assert key == ()

    def test_AddNaPtStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 18.18.18.2")

        # add a static nat entry
        dvs.runcmd("config nat add static udp 67.66.65.1 670 18.18.18.2 180")

        # check the entry in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAPT")
        entry = tbl.getKeys()
        assert len(entry) == 1

        (status, fvs) = tbl.get("67.66.65.1|UDP|670")

        assert fvs==(('local_ip', '18.18.18.2'),('local_port', '180'))

        # check the entry in app db
        tbl = swsscommon.Table(self.appdb, "NAPT_TABLE:UDP")
        entry = tbl.getKeys()
        assert len(entry) == 2

        (status, fvs) = tbl.get("67.66.65.1:670")

        assert fvs== (('translated_ip', '18.18.18.2'), ('translated_l4_port', '180'), ('nat_type', 'dnat'), ('entry_type', 'static')) 

        #check the entry in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        keys = tbl.getKeys()
        assert len(keys) == 2

        for key in keys:
            if (key.find("dst_ip:67.66.65.1")) and (key.find("key.l4_dst_port:670")):
                 assert True
            if (key.find("src_ip:18.18.18.2")) or (key.find("key.l4_src_port:180")):
                 assert True
            else:
                 assert False

    def test_DelNaPtStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # delete a static nat entry
        dvs.runcmd("config nat remove static udp 67.66.65.1 670 18.18.18.2 180")

        # check the entry is no there in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAPT")
        entry = tbl.getKeys()
        assert entry == ()

        # check the entry is not there in app db
        tbl = swsscommon.Table(self.appdb, "NAPT_TABLE")
        entry = tbl.getKeys()
        assert entry == ()

        #check the entry is not there in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        key = tbl.getKeys()
        assert key == ()

    def test_AddTwiceNatEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 18.18.18.2")
        dvs.servers[1].runcmd("ping -c 1 67.66.65.2")

        # add a twice nat entry
        dvs.runcmd("config nat add static basic 67.66.65.2 18.18.18.1 -nat_type snat -twice_nat_id 9")
        dvs.runcmd("config nat add static basic 67.66.65.1 18.18.18.2 -nat_type dnat -twice_nat_id 9")

        # check the entry in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAT")
        entry = tbl.getKeys()
        assert len(entry) == 2

        (status, fvs) = tbl.get("67.66.65.1")

        assert fvs== (('nat_type', 'dnat'), ('twice_nat_id', '9'), ('local_ip', '18.18.18.2'))

        (status, fvs) = tbl.get("67.66.65.2")

        assert fvs== (('nat_type', 'snat'), ('twice_nat_id', '9'), ('local_ip', '18.18.18.1'))

        # check the entry in app db
        tbl = swsscommon.Table(self.appdb, "NAT_TWICE_TABLE")
        entry = tbl.getKeys()
        assert len(entry) == 2

        (status, fvs) = tbl.get("67.66.65.2:67.66.65.1")

        assert fvs== (('translated_src_ip', '18.18.18.1'), ('translated_dst_ip', '18.18.18.2'), ('entry_type', 'static')) 

        (status, fvs) = tbl.get("18.18.18.2:18.18.18.1")

        assert fvs== (('translated_src_ip', '67.66.65.1'), ('translated_dst_ip', '67.66.65.2'), ('entry_type', 'static')) 

        #check the entry in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        keys = tbl.getKeys()
        assert len(keys) == 2
        for key in keys:
           if (key.find("dst_ip:18.18.18.1")) or (key.find("src_ip:18.18.18.2")):
               assert True
           else:
               assert False

    def test_DelTwiceNatStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # delete a static nat entry
        dvs.runcmd("config nat remove static basic 67.66.65.2 18.18.18.1")
        dvs.runcmd("config nat remove static basic 67.66.65.1 18.18.18.2")

        # check the entry is no there in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAT")
        entry = tbl.getKeys()
        assert entry == ()

        # check the entry is not there in app db
        tbl = swsscommon.Table(self.appdb, "NAT_TWICE_TABLE")
        entry = tbl.getKeys()
        assert entry == ()

        #check the entry is not there in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        key = tbl.getKeys()
        assert key == ()

    def test_AddTwiceNaPtEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 18.18.18.2")
        dvs.servers[1].runcmd("ping -c 1 67.66.65.2")

        # add a twice nat entry
        dvs.runcmd("config nat add static udp 67.66.65.2 670 18.18.18.1 181 -nat_type snat -twice_nat_id 7")
        dvs.runcmd("config nat add static udp 67.66.65.1 660 18.18.18.2 182 -nat_type dnat -twice_nat_id 7")

        # check the entry in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAPT")
        entry = tbl.getKeys()
        assert len(entry) == 2

        (status, fvs) = tbl.get("67.66.65.1|UDP|660")

        assert fvs== (('nat_type', 'dnat'), ('local_ip', '18.18.18.2'), ('twice_nat_id', '7'), ('local_port', '182'))
        (status, fvs) = tbl.get("67.66.65.2|UDP|670")

        assert fvs== (('nat_type', 'snat'), ('local_ip', '18.18.18.1'),('twice_nat_id', '7'), ('local_port', '181'))

        # check the entry in app db
        tbl = swsscommon.Table(self.appdb, "NAPT_TWICE_TABLE")
        entry = tbl.getKeys()
        assert len(entry) == 2

        (status, fvs) = tbl.get("UDP:67.66.65.2:670:67.66.65.1:660")

        assert fvs== (('translated_src_ip', '18.18.18.1'), ('translated_src_l4_port', '181'), ('translated_dst_ip', '18.18.18.2'), ('translated_dst_l4_port', '182'), ('entry_type', 'static'))

        (status, fvs) = tbl.get("UDP:18.18.18.2:182:18.18.18.1:181")

        assert fvs== (('translated_src_ip', '67.66.65.1'), ('translated_src_l4_port', '660'),('translated_dst_ip', '67.66.65.2'),('translated_dst_l4_port', '670'), ('entry_type', 'static'))

        #check the entry in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        keys = tbl.getKeys()
        assert len(keys) == 2
        for key in keys:
           if (key.find("src_ip:18.18.18.2")) or (key.find("l4_src_port:182")):
               assert True
           if (key.find("dst_ip:18.18.18.1")) or (key.find("l4_dst_port:181")):
               assert True
           else:
               assert False

    def test_DelTwiceNaPtStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # delete a static nat entry
        dvs.runcmd("config nat remove static udp 67.66.65.2 670 18.18.18.1 181")
        dvs.runcmd("config nat remove static udp 67.66.65.1 660 18.18.18.2 182")

        # check the entry is not there in the config db
        tbl = swsscommon.Table(self.configdb, "STATIC_NAPT")
        entry = tbl.getKeys()
        assert entry == ()

        # check the entry is not there in app db
        tbl = swsscommon.Table(self.appdb, "NAPT_TWICE_TABLE")
        entry = tbl.getKeys()
        assert entry == ()

        #check the entry is not there in asic db
        tbl = swsscommon.Table(self.asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY")
        key = tbl.getKeys()
        assert key == ()

        # clear interfaces
        self.clear_interfaces(dvs)
