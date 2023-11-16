from swsscommon import swsscommon

import util
import json


class P4RtTableDefinitionWrapper(util.DBInterface):
    """Interface to interact with APP DB for P4RT tables definition."""

    # database constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = swsscommon.APP_P4RT_TABLES_DEFINITION_TABLE_NAME

    # attribute fields for tables definition object
    INFO_FIELD = "info"

    # tables definition object's attribute values
    INFO_VALUE = "{\"tables\":[{\"actions\":[{\"alias\":\"drop\",\"id\":16777222,\"name\":\"ingress.routing.drop\",\"params\":null},{\"alias\":\"set_nexthop_id\",\"id\":16777221,\"name\":\"ingress.routing.set_nexthop_id\",\"params\":[{\"bitwidth\":0,\"format\":\"STRING\",\"id\":1,\"name\":\"nexthop_id\",\"references\":[{\"match\":\"nexthop_id\",\"table\":\"nexthop_table\"}]}]},{\"alias\":\"set_wcmp_group_id\",\"id\":16777220,\"name\":\"ingress.routing.set_wcmp_group_id\",\"params\":[{\"bitwidth\":0,\"format\":\"STRING\",\"id\":1,\"name\":\"wcmp_group_id\",\"references\":[{\"match\":\"wcmp_group_id\",\"table\":\"wcmp_group_table\"}]}]}],\"alias\":\"vipv4_table\",\"counter/unit\":\"BOTH\",\"id\":33554500,\"matchFields\":[{\"bitwidth\":32,\"format\":\"IPV4\",\"id\":1,\"name\":\"ipv4_dst\",\"references\":null}],\"name\":\"ingress.routing.vipv4_table\"}]}"


    def generate_app_db_key(self, context):
        d = {}
        d["context"] = context
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key


    # create tables definition set
    def create_tables_definition(self, info=None):
        info = info or self.INFO_VALUE
        attr_list = [(self.INFO_FIELD, info)]
        tables_definition_key = self.generate_app_db_key("0")
        self.set_app_db_entry(tables_definition_key, attr_list)
        return tables_definition_key, attr_list

