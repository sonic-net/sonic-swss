import time
import os
import pytest

from swsscommon import swsscommon



def test_gcda_collection(dvs):
    dvs.runcmd("/tmp/gcov/gcov_support.sh collect_gcda")

def test_gcda_generate_all(dvs):
    dvs.runcmd("/tmp/gcov/gcov_support.sh generate all")

def test_nonflaky_dummy():
    pass
