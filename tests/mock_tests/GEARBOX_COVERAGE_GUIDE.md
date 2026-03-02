# Gearbox Coverage Guide: initGearboxPort, setGearboxPortsAttr, setGearboxPortAttr, updateGearboxPortOperStatus

## Overview

These functions only run when **m_gearboxEnabled** is true. Gearbox is enabled when:
1. `platformHasGearbox()` returns true (checks `/usr/share/sonic/hwsku/gearbox_config.json`)
2. `isGearboxConfigDone(gearboxTable)` returns true (GearboxConfigDone in _GEARBOX_TABLE)

## Approach Options

### Option A: Environment Variable (Recommended - Minimal Change)

Add a test hook in `gearboxutils.cpp`:

```cpp
bool GearboxUtils::platformHasGearbox()
{
    if (getenv("GEARBOX_UT_TEST_ENABLE"))
        return true;

    if (access("/usr/share/sonic/hwsku/gearbox_config.json", F_OK) != -1)
        return true;
    return false;
}
```

Set `GEARBOX_UT_TEST_ENABLE=1` in test SetUp, unset in TearDown.

### Option B: Wrap access() (No Production Code Change)

Add to Makefile.am for gearbox tests:
```
tests_CXXFLAGS += -Wl,-wrap,access
```

Add to gearbox_ut.cpp:
```cpp
extern "C" int __real_access(const char *pathname, int mode);

extern "C" int __wrap_access(const char *pathname, int mode)
{
    if (pathname && strstr(pathname, "gearbox_config.json"))
        return 0;  // Simulate file exists
    return __real_access(pathname, mode);
}
```

**Caution**: This affects ALL tests in the binary; ensure no other test relies on access() for gearbox path.

### Option C: Create Config File in Test

In SetUp, create the file (may require test to run with appropriate permissions):
```cpp
// Create hwsku dir and empty config file
mkdir -p /usr/share/sonic/hwsku
touch /usr/share/sonic/hwsku/gearbox_config.json
```

## Required Test Setup

### 1. Populate _GEARBOX_TABLE Before PortsOrch Creation

The gearbox table must be populated **before** PortsOrch constructor runs (initGearbox is called in ctor):

```cpp
void setupGearboxTable(swss::DBConnector* app_db) {
    swss::Table gearboxTable(app_db, "_GEARBOX_TABLE");

    // Must have GearboxConfigDone for isGearboxEnabled to return true
    gearboxTable.set("GearboxConfigDone", {});

    // PHY - phy_oid must be valid SAI switch OID (use gSwitchId)
    gearboxTable.set("phy:1", {
        {"phy_id", "1"},
        {"phy_oid", "oid:0x1000000000001"},  // Or sai_serialize_object_id(gSwitchId)
        {"name", "phy1"}
    });

    // Interface - index 0 matches port index from SAI default ports
    gearboxTable.set("interface:0", {
        {"index", "0"},
        {"phy_id", "1"},
        {"line_lanes", "0, 1, 2, 3"},
        {"system_lanes", "100, 101, 102, 103"}
    });

    // Port config for index 0
    gearboxTable.set("phy:1:ports:0", {
        {"index", "0"},
        {"system_speed", "100000"},
        {"system_fec", "rs"},
        {"system_auto_neg", "false"},
        {"system_loopback", "none"},
        {"system_training", "false"},
        {"line_speed", "100000"},
        {"line_fec", "rs"},
        {"line_auto_neg", "true"},
        {"line_media_type", "fiber"},
        {"line_intf_type", "cr4"},
        {"line_loopback", "none"},
        {"line_training", "false"},
        {"line_adver_speed", "100000"},
        {"line_adver_fec", "1"},
        {"line_adver_auto_neg", "true"},
        {"line_adver_asym_pause", "false"},
        {"line_adver_media_type", "fiber"}
    });
}
```

### 2. Port Index Must Match Gearbox Interface

`initGearboxPort` checks `m_gearboxInterfaceMap.find(port.m_index)`. The SAI default ports from `ut_helper::getInitialSaiPorts()` have indices. Ensure a port with index 0 exists (Ethernet0 typically has index 0).

### 3. Mock SAI Port API for Gearbox

initGearboxPort calls:
- `sai_port_api->create_port()` - for system-side and line-side ports
- `sai_port_api->create_port_connector()` - to connect them

The SAI VS (libsaivs) may support these. If not, hook the SAI port API:

```cpp
sai_object_id_t g_mock_system_port = 0x2000;
sai_object_id_t g_mock_line_port = 0x2001;

sai_status_t mock_create_port(sai_object_id_t *port_id, sai_object_id_t switch_id, ...) {
    static int count = 0;
    *port_id = (count++ == 0) ? g_mock_system_port : g_mock_line_port;
    return SAI_STATUS_SUCCESS;
}
sai_status_t mock_create_port_connector(...) { return SAI_STATUS_SUCCESS; }
```

### 4. Add GB_COUNTERS_DB to database_config.json

initGearbox creates `DBConnector("GB_COUNTERS_DB", 0)`. Ensure tests/mock_tests/database_config.json has GB_COUNTERS_DB defined (it does per current config).

### 5. Required Orchestrators

PortsOrch needs BufferOrch, FlexCounterOrch, etc. for registerPort. The GearboxPortsOrchTest fixture is minimal - you may need to add BufferOrch, FlexCounterOrch, and port initialization similar to portphyattr_ut.cpp or portsorch_ut.cpp.

## Test Flow for initGearboxPort

1. Set GEARBOX_UT_TEST_ENABLE (or use Option B/C)
2. setupGearboxTable(app_db) - before PortsOrch
3. Create PortsOrch → initGearbox() will enable gearbox
4. Add ports via PortConfigDone, PortInitDone (like portsorch_ut)
5. doTask() → registerPort() → initGearboxPort()
6. Verify m_gearboxPortListLaneMap has entry, port.m_system_side_id and port.m_line_side_id set

## Test Flow for setGearboxPortsAttr / setGearboxPortAttr

1. Complete initGearboxPort flow first (so m_gearboxPortListLaneMap is populated)
2. Call setPortAdminStatus(port, true) → triggers setGearboxPortsAttr(SAI_PORT_ATTR_ADMIN_STATE)
3. Or setPortMtu() → triggers setGearboxPortsAttr(SAI_PORT_ATTR_MTU)
4. Or setPortFec() → triggers setGearboxPortsAttr(SAI_PORT_ATTR_FEC_MODE)

## Test Flow for updateGearboxPortOperStatus

1. Complete initGearboxPort flow (port has system_side_id, line_side_id)
2. updateGearboxPortOperStatus is called from updatePortOperStatus when port oper status changes
3. Simulate port status notification or call the code path that triggers it
4. Mock sai_port_api->get_port_attribute for SAI_PORT_ATTR_OPER_STATUS

## Key Dependencies

- **loopback_mode_map**, **media_type_map**, **interface_type_map** - must have entries for config values ("none", "fiber", "cr4")
- **m_portHlpr.fecToSaiFecMode()** - "rs" must map to valid SAI FEC mode
- **isSpeedSupported()** - may need to return true for gearbox speeds
