""" Defines common P4RT utility functions."""
from swsscommon import swsscommon

import time


def _set_up_appl_db(dvs):
  """ Initializes application database connector."""
  return swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)


def _set_up_asic_db(dvs):
  """ Initializes ASIC database connector."""
  return swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)


def _set_up_appl_state_db(dvs):
  """ Initializes APPL STATE database connector."""
  return swsscommon.DBConnector(swsscommon.APPL_STATE_DB, dvs.redis_sock, 0)


def get_keys(db, tbl_name):
  """ Retrieves keys from given database and table."""
  tbl = swsscommon.Table(db, tbl_name)
  return tbl.getKeys()


def get_key(db, tbl_name, key):
  """ Retrieves entry corresponding to given key in given database and table."""
  tbl = swsscommon.Table(db, tbl_name)
  return tbl.get(key)


def verify_attr(fvs, attr_list):
  """ Verifies attribute list for given key in a database table."""
  assert len(fvs) == len(attr_list)
  d = dict(attr_list)
  for fv in fvs:
    if fv[0] in d:
      assert fv[1] == d[fv[0]]
    else:
      assert False

def prepend_match_field(match_field):
  return "match/" + match_field

def prepend_param_field(param_field):
  return "param/" + param_field

def verify_response(consumer, key, attr_list, status, err_message = "SWSS_RC_SUCCESS"):
  """ Verifies a response."""
  consumer.readData()
  (op, data, values) = consumer.pop()
  assert data == key
  assert op == status
  assert len(values) >= 1
  assert values[0][0] == "err_str"
  assert values[0][1] == err_message
  values = values[1:]
  verify_attr(values, attr_list)


def check_syslog(dvs, marker, process, err_log, expected_cnt):
  """ Checks syslog on dvs docker.

  Scans /var/log/syslog for expected count (expected_cnt) of the error
  log(err_log). Filters Logs starting at timestamp marked by "marker" based on
  the given process.
  """
  (exitcode, num) = dvs.runcmd([
      "sh", "-c",
      "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep %s | grep -E \'%s\' | wc -l"
      % (marker, process, err_log)
  ])
  assert num.strip() == str(expected_cnt)

def get_port_oid_by_name(dvs, port_name):
  counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
  port_map_tbl = swsscommon.Table(counters_db, "COUNTERS_PORT_NAME_MAP")
  port_oid = None
  for k in port_map_tbl.get("")[1]:
        if k[0] == port_name:
            port_oid = k[1]
  return port_oid

def initialize_interface(dvs, port_name, ip):
  dvs.port_admin_set(port_name, "up")
  dvs.interface_ip_add(port_name, ip)

def set_interface_status(dvs, if_name, status = "down", server = 0):
  dvs.servers[0].runcmd("ip link set {} dev {}".format(status, if_name)) == 0
  time.sleep(1)

class DBInterface(object):
  """ Interface to interact with different redis databases on dvs."""

  # common attribute fields for L3 objects
  ACTION_FIELD = "action"

  def set_up_databases(self, dvs):
    self.appl_db = _set_up_appl_db(dvs)
    self.asic_db = _set_up_asic_db(dvs)
    self.appl_state_db = _set_up_appl_state_db(dvs)

  def set_app_db_entry(self, key, attr_list):
    fvs = swsscommon.FieldValuePairs(attr_list)
    tbl = swsscommon.ProducerStateTable(self.appl_db, self.APP_DB_TBL_NAME)
    tbl.set(key, fvs)
    time.sleep(1)

  def remove_app_db_entry(self, key):
    tbl = swsscommon.ProducerStateTable(self.appl_db, self.APP_DB_TBL_NAME)
    tbl._del(key)
    time.sleep(1)

  # Get list of original entries in redis on init.
  def get_original_redis_entries(self, db_list):
    self._original_entries = {}
    for i in db_list:
      db = i[0]
      table = i[1]
      self._original_entries["{}:{}".format(db, table)]= get_keys(db, table)

class KeyToOidDBHelper(object):
  """Provides helper APIs for P4RT key to OID mapping in Redis DB."""

  # Table name in Redis DB for the mapping.
  TBL_NAME = "P4RT_KEY_TO_OID"
  KEY = ""

  def __init__(self, dvs):
    self.table = swsscommon.Table(_set_up_appl_state_db(dvs), self.TBL_NAME)

  def get_db_info(self):
    return self.table.get(self.KEY)
