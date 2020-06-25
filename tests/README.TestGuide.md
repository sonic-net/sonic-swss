# Docker Virtual Switch (DVS) Testing Guide

## Writing New Tests
Any new feature, change, or bug fix to SWSS should ideally come with a new virtual switch test (assuming it modifies the control plane behavior).

To make this easier, we've added some APIs in `tests/dvslib` to make it easier to implement common test patterns. Please refer to the API Quick Reference at the bottom of this page, as well as the documentation in `dvslib`, for more details.

## Common Design Patterns
### Create-Verify-Delete-Verify
The most common operation you will see in the virtual switch tests is some variation of:

- Create/configure an object
- Verify that the correct output is produced
- Delete the original object
- Verify that the output has been removed

This might look like:

- Add a VLAN to Config DB
- Check that the VLAN exists in ASIC DB
- Delete the VLAN
- Check that there are no VLANs in ASIC DB

**NOTE:** The last Verify step is important to ensure that succesive test cases start in a clean, known state.

In order to avoid timing issues, it's recommended to use the `wait` methods in `dvs_database` to access the database for the Verify steps. There are a few ways to do this.

#### Verify some specific output
Typically you'll know what output you expect to see in the database. In this case, you can use the `wait_for_exact_match` to check for the entry you expect.

**Example:**
```

```

Sometimes you only care about (or can only verify) a few specific fields. This is especially common for update operations (e.g. "set MTU to 7777"). In this case, you can use `wait_for_field_match` to check for the specific fields you expect.

**Example:**
```

```

Finally, there are some test cases where the output we care about is actually contained in the keys. For these cases, `wait_for_matching_keys` can poll for the keys you expect.

**Example:**
```

```

Similarly, `wait_for_deleted_keys` can help you check that specific keys *don't* exist.

**Example:**
```

```

#### Verify *any* output
Sometimes we don't care what the specific output is - we just want to check if *something* exists. One example of this is to check if we've finished cleaning up the database or not.

For entries, we can use `wait_for_entry` to check if something exists at a specific key, and `wait_for_deleted_entry` to check that nothing exists at the specified key.

**Example:**
```
self.remove_acl_table(table1)

# No need to assert, this method will fail if the entry still exists
self.asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", table_oid)
```

For keys, we can use the `wait_for_n_keys` to check that a specified number of keys exist. Waiting for 0 keys is a great way to check if a table is empty or not.

**Example:**
```
self.create_vlan(vlan1)

# No need to assert, this method will fail if the key isn't found
self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER", 1)
```

### Wait Guards
Sometimes we need to ensure that a series of configuration steps happen in the correct order, but we don't necessarily care about the intermediate state.

For example, maybe some process takes 3 steps and we have already checked that the first 2 steps are correct in earlier test cases.

In this case, it's still helpful to use the `wait` methods to ensure that the configuration happens in the intended order. If exact fields have already been checked in previous test cases, then it's enough to just check if something does (or doesn't) exist before proceeding.

**Example 1:**
```
self.remove_port_channel_member(lag_id, lag_member)

# Here we make sure that the LAG member is deleted, otherwise we won't be able
# to delete the LAG
self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 0)

self.remove_port_channel(lag_id)
```

**Example 2:**
```
self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)

# Here we have to ensure that the IP addresses are removed from the interface,
# otherwise removing the profile will crash the device
self.app_db.wait_for_deleted_keys(APP_INTF_TABLE_NAME, [self.IPV4_ADDR_UNDER_TEST,self.IPV6_ADDR_UNDER_TEST])

self.remove_sub_port_intf_profile(sub_port_intf_name)
```

### User-Defined Wait Methods
Some features have behavior that is too specific to be covered by the generic `wait` methods in `dvs_database`. A common example of this are features whose state is encoded in the actual *keys* stored in Redis.

For such cases it makes sense for the user to use the `wait_for_result` API from `dvs_common` to defined their own custom wait method rather than adding an application-specific API to the database library.

**Example:**
```
def check_sub_port_intf_route_entries(self):
    expected_destinations = [self.IPV4_TOME_UNDER_TEST,
                             self.IPV4_SUBNET_UNDER_TEST,
                             self.IPV6_TOME_UNDER_TEST,
                             self.IPV6_SUBNET_UNDER_TEST]

    def _access_function():
        raw_route_entries = self.asic_db.get_keys(ASIC_ROUTE_ENTRY_TABLE)

        # We have to convert the key to a dictionary and fetch a specific
        # value from it - not something the DB library is equipped to do!
        route_destinations = [str(json.loads(raw_route_entry)["dest"])
                                for raw_route_entry in raw_route_entries]

        return (all(dest in route_destinations for dest in expected_destinations), None)

    wait_for_result(_access_function, DEFAULT_POLLING_CONFIG)
```

## API Quick Reference (WIP)

### `dvs_common`
`dvs_common` contains generic utilities that can be re-used across tests and test libraries (e.g. polling).

### `dvs_database`
`dvs_database` contains utilities for accessing the Redis DBs in the virtual switch.
