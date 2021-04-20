import json
from dvslib.dvs_common import wait_for_result

class DVSRoute(object):
    def __init__(self, adb, cdb):
        self.asic_db = adb
        self.config_db = cdb

    def check_asicdb_route_entries(self, destinations):
        def _access_function():
            route_entries = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"]
                                  for route_entry in route_entries]
            return (all(destination in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

        keys = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")

        return [k for k in keys if json.loads(k)['dest'] in destinations]

    def check_asicdb_deleted_route_entries(self, destinations):
        def _access_function():
            route_entries = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"] for route_entry in route_entries]
            return (all(destination not in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)
