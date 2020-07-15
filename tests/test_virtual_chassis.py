import time
import json
import redis
import pytest
from swsscommon import swsscommon

class TestVirtualChassis(object):
    def check_chassis_sysport(self, dvs):
        # check if instance is connected to a chassis db
        cfgFile = "/usr/share/sonic/virtual_chassis/default_config.json"
        (ret, out) = dvs.runcmd("sonic-cfggen -v  DEVICE_METADATA.localhost.chassis_db_address -j %s" % cfgFile)
        verifyChassDb = True
        if ret != 0 or out.strip() == "":
            # instance has no connection to chassis db
            verifyChassDb = False
            print "verify vs without chassisDb"
        (ret, out) = dvs.runcmd("sonic-db-cli CHASSIS_DB keys SYSTEM_PORT\"*\"")
        if verifyChassDb:
            assert len( out.split() ) == 96
        else:
            assert 'Invalid database name' in out
        (ret, out) = dvs.runcmd("sonic-db-cli CHASSIS_DB HGETALL 'SYSTEM_PORT|Linecard3|Ethernet0'")
        out = out.replace("u'", "'")
        if verifyChassDb:
            assert "'system_port_id': '65'" in out
            assert "'switch_id': '2'" in out
        else:
            assert 'Invalid database name' in out

    def test_chassis_sysport(self, dvs, testlog):
        dvss = {}
        if dvs.vct is not None:
            dvss = dvs.vct.dvss
        else:
            dvss[dvs.ctn.name] = dvs
        for dv in dvss.values():
            print "checking %s" % dv.ctn.name
            self.check_chassis_sysport( dv )

    def test_connectivity(self, dvs, testlog):
        if dvs.vct is None:
            return
        vct = dvs.vct
        dvss = {}
        dvss = vct.dvss
        ns = vct.ns
        nbrs = {}
        nbrs[ "lc1." + ns ] = { "supervisor." + ns : "10.8.1.200",
                                "lc2." + ns : "10.8.1.2",
                                "lc3." + ns : "10.8.1.3",
                                "R1." + ns : "10.8.101.2",
                                "R4." + ns : "10.8.104.2" }
        nbrs[ "lc2." + ns ] = { "supervisor." + ns : "10.8.1.200",
                                "lc1." + ns : "10.8.1.1",
                                "lc3." + ns : "10.8.1.3",
                                "R2." + ns : "10.8.102.2" }
        nbrs[ "lc3" + ns ] = { "supervisor." + ns : "10.8.1.200",
                               "lc1." + ns : "10.8.1.1",
                               "lc2." + ns : "10.8.1.2",
                               "R3." + ns : "10.8.103.2" }
        for name in dvss.keys():
            if name not in nbrs:
                continue
            for nbr, ip in nbrs[ name ].iteritems():
                if nbr not in dvss.keys():
                    continue
                dv = dvss[ nbr ]
                print "%s: ping %s (%s)" % ( name, ip, nbr )
                _, out = dv.runcmd(['sh', "-c", "ping -c 5 -W 0 -q %s" % ip])
                print out
                assert '5 received' in out
