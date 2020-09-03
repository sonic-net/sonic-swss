import time
import pytest

import test_vnet as vnet

from swsscommon import swsscommon


# Define fake platform for "DVS" fixture, so it will set "platform" environment variable for "orchagent".
# It is needed in order to enable platform specific "orchagent" code for testing "bitmap" VNET implementation.
DVS_FAKE_PLATFORM = "mellanox"


'''
Provides test cases for the "bitmap" VNET implementation.
Test cases are inherited from "test_vnet.py::TestVnetOrch" since they are the same for both "legacy" and "bitmap" implementation.
Difference between these two implementations is in set SAI attributes, so different values should be checked in ASIC_DB.
This class should override "get_vnet_obj()" method in order to return object with appropriate implementation of "check" APIs.
'''
class TestVnetBitmapOrch(vnet.TestVnetOrch):

    '''
    Returns specific VNET object with the appropriate implementation of "check" APIs for the "bitmap" VNET.
    Test cases use these "check" APIs in order to verify whether correct config is applied to ASIC_DB.
    '''
    def get_vnet_obj(self):
        return vnet.VnetVxlanVrfTunnel()

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
