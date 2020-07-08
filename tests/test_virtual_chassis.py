import time
import json
import redis
import pytest
from swsscommon import swsscommon

class TestVirtualChassis(object):
    def ctn_runcmd(self, ctn, cmd):
        res = ctn.exec_run(cmd)
        try:
            exitcode = res.exit_code
            out = res.output
        except AttributeError:
            exitcode = 0
            out = res
        if exitcode != 0:
            print "-----rc={} for cmd {}-----".format(exitcode, cmd)
            print out.rstrip()
            print "-----"

        return (exitcode, out)

    def check_chassis_sysport(self, ctn, dvs):
        # check if instance is connected to a chassis db
        cfgFile = "/usr/share/sonic/hwsku/default_config.json"
        (ret, out) = self.ctn_runcmd(ctn, "sonic-cfggen -v  DEVICE_METADATA.localhost.chassis_db_address -j %s" % cfgFile)
        verifyChassDb = True
        if ret != 0 or out.strip() == "":
            # instance has no connection to chassis db
            verifyChassDb = False
            print "verify vs without chassisDb"
        (ret, out) = self.ctn_runcmd(ctn, "sonic-db-cli CHASSIS_DB keys SYSTEM_PORT\"*\"")
        if verifyChassDb:
            assert len( out.split() ) == 96
        else:
            assert 'Invalid database name' in out
        (ret, out) = self.ctn_runcmd(ctn,"sonic-db-cli CHASSIS_DB HGETALL 'SYSTEM_PORT|Linecard3|Ethernet0'")
        out = out.replace("u'", "'")
        if verifyChassDb:
            assert "'system_port_id': '65'" in out
            assert "'switch_id': '2'" in out
        else:
            assert 'Invalid database name' in out

    def test_chassis_sysport(self, dvs, testlog):
        ctns = {}
        if dvs.vct is not None:
            ctns = dvs.vct.ctns
        else:
            ctns[ 0 ] = dvs.ctn
        for ctname in ctns.keys():
            print "checking %s" % ctname
            self.check_chassis_sysport( ctns[ctname], dvs )

    def test_connectivity(self, dvs, testlog):
        if dvs.vct is None:
            return
        ctns = {}
        ctns = dvs.vct.ctns
        pings = {}
        ns = dvs.vct.nsname
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
        for ctname in ctns.keys():
            if ctname not in nbrs:
                continue
            for nbr, ip in nbrs[ ctname ].iteritems():
                if nbr not in ctns.keys():
                    continue
                print "%s: ping %s (%s)" % ( ctname, ip, nbr )
                (ret, out) = self.ctn_runcmd( ctns[ctname], [ 'sh', "-c", "ping -c 5 -W 0 -q %s" % ip ]  )
                print out
                assert '5 received' in out
