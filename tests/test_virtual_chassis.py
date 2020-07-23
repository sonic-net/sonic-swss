import time
import json
import redis
import pytest
from swsscommon import swsscommon

class TestVirtualChassis(object):
    def test_connectivity(self, dvs, testlog):
        if dvs.vct is None:
            return
        vct = dvs.vct
        dvss = {}
        dvss = vct.dvss
        ns = vct.ns
        nbrs = vct.get_topo_neigh()
        for name in dvss.iterkeys():
            dv = dvss[name]
            #ping all vs's inband address
            for ctn in vct.inbands.keys():
                ip = vct.inbands[ctn]["inband_address"]
                ip = ip.split("/")[0]
                print("%s: ping inband address %s" % (name, ip))
                _, out = dv.runcmd(['sh', "-c", "ping -c 5 -W 0 -q %s" % ip])
                print(out)
                assert '5 received' in out
            if name not in nbrs.keys():
                continue
            for item in nbrs[name]:
                ip = str(item[1])
                print("%s: ping neighbor address %s" % (name, ip))
                _, out = dv.runcmd(['sh', "-c", "ping -c 5 -W 0 -q %s" % ip])
                print(out)
                assert '5 received' in out
