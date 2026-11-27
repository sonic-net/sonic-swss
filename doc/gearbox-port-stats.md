# Gearbox port statistics sides

The optional per-PHY `port_stats_supported_sides` field selects which gearbox
port objects can be registered with the `PORT_STAT_COUNTER` flex-counter group.
It applies to every interface belonging to that PHY.

In the existing `phys[]` entry in `gearbox_config.json`, add:

```json
"port_stats_supported_sides": "line"
```

This enables line-side port statistics and prevents registration of the
system-side port for that group. Both hardware port objects and their connector
are still created. The main ASIC's port counters and other counter groups,
including MACsec, keep their existing behavior.

The field takes one string: `"line"`, `"system"`, or `"both"`. An absent field
defaults to `"both"`, preserving registration of both sides. Arrays, empty
strings, comma-separated lists, and other values are rejected.

gearsyncd publishes the same string in the PHY record in APPL_DB.
`GearboxUtils::loadPhyMap()` validates that representation too, and defaults
to `"both"` when reading an older record without the field. An invalid database
value raises an error instead of silently enabling a side.

When CONFIG_DB's `GEARBOX` table is populated, gearsyncd uses it in preference
to the JSON fallback. Configure the `phy:<id>` record there using a string:

```json
"port_stats_supported_sides": "line"
```

The allowed database strings are also `line`, `system`, and `both`.

PortsOrch loads the capability into its PHY cache during initialization.
Both initial counter enablement and initialization of a port after counters
are enabled use the same registration helper. The existing `counterpoll port`
enable/disable and interval controls continue to apply to the counter group.

This is startup configuration. Editing the JSON or the database while
orchagent is running does not refresh its cached capability or cancel existing
polling subscriptions. Apply the setting through a clean initialization of the
gearbox and counter state. Changing this setting during a warm restart is not
supported by this change; retained subscriptions must not be mistaken for the
new registration policy.
