# Docker Virtual Switch (DVS) Developer Guide

## Overview

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
Sometimes we don't care what the specific output is - we just want to check if *something* exists. This is typically done to check if we've finished cleaning up the database or not.

For entries, we can use `wait_for_entry` to check if something exists at a specific key, and `wait_for_deleted_entry` to check that nothing exists at the specified key.

**Example:**
```

```

For keys, we can use the `wait_for_n_keys` to check that a specified number of keys exist. Waiting for 0 keys is a great way to check if a table is empty or not.

**Example:**
```

```

### Wait Guards
Sometimes we need to ensure that a series of configuration steps happen in the correct order, but we don't necessarily care about the intermediate state.

For example, maybe some process takes 3 steps and we have already checked that the first 2 steps are correct in earlier test cases.

In this case, it's helpful to add empty `wait` methods to ensure that the configuration happens in the intended order.

**Example 1:**
```

```

**Example 2:**
```

```

### User-Defined Wait Methods
Some features have behavior that is too specific to be covered by the generic `wait` methods in `dvs_database`. A common example of this are features whose state is encoded in the actual *keys* stored in Redis.

For such cases it makes sense for the user to use the `wait_for_result` API from `dvs_common` to defined their own custom wait method rather than adding an application-specific API to the database library (see "`dvslib` Design Principles" for more info).

**Example:**
```

```

## API Quick Reference (WIP)

### `dvs_common`
`dvs_common` contains generic utilities that can be re-used across tests and test libraries (e.g. polling).

### `dvs_database`
`dvs_database` contains utilities for accessing the Redis DBs in the virtual switch.

## `dvslib` Design Principles (WIP)

### 1. If it can't be reused, don't put it in `dvslib`.

The idea of `dvslib` is to facilitate collaboration and code-reuse between test authors. Adding code that's too specific to any one particular test case clutters the library and makes it more difficult for contributors to find the tools they need to write their own tests.

**Example:**
```

```

### 2. Prefer high-level APIs.

We want it to be easy to write new tests, and we want it to be easy for future maintainers to *understand* those tests. (WIP)

#### "Mixed" APIs
In some cases, it may be difficult or impossible to provide one universal interface for all test authors. This is especially true for foundational services like database access, syslog parsing, and so on.

For such APIs we still recommend you provide a high-level API for the most common use cases. However, it may be appropriate to also expose the "building-block" APIs you used under the hood to help users build their own APIs for their specific use-cases. `dvs_database` is a good example of this pattern.

**Example:**
```

```

### 3. Prefer object-based APIs, and expose those APIs through the `dvs` fixture.

Because our tests are primarily based on checking device state, we have to manage a lot of state and state updates in our tests. This is much simpler to do, and much simpler for other contributors to understand, if we abstract this state using object APIs.

**Example:**
```

```

Exposing these APIs through the `dvs` fixture makes it easier to implement system-wide features like test pre/post-checks and garbage collection. Additionally, we want to support multiple virtual switches in the future, and associating each object with its parent device makes that effort simpler.
