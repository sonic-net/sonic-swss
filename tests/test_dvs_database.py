"""Tests for the DVSDatabase helpers themselves."""
import pytest

# No daemon subscribes to this table, so writing it has no side effects.
TABLE = "DVSLIB_RESTORE_TEST"
KEY = "row"


@pytest.fixture
def config_db(dvs):
    db = dvs.get_config_db()
    db.delete_entry(TABLE, KEY)
    yield db
    db.delete_entry(TABLE, KEY)


class TestRestoreEntry(object):
    def test_restore_entry_puts_row_back_exactly(self, config_db):
        config_db.create_entry(TABLE, KEY, {"keep": "1", "change": "old"})
        snapshot = config_db.get_entry(TABLE, KEY)

        config_db.update_entry(TABLE, KEY, {"change": "new", "added": "x"})
        config_db.restore_entry(TABLE, KEY, snapshot)

        assert config_db.get_entry(TABLE, KEY) == {"keep": "1", "change": "old"}

    def test_restore_entry_empty_snapshot_deletes_row(self, config_db):
        snapshot = config_db.get_entry(TABLE, KEY)
        assert snapshot == {}

        config_db.create_entry(TABLE, KEY, {"added": "x"})
        config_db.restore_entry(TABLE, KEY, snapshot)

        assert KEY not in config_db.get_keys(TABLE)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
