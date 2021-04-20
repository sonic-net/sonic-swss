class TestAclCli:
    def test_AddTableMultipleTimes(self, dvs, dvs_acl):
        dvs.runcmd("config acl add table TEST L3 -p Ethernet0")

        cdb = dvs.get_config_db()
        cdb.wait_for_field_match(
            "ACL_TABLE",
            "TEST",
            {"ports": "Ethernet0"}
        )

        # Verify that subsequent updates don't delete "ports" from config DB
        dvs.runcmd("config acl add table TEST L3 -p Ethernet4")
        cdb.wait_for_field_match(
            "ACL_TABLE",
            "TEST",
            {"ports": "Ethernet4"}
        )

        # Verify that subsequent updates propagate to ASIC DB
        L3_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12"]
        dvs.runcmd(f"config acl add table TEST L3 -p {','.join(L3_BIND_PORTS)}")
        acl_table_id = dvs_acl.get_acl_table_ids(1)[0]
        acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(L3_BIND_PORTS))

        dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
        dvs_acl.verify_acl_table_port_binding(acl_table_id, L3_BIND_PORTS, 1)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
