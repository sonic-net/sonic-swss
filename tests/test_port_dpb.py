from swsscommon import swsscommon
import redis
import time
import os
import pytest
from pytest import *
import json
import re
from port_dpb import DPB

speed100G = 100000
speed50G = 50000
speed40G = 40000
speed25G = 25000
speed10G = 10000
maxPorts = 128
maxRootPorts = 32
maxBreakOut = 4

@pytest.mark.usefixtures('dpb_setup_fixture')
@pytest.mark.xfail(reason="sonic cfggen bug: buildimage#5263")
class TestPortDPB(object):

    '''
    |--------------------------------------------------
    |        | 1X100G | 1X40G | 4X10G | 4X25G | 2X50G |
    |--------------------------------------------------
    | 1X100G |   NA   |   P   |   P   |   P   |   P   |
    |--------------------------------------------------
    | 1X40G  |   P    |   NA  |   P   |   P   |       |
    |--------------------------------------------------
    | 4X10G  |   P    |   P   |   NA  |   P   |       |
    |--------------------------------------------------
    | 4X25G  |   P    |   P   |   P   |   NA  |       |
    |--------------------------------------------------
    | 2X50G  |   P    |       |       |       |   NA  |
    |--------------------------------------------------
    NA    --> Not Applicable
    P     --> Pass
    F     --> Fail
    Empty --> Not Tested
    '''

    def test_port_breakout_one(self, dvs):
        dpb = DPB()
        dpb.breakout(dvs, "Ethernet0", maxBreakOut)
        #print "**** 1X40G --> 4X10G passed ****"
        dpb.change_speed_and_verify(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"], speed25G)
        #print "**** 4X10G --> 4X25G passed ****"
        dpb.change_speed_and_verify(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"], speed10G)
        #print "**** 4X25G --> 4X10G passed ****"
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])
        #print "**** 4X10G --> 1X40G passed ****"
        dpb.change_speed_and_verify(dvs, ["Ethernet0"], speed100G)
        #print "**** 1X40G --> 1X100G passed ****"
        dpb.breakout(dvs, "Ethernet0", maxBreakOut)
        #print "**** 1X100G --> 4X25G passed ****"
        dpb.change_speed_and_verify(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"], speed10G)
        #print "**** 4X25G --> 4X10G passed ****"
        dpb.change_speed_and_verify(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"], speed25G)
        #print "**** 4X10G --> 4X25G passed ****"
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])
        #print "**** 4X25G --> 1X100G passed ****"
        dpb.breakout(dvs, "Ethernet0", maxBreakOut//2)
        #print "**** 1X100G --> 2X50G passed ****"
        dpb.breakin(dvs, ["Ethernet0", "Ethernet2"])
        #print "**** 2X50G --> 1X100G passed ****"
        dpb.change_speed_and_verify(dvs, ["Ethernet0"], speed40G)
        #print "**** 1X100G --> 1X40G passed ****"

    def test_port_breakout_multiple(self, dvs):
        dpb = DPB()
        port_names = ["Ethernet0", "Ethernet12", "Ethernet64", "Ethernet112"]
        for pname in port_names:
            dpb.breakout(dvs, pname, maxBreakOut)
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])
        dpb.breakin(dvs, ["Ethernet12", "Ethernet13", "Ethernet14", "Ethernet15"])
        dpb.breakin(dvs, ["Ethernet64", "Ethernet65", "Ethernet66", "Ethernet67"])
        dpb.breakin(dvs, ["Ethernet112", "Ethernet113", "Ethernet114", "Ethernet115"])

    @pytest.mark.skip("breakout_all takes too long to execute")
    def test_port_breakout_all(self, dvs):
        dpb = DPB()
        port_names = []
        for i in range(maxRootPorts):
            pname = "Ethernet" + str(i*maxBreakOut)
            port_names.append(pname)

        for pname in port_names:
            dpb.breakout(dvs, pname, maxBreakOut)

        child_port_names = []
        for i in range(maxPorts):
            cpname = "Ethernet" + str(i)
            child_port_names.append(cpname)

        for i in range(32):
            start = i*maxBreakOut
            end = start+maxBreakOut
            dpb.breakin(dvs, child_port_names[start:end])


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
