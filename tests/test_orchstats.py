from swsscommon import swsscommon

def test_orchstats(dvs: conftest.DockerVirtualSwitch,):
    tb=swsscommon.Table(dvs.get_counters_db(), "ORCH_STATS_TABLE")
    assert(tb.get("PORT")[0])
