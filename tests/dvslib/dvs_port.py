
class DVSPort(object):
    def __init__(self, adb, cdb):
        self.asic_db = adb
        self.config_db = cdb

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
