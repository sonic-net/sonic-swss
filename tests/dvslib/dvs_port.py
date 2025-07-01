"""Utilities for interacting with PORT objects when writing VS tests."""

from typing import Dict, List
from swsscommon import swsscommon


class DVSPort(object):
    """Manage PORT objects on the virtual switch."""
    ASIC_DB = swsscommon.ASIC_DB
    APPL_DB = swsscommon.APPL_DB

    CHANNEL_UNITTEST = "SAI_VS_UNITTEST_CHANNEL"

    ASIC_VIDTORID = "VIDTORID"

    CFGDB_PORT = "PORT"
    APPDB_PORT = "PORT_TABLE"
    ASICDB_PORT = "ASIC_STATE:SAI_OBJECT_TYPE_PORT"

    COUNTERS_COUNTERS = "COUNTERS"
    COUNTERS_PORT_NAME_MAP = "COUNTERS_PORT_NAME_MAP"

    def __init__(self, asicdb, appdb, cfgdb, counters_db):
        self.asic_db = asicdb
        self.app_db = appdb
        self.config_db = cfgdb
        self.counters_db = counters_db

    def create_port_generic(
        self,
        port_name: str,
        lanes: str,
        speed: str,
        qualifiers: Dict[str, str] = {}
    ) -> None:
        """Create PORT in Config DB."""
        attr_dict = {
            "lanes": lanes,
            "speed": speed,
            **qualifiers
        }

        self.config_db.create_entry(self.CFGDB_PORT, port_name, attr_dict)

    def remove_port_generic(
        self,
        port_name: str
    )-> None:
        """Remove PORT from Config DB."""
        self.config_db.delete_entry(self.CFGDB_PORT, port_name)

    def remove_port(self, port_name):
        self.config_db.delete_field("CABLE_LENGTH", "AZURE", port_name)
        
        port_bufferpg_keys = self.config_db.get_keys("BUFFER_PG|%s" % port_name)
        for key in port_bufferpg_keys:
            self.config_db.delete_entry("BUFFER_PG|%s|%s" % (port_name, key), "")
        
        port_bufferqueue_keys = self.config_db.get_keys("BUFFER_QUEUE|%s" % port_name)
        for key in port_bufferqueue_keys:
            self.config_db.delete_entry("BUFFER_QUEUE|%s|%s" % (port_name, key), "")
            
        self.config_db.delete_entry("BREAKOUT_CFG|%s" % port_name, "")
        self.config_db.delete_entry("INTERFACE|%s" % port_name, "")
        self.config_db.delete_entry("PORT", port_name)

    def update_port(
        self,
        port_name: str,
        attr_dict: Dict[str, str]
    ) -> None:
        """Update PORT in Config DB."""
        self.config_db.update_entry(self.CFGDB_PORT, port_name, attr_dict)

    def get_port_id(
        self,
        port_name: str
    ) -> str:
        """Get port id from COUNTERS DB."""
        attr_list = [ port_name ]
        fvs = self.counters_db.wait_for_fields(self.COUNTERS_PORT_NAME_MAP, "", attr_list)

        return fvs[port_name]

    def get_port_ids(
        self,
        expected: int = None,
        dbid: int = swsscommon.ASIC_DB
    ) -> List[str]:
        """Get all of the PORT objects in ASIC/APP DB."""
        conn = None
        table = None

        if dbid == swsscommon.ASIC_DB:
            conn = self.asic_db
            table = self.ASICDB_PORT
        elif dbid == swsscommon.APPL_DB:
            conn = self.app_db
            table = self.APPDB_PORT
        else:
            raise RuntimeError("Interface not implemented")

        if expected is None:
            return conn.get_keys(table)

        return conn.wait_for_n_keys(table, expected)

    def set_port_counter(
        self,
        sai_port_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Set port counter value in ASIC DB."""
        attr_list = [ sai_port_id ]
        fvs = self.asic_db.wait_for_fields(self.ASIC_VIDTORID, "", attr_list)

        ntf = swsscommon.NotificationProducer(self.asic_db.db_connection, self.CHANNEL_UNITTEST)

        # Enable test mode
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "true", fvp)

        # Set queue stats
        key = fvs[sai_port_id]
        fvp = swsscommon.FieldValuePairs(list(sai_qualifiers.items()))
        ntf.send("set_stats", str(key), fvp)

        # Disable test mode
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "false", fvp)

    def verify_port_counter(
        self,
        sai_port_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that port counter object has correct COUNTERS DB representation."""
        self.counters_db.wait_for_field_match(self.COUNTERS_COUNTERS, sai_port_id, sai_qualifiers)

    def verify_port_count(
        self,
        expected: int,
        dbid: int = swsscommon.ASIC_DB
    ) -> None:
        """Verify that there are N PORT objects in ASIC/APP DB."""
        self.get_port_ids(expected, dbid)
