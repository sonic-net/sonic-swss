import time

class TestNat(object):
    def setup_db(self, dvs):
        self.app_db = dvs.get_app_db()
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()

    def set_interfaces(self, dvs):
        fvs = {"NULL": "NULL"}
        self.config_db.create_entry("INTERFACE", "Ethernet0|67.66.65.1/24", fvs)
        self.config_db.create_entry("INTERFACE", "Ethernet4|18.18.18.1/24", fvs)
        self.config_db.create_entry("INTERFACE", "Ethernet0", fvs)
        self.config_db.create_entry("INTERFACE", "Ethernet4", fvs)
        dvs.runcmd("config interface startup Ethernet0")
        dvs.runcmd("config interface startup Ethernet4")

        dvs.servers[0].runcmd("ip link set down dev eth0")
        dvs.servers[0].runcmd("ip link set up dev eth0")
        dvs.servers[0].runcmd("ifconfig eth0 67.66.65.2/24")
        dvs.servers[0].runcmd("ip route add default via 67.66.65.1")

        dvs.servers[1].runcmd("ip link set down dev eth0")
        dvs.servers[1].runcmd("ip link set up dev eth0")
        dvs.servers[1].runcmd("ifconfig eth0 18.18.18.2/24")
        dvs.servers[1].runcmd("ip route add default via 18.18.18.1")

        dvs.runcmd("config nat add interface Ethernet0 -nat_zone 1")

        time.sleep(1)

    def clear_interfaces(self, dvs):
        dvs.servers[0].runcmd("ifconfig eth0 0.0.0.0")
        dvs.servers[1].runcmd("ifconfig eth0 0.0.0.0")

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
        self.app_db.wait_for_n_keys("NAT_GLOBAL_TABLE", 1)

        fvs = self.app_db.wait_for_entry("NAT_GLOBAL_TABLE", "Values")

        assert fvs == {"admin_mode": "enabled", "nat_timeout": "450", "nat_udp_timeout": "360", "nat_tcp_timeout": "900"}

    def test_NatInterfaceZone(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)
        self.set_interfaces(dvs)

        # check NAT zone is set for interface in app db
        fvs = self.app_db.wait_for_entry("INTF_TABLE", "Ethernet0")
        zone = False
        for f, v in fvs.items():
            if f == "nat_zone" and v == '1':
                zone = True
                break
        assert zone

    def test_AddNatStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 18.18.18.2")

        # add a static nat entry
        dvs.runcmd("config nat add static basic 67.66.65.1 18.18.18.2")

        # check the entry in the config db
        self.config_db.wait_for_n_keys("STATIC_NAT", 1)

        fvs = self.config_db.wait_for_entry("STATIC_NAT", "67.66.65.1")

        assert fvs == {"local_ip": "18.18.18.2"}

        # check the entry in app db
        self.app_db.wait_for_n_keys("NAT_TABLE", 2)

        fvs = self.app_db.wait_for_entry("NAT_TABLE", "67.66.65.1")

        assert fvs == {
            "translated_ip": "18.18.18.2",
            "nat_type": "dnat",
            "entry_type": "static"
        }

        #check the entry in asic db
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 2)

        for key in keys:
            assert "\"dst_ip\":\"67.66.65.1\"" in key or "\"src_ip\":\"18.18.18.2\"" in key

    def test_DelNatStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # delete a static nat entry
        dvs.runcmd("config nat remove static basic 67.66.65.1 18.18.18.2")

        # check the entry is no there in the config db
        self.config_db.wait_for_n_keys("STATIC_NAT", 0)

        # check the entry is not there in app db
        self.app_db.wait_for_n_keys("NAT_TABLE", 0)

        #check the entry is not there in asic db
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 0)

    def test_AddNaPtStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 18.18.18.2")

        # add a static nat entry
        dvs.runcmd("config nat add static udp 67.66.65.1 670 18.18.18.2 180")

        # check the entry in the config db
        self.config_db.wait_for_n_keys("STATIC_NAPT", 1)

        fvs = self.config_db.wait_for_entry("STATIC_NAPT", "67.66.65.1|UDP|670")

        assert fvs == {"local_ip": "18.18.18.2", "local_port": "180"}

        # check the entry in app db
        self.app_db.wait_for_n_keys("NAPT_TABLE:UDP", 2)

        fvs = self.app_db.wait_for_entry("NAPT_TABLE:UDP", "67.66.65.1:670")

        assert fvs == {"translated_ip": "18.18.18.2", "translated_l4_port": "180", "nat_type": "dnat", "entry_type": "static"}

        #check the entry in asic db
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 2)
        for key in keys:
            if "\"dst_ip\":\"67.66.65.1\"" in key and "\"l4_dst_port\":\"670\"" in key:
                assert True
            elif "\"src_ip\":\"18.18.18.2\"" in key or "\"l4_src_port\":\"180\"" in key:
                assert True
            else:
                assert False

    def test_DelNaPtStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # delete a static nat entry
        dvs.runcmd("config nat remove static udp 67.66.65.1 670 18.18.18.2 180")

        # check the entry is no there in the config db
        self.config_db.wait_for_n_keys("STATIC_NAPT", 0)

        # check the entry is not there in app db
        self.app_db.wait_for_n_keys("NAPT_TABLE:UDP", 0)

        #check the entry is not there in asic db
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 0)

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
        self.config_db.wait_for_n_keys("STATIC_NAT", 2)

        fvs = self.config_db.wait_for_entry("STATIC_NAT", "67.66.65.1")
        assert fvs == {"nat_type": "dnat", "twice_nat_id": "9", "local_ip": "18.18.18.2"}

        fvs = self.config_db.wait_for_entry("STATIC_NAT", "67.66.65.2")
        assert fvs == {"nat_type": "snat", "twice_nat_id": "9", "local_ip": "18.18.18.1"}

        # check the entry in app db
        self.app_db.wait_for_n_keys("NAT_TWICE_TABLE", 2)

        fvs = self.app_db.wait_for_entry("NAT_TWICE_TABLE", "67.66.65.2:67.66.65.1")
        assert fvs == {"translated_src_ip": "18.18.18.1", "translated_dst_ip": "18.18.18.2", "entry_type": "static"}

        fvs = self.app_db.wait_for_entry("NAT_TWICE_TABLE", "18.18.18.2:18.18.18.1")
        assert fvs == {"translated_src_ip": "67.66.65.1", "translated_dst_ip": "67.66.65.2", "entry_type": "static"}

        #check the entry in asic db
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 2)
        for key in keys:
            assert "\"dst_ip\":\"67.66.65.1\"" in key or "\"src_ip\":\"18.18.18.2\"" in key

    def test_DelTwiceNatStaticEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # delete a static nat entry
        dvs.runcmd("config nat remove static basic 67.66.65.2 18.18.18.1")
        dvs.runcmd("config nat remove static basic 67.66.65.1 18.18.18.2")

        # check the entry is no there in the config db
        self.config_db.wait_for_n_keys("STATIC_NAT", 0)

        # check the entry is not there in app db
        self.app_db.wait_for_n_keys("NAT_TWICE_TABLE", 0)

        #check the entry is not there in asic db
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 0)

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
        self.config_db.wait_for_n_keys("STATIC_NAPT", 2)

        fvs = self.config_db.wait_for_entry("STATIC_NAPT", "67.66.65.1|UDP|660")
        assert fvs == {"nat_type": "dnat", "local_ip": "18.18.18.2", "twice_nat_id": "7", "local_port": "182"}

        fvs = self.config_db.wait_for_entry("STATIC_NAPT", "67.66.65.2|UDP|670")
        assert fvs == {"nat_type": "snat", "local_ip": "18.18.18.1", "twice_nat_id": "7", "local_port": "181"}

        # check the entry in app db
        self.app_db.wait_for_n_keys("NAPT_TWICE_TABLE", 2)

        fvs = self.app_db.wait_for_entry("NAPT_TWICE_TABLE", "UDP:67.66.65.2:670:67.66.65.1:660")
        assert fvs == {"translated_src_ip": "18.18.18.1", "translated_src_l4_port": "181", "translated_dst_ip": "18.18.18.2", "translated_dst_l4_port": "182", "entry_type": "static"}

        fvs = self.app_db.wait_for_entry("NAPT_TWICE_TABLE", "UDP:18.18.18.2:182:18.18.18.1:181")
        assert fvs == {"translated_src_ip": "67.66.65.1", "translated_src_l4_port": "660", "translated_dst_ip": "67.66.65.2", "translated_dst_l4_port": "670", "entry_type": "static"}

        #check the entry in asic db
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 2)
        for key in keys:
            if "\"src_ip\":\"18.18.18.2\"" in key or "\"l4_src_port\":\"182\"" in key:
                assert True
            elif "\"dst_ip\":\"67.66.65.1\"" in key or "\"l4_dst_port\":\"660\"" in key:
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
        self.config_db.wait_for_n_keys("STATIC_NAPT", 0)

        # check the entry is not there in app db
        self.app_db.wait_for_n_keys("NAPT_TWICE_TABLE", 0)

        #check the entry is not there in asic db
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NAT_ENTRY", 0)

        # clear interfaces
        self.clear_interfaces(dvs)

    def test_VerifyConntrackTimeoutForNatEntry(self, dvs, testlog):
        # initialize
        self.setup_db(dvs)

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 18.18.18.2")

        # add a static nat entry
        dvs.runcmd("config nat add static basic 67.66.65.1 18.18.18.2")

        # check the conntrack timeout for static entry
        output = dvs.runcmd("conntrack -j -L -s 18.18.18.2 -p udp -q 67.66.65.1")
        assert len(output) == 2

        conntrack_list = list(output[1].split(" "))

        src_exists = dst_exists = proto_exists = False
        proto_index = 0

        for i in conntrack_list:
            if i == "src=18.18.18.2":
                src_exists = True
            elif i == "dst=67.66.65.1":
                dst_exists = True
            elif i == "udp":
                proto_exists = True
                proto_index = conntrack_list.index(i)

        assert src_exists == True
        assert dst_exists == True
        assert proto_exists == True

        if conntrack_list[proto_index + 7] < 432000 and conntrack_list[proto_index + 7] > 431900:
            assert False

        # delete a static nat entry
        dvs.runcmd("config nat remove static basic 67.66.65.1 18.18.18.2")

