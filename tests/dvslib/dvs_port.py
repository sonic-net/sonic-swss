"""Utilities for interacting with PORT objects when writing VS tests."""
from typing import Dict, List
from swsscommon import swsscommon


class DVSPort(object):
    """Manage PORT objects on the virtual switch."""
    ASIC_DB = swsscommon.ASIC_DB
    APPL_DB = swsscommon.APPL_DB

    CFGDB_PORT = "PORT"
    APPDB_PORT = "PORT_TABLE"
    ASICDB_PORT = "ASIC_STATE:SAI_OBJECT_TYPE_PORT"

    def __init__(self, asicdb, appdb, cfgdb):
        self.asic_db = asicdb
        self.app_db = appdb
        self.config_db = cfgdb

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

    def verify_port_count(
        self,
        expected: int,
        dbid: int = swsscommon.ASIC_DB
    ) -> None:
        """Verify that there are N PORT objects in ASIC/APP DB."""
        self.get_port_ids(expected, dbid)
