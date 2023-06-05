from swsscommon import swsscommon

def test_orchstats():
    db = swsscommon.DBConnector("COUNTERS_DB", 0)
    tb=swsscommon.Table(db, "ORCH_STATS_TABLE")
    assert(tb.get("PORT")[0])
