---
name: erspan-span-implementation
description: >-
  Implement, test, debug, and maintain SPAN and ERSPAN (port-mirroring) features
  on SuperSONiC / Vega8 (Spectrum-4). Covers orchagent SAI programming, UCLI CLI,
  YANG models, gNMI integration, and on-box validation. Use when the user asks
  about mirror sessions, port mirroring, ERSPAN, SPAN, sampled mirroring,
  truncation, congestion mode, or any UPSW-1730..UPSW-1762 JIRA.
---

# ERSPAN / SPAN Implementation Skill

SPAN (local port mirror) and ERSPAN (encapsulated remote SPAN) on UpscaleAI
SuperSONiC, running on NVIDIA Spectrum-4 (Vega8) hardware with SONiC 202511.

## Repositories & File Map

| Repo | Key Files | What Changes |
|------|-----------|--------------|
| `sonic-swss` | `orchagent/mirrororch.{h,cpp}`, `orchagent/sfloworch.{h,cpp}` | SAI programming, session lifecycle, sampled mirroring |
| `ucli-dev` (submodule) | `scripts/mirror/*.py`, `command-tree/port_mirror{,_config}.xml` | CLI commands, CONFIG_DB writes |
| `caspian-sonic-buildimage` | `src/sonic-yang-models/yang-models/sonic-mirror-session.yang` | Schema (directly tracked, not submodule) |
| `sonic-mgmt-common` (submodule) | `models/yang/sonic/import.mk`, `cvl/testdata/schema/sonic-mirror-session.yang` | CVL validation, gNMI |
| `sonic-gnmi` (submodule) | `test/test_gnmi_configdb_patch.py` | gNMI patch tests |
| `engineering-notes` | `super-sonic/erspan/*.md` | Implementation status, gap analysis |

Working directories:

```text
~/caspian-sonic-buildimage/      # Main build repo (contains submodules below)
├── src/sonic-swss/              # orchagent (submodule, PR target: upscale-ai-network/sonic-swss)
├── ucli/                        # UCLI (submodule, PR target: upscale-ai-network/ucli)
├── src/sonic-mgmt-common/       # gNMI/CVL (submodule)
├── src/sonic-gnmi/              # gNMI server (submodule)
├── src/sonic-yang-models/       # YANG models (directly tracked, NOT a submodule)
~/sonic-swss/                    # Standalone checkout for dev (mirrors submodule)
~/ucli-dev/                      # Standalone checkout for dev (mirrors submodule)
~/sonic-mgmt-common/             # Standalone checkout for dev
~/sonic-gnmi/                    # Standalone checkout for dev
~/engineering-notes/             # Documentation
```

**Build workflow:** Changes are developed in standalone repos, pushed to their
respective upstream branches, then the submodule SHAs are bumped in
`caspian-sonic-buildimage` before building the full SONiC image.

---

## JIRA Requirements

**Story:** [UPSW-1730](https://bugatti-asic.atlassian.net/browse/UPSW-1730)

### SPAN (UPSW-1731 — UPSW-1746)

| JIRA | Requirement | Status | Test Command |
|------|-------------|--------|--------------|
| UPSW-1731 | Local SPAN sessions with IDs 0–7 | Done | `port-mirror session 1 span source Ethernet0` |
| UPSW-1732 | Ingress/egress direction | Done | `port-mirror session 1 span direction ingress` |
| UPSW-1733 | Single/ranged source ports incl. bonds | Done | `port-mirror session 1 span source Ethernet0-Ethernet16` |
| UPSW-1734 | Local destination (mirror-to-port) | Done | `port-mirror session 1 span destination Ethernet16` |
| UPSW-1735 | Truncation (byte-aligned) | Done | `port-mirror session 1 span truncate 128` |
| UPSW-1736 | Delete all or individual | Done | `no port-mirror session 1` / `no port-mirror` |
| UPSW-1737 | Selective mirroring via ACL | Done | ACL `action span destination Ethernet52` |
| UPSW-1738 | ACL ingress/egress attachment | Done | `ip access-group MIRROR_ACL in` |
| UPSW-1739 | Mirroring to CPU | Done | `port-mirror session 1 span destination CPU` |
| UPSW-1740 | CPU SPAN rate-limit / policer | Done | `port-mirror session 1 span policer mirror_policer_1` |
| UPSW-1741 | Prevent recursive mirroring | Done | dst in src list → rejected |
| UPSW-1742 | Enforce ASIC mirror limits (max 8) | Done | 9th session → rejected |
| UPSW-1743 | Many-to-one only | Done | same source in 2 sessions → rejected |
| UPSW-1744 | Expose operational state | Done | `show port-mirror` |
| UPSW-1745 | Expose hardware ACL programming | Done | `show port-mirror hardware` |
| UPSW-1746 | Congestion warning | Done | `port-mirror session 1 span congestion-mode correlated` |

### ERSPAN (UPSW-1747 — UPSW-1762)

| JIRA | Requirement | Status | Test Command |
|------|-------------|--------|--------------|
| UPSW-1747 | ERSPAN with GRE encapsulation | Done | `port-mirror session 2 erspan source-ip 10.1.0.32 dest-ip 192.168.240.25` |
| UPSW-1748 | Ingress/egress directions | Done | `port-mirror session 2 erspan direction ingress` |
| UPSW-1749 | Configurable source IP | Done | `port-mirror session 2 erspan source-ip 10.1.0.32` |
| UPSW-1750 | Configurable destination IP | Done | `port-mirror session 2 erspan dest-ip 192.168.240.25` |
| UPSW-1751 | Frame truncation | Done | `port-mirror session 2 erspan truncate 256` |
| UPSW-1752 | Delete all or individual | Done | `no port-mirror session 2` |
| UPSW-1753 | Selective mirroring via ACL | Done | ACL `action erspan source-ip 10.1.0.32 dest-ip 192.168.240.25` |
| UPSW-1754 | IP+GRE+L2 encap; ip.proto==47 | Done | Verify ASIC_DB GRE attrs |
| UPSW-1755 | Validate destination reachability | Done | unreachable dst_ip → advisory warning |
| UPSW-1756 | Reject IPv6 if unsupported | N/A | SAI supports IPv6 natively |
| UPSW-1757 | ARP/neighbor resolution | Done | session inactive until neighbor resolves |
| UPSW-1758 | Prevent recursive loops | Done | src_ip == dst_ip → rejected |
| UPSW-1759 | Enforce ASIC limits | Done | shared max 8 with SPAN |
| UPSW-1760 | Operational state + counters | Done | `show port-mirror counters` (HW counters N/A on Spectrum-4) |
| UPSW-1761 | MLAG redirect2Enable | Skipped | MLAG not supported on platform |
| UPSW-1762 | Document best-effort delivery | Done | SWSS_LOG_NOTICE on activation |

### Sampled Mirroring Extensions (Reviewer-Requested, No JIRA)

| Requirement | Status | Test Command |
|-------------|--------|--------------|
| Sampled port mirroring (1-in-N) | Done | `port-mirror session 2 erspan sample-rate 1000` |
| ERSPAN-only + ingress-only restriction | Done | SPAN + sample-rate → rejected |
| ERSPAN Session ID (GRE header) | Done | `port-mirror session 2 erspan erspan-id 42` |
| In-place update for mutable fields | Done | change field on active session → no blackout |
| sFlow conflict detection | Done | sFlow + sampled mirror on same port → rejected |
| Capability discovery | Done | `redis-cli -n 6 HGETALL "SWITCH_CAPABILITY\|switch"` |

**Summary:** 38 requirements, 36 implemented, 1 skipped (MLAG), 1 N/A (IPv6).

---

## Architecture

### System Overview

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                      CONTROL PLANE (switch CPU)                        │
│                                                                        │
│  ┌──────────── UCLI / Management ─────────────┐                       │
│  │  port-mirror session <ID> erspan|span ...   │                       │
│  │  ip access-list ... action span/erspan      │                       │
│  │  show port-mirror [--detail|counters|hw]    │                       │
│  └──────────────────────┬──────────────────────┘                       │
│                         │ CONFIG_DB writes                             │
│  ┌──────────────────────┴──────────────────────────────────────────┐   │
│  │                  SONiC Redis Database Layer                     │   │
│  │  CONFIG_DB            APPL_DB       STATE_DB        ASIC_DB    │   │
│  │  (MIRROR_SESSION,     (mirroring)   (MIRROR_SESSION (SAI       │   │
│  │   ACL_TABLE,                         _TABLE, ACL,   objects)   │   │
│  │   ACL_RULE)                          SWITCH_CAP)               │   │
│  └──────────────────────┬──────────────────────────────────────────┘   │
│                         │                                              │
│  ┌──────────────────────┴──────────────────────────────────────────┐   │
│  │                     orchagent (swss)                            │   │
│  │  MirrorOrch ←→ PortsOrch ←→ AclOrch ←→ RouteOrch / NeighOrch │   │
│  │      │             │            │                              │   │
│  │      └─────────────┴────────────┴──────────────────────────────│   │
│  │                        SAI Mirror + SAI ACL APIs               │   │
│  └────────────────────────────┬───────────────────────────────────┘   │
├───────────────────────────────┼───────────────────────────────────────┤
│                         DATA PLANE                                    │
│  ┌────────────────────────────┴───────────────────────────────────┐   │
│  │                  Spectrum-4 ASIC (Vega8)                       │   │
│  │  · Ingress/egress mirror sources                               │   │
│  │  · Local SPAN to monitor port                                  │   │
│  │  · ERSPAN encapsulation (IP + GRE + mirrored L2)              │   │
│  │  · ACL-triggered mirror / CPU mirror (policed)                │   │
│  │  · Truncation (byte-aligned)                                   │   │
│  │  · Congestion mode (independent / correlated)                  │   │
│  │  · Sampled mirroring (1-in-N, ingress only)                   │   │
│  │  · Up to 8 mirror sessions (SPAN + ERSPAN combined)           │   │
│  └────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

**Data flow — SPAN (local mirror):**
1. UCLI writes `MIRROR_SESSION|<name>` to CONFIG_DB with `type=SPAN`, `src_port`, `dst_port`, `direction`, optional `truncate_size`, `policer`, `congestion_mode`.
2. `MirrorOrch` subscribes, validates port state via `PortsOrch`, creates SAI mirror session (`sai_mirror_api->create_mirror_session`) with `SAI_MIRROR_SESSION_TYPE_LOCAL`.
3. Spectrum-4 ASIC copies ingress/egress frames from source port(s) to destination monitor port.

**Data flow — ERSPAN (remote GRE mirror):**
1. UCLI writes `MIRROR_SESSION|<name>` with `type=ERSPAN`, `src_ip`, `dst_ip`, `direction`, optional `erspan_id`, `truncate_size`, `sample_rate`, `policer`, `congestion_mode`, `monitor_port`, `dst_mac`.
2. **Resolved path** (no `monitor_port`): `MirrorOrch` waits for `RouteOrch`/`NeighOrch` to resolve `dst_ip` → next-hop → MAC. Session goes inactive if route/neighbor unreachable; reactivates on resolution.
3. **Direct path** (`monitor_port` set): `MirrorOrch` resolves the port OID immediately, uses configured `dst_mac` (or one-shot ARP lookup), and calls `activateSession()` in `createEntry()`. No Observer subscription — immune to route/ARP flaps.
4. Creates SAI mirror session (`SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE`) with GRE encapsulation attributes.

**Data flow — ACL-based selective mirror:**
1. UCLI writes ACL rule with `MIRROR_INGRESS_ACTION` or `MIRROR_EGRESS_ACTION` referencing a mirror session name.
2. `AclOrch` binds ACL entry to mirror session OID from `MirrorOrch`.
3. Only traffic matching ACL is mirrored (not all traffic on the port).

### Data Flow (Simplified)

```text
UCLI CLI  →  CONFIG_DB (MIRROR_SESSION table)
                    ↓  Redis notification (SET/DEL)
              orchagent / MirrorOrch
                    ↓
              createEntry() → activateSession() → SAI mirror API → ASIC
                    ↓
              STATE_DB (session status: active/inactive)
                    ↓
              show port-mirror (reads CONFIG_DB + STATE_DB)
```

### Session Types

| Type | SAI Type | Key Attributes |
|------|----------|----------------|
| SPAN | `SAI_MIRROR_SESSION_TYPE_LOCAL` | `dst_port`, `src_port`, `direction`, `truncate_size`, `congestion_mode` |
| ERSPAN | `SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE` | `src_ip`, `dst_ip`, `gre_type`, `dscp`, `ttl` + all SPAN fields + `erspan_id`, `sample_rate` |

### CONFIG_DB Schema (MIRROR_SESSION Table)

Key: `MIRROR_SESSION|<session-name>` (numeric IDs: "1", "2", etc.)

| Field | Type | SPAN | ERSPAN | Description |
|-------|------|------|--------|-------------|
| `type` | `SPAN\|ERSPAN` | Req | Req | Session type |
| `src_port` | string | Opt | Opt | Comma-separated source ports |
| `dst_port` | string | Req | — | Monitor port or `CPU` |
| `direction` | `RX\|TX\|BOTH` | Opt | Opt | Default: BOTH |
| `src_ip` | IP | — | Req | Outer header source IP |
| `dst_ip` | IP | — | Req | Outer header dest IP |
| `monitor_port` | string | — | Opt | Explicit egress port (direct path, bypasses route resolution) |
| `dst_mac` | MAC | — | Opt | Explicit next-hop MAC (used with monitor_port) |
| `gre_type` | hex | — | Opt | GRE type (default: 0x88be Mellanox) |
| `dscp` | 0–63 | — | Opt | Outer DSCP |
| `ttl` | 0–255 | — | Opt | Outer TTL (default: 255) |
| `queue` | uint8 | — | Opt | Traffic class |
| `policer` | string | Opt | Opt | Rate limiter |
| `truncate_size` | 0,64–9236 | Opt | Opt | Frame truncation (0=disabled) |
| `congestion_mode` | `independent\|correlated` | Opt | Opt | Default: independent |
| `erspan_id` | 0–1023 | — | Opt | GRE session ID |
| `sample_rate` | 0,1–16777215 | — | Opt | 1-in-N sampling (0=full) |

### Mutable vs Immutable Fields (updateEntry)

| Field | Mutability | Update Behavior |
|-------|-----------|-----------------|
| `truncate_size` | CREATE_AND_SET | In-place `set_mirror_session_attribute()` |
| `erspan_id` | CREATE_AND_SET | In-place (capability-gated) |
| `congestion_mode` | CREATE_AND_SET | In-place |
| `sample_rate` | Mutable* | SAMPLEPACKET teardown+recreate (rate is CREATE_ONLY on SAMPLEPACKET) |
| All others | Immutable | Full session delete + recreate |

### MirrorOrch Lifecycle

**createEntry():** Parses CONFIG_DB fields, validates ranges/types. Creates a counter
object (lives for session lifetime, survives activate/deactivate cycles). For SPAN →
`activateSession()` immediately. For ERSPAN with `monitor_port` (direct path) →
`activateSession()` immediately (no Observer registration). For ERSPAN without
`monitor_port` (resolved path) → subscribes to RouteOrch/NeighOrch; session stays
inactive until `dst_ip → route → ARP → MAC → FDB → egress port` all resolve.
If session already exists, delegates to `updateEntry()`.

**activateSession():** Programs SAI objects. SPAN sets `SAI_MIRROR_SESSION_TYPE_LOCAL`
with monitor port. ERSPAN sets `SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE` with
GRE/IP/MAC headers, VLAN attrs if needed, ERSPAN_SESSION_ID (capability-gated).
Both set TRUNCATE_SIZE, CONGESTION_MODE, POLICER if configured. For sampled
mirroring, creates `SAI_OBJECT_TYPE_SAMPLEPACKET` and binds via
`SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION`.

**updateEntry():** Classifies each changed field as mutable or immutable. If any
immutable field changed → `deleteEntry()` + `createEntry()`. Otherwise applies
mutable updates in-place. Mode transitions (full ↔ sampled) always require
teardown. `erspan_id` and `sample_rate` are rejected for SPAN sessions.

**Observer pattern:** MirrorOrch registers as Observer for NeighOrch, RouteOrch,
FdbOrch, PortsOrch. When neighbors/routes change, ERSPAN sessions are
re-evaluated. SPAN sessions and direct-path sessions (`monitor_port` set) are
explicitly skipped in `updateNeighbor()` and `updateNextHop()` (no L3 dependency).

### SAI Attributes Quick Reference

**Mirror session:** TYPE, MONITOR_PORT, TC, TRUNCATE_SIZE (C+S), CONGESTION_MODE
(C+S), POLICER, SRC_IP, DST_IP, SRC_MAC, DST_MAC, GRE_PROTOCOL_TYPE,
ERSPAN_ENCAPSULATION_TYPE, IPHDR_VERSION, TOS, TTL, ERSPAN_SESSION_ID (C+S),
VLAN_* (VLAN egress), COUNTER_ID (unsupported on Spectrum-4).

**Samplepacket:** SAMPLE_RATE (CREATE_ONLY), TYPE, MODE, TRUNCATE_SIZE.

**Port binding:** `INGRESS_MIRROR_SESSION` / `EGRESS_MIRROR_SESSION` (full),
`INGRESS_SAMPLE_MIRROR_SESSION` (sampled). sFlow uses `INGRESS_SAMPLEPACKET_ENABLE`
(conflicts with sampled mirror).

### UCLI Data Layer

`scripts/mirror/_db.py` — `MirrorDb` class uses `SessionConfigDBConnector` via
`redis_factory`. `_set_field()` uses HSET (merge) for in-place updates — orchagent
handles field changes without session teardown.

Constants in `_constants.py`: TRUNCATE_MIN=64, ERSPAN_ID_MAX=1023,
SAMPLE_RATE_MAX=16777215, MIRROR_MAX_SESSIONS=8, FIELD_MONITOR_PORT, FIELD_DST_MAC.

---

## Configuration Commands (UCLI)

### ERSPAN

```text
port-mirror session <ID> erspan source <port-list>
port-mirror session <ID> erspan source-ip <IP>
port-mirror session <ID> erspan dest-ip <IP>
port-mirror session <ID> erspan direction ingress|egress|both
port-mirror session <ID> erspan monitor-port <port>
port-mirror session <ID> erspan dest-mac <MAC>
port-mirror session <ID> erspan truncate <64-9236>
port-mirror session <ID> erspan erspan-id <0-1023>
port-mirror session <ID> erspan sample-rate <1-16777215>
port-mirror session <ID> erspan congestion-mode independent|correlated
port-mirror session <ID> erspan policer <name>
```

### SPAN

```text
port-mirror session <ID> span source <port-list>
port-mirror session <ID> span destination <port|CPU>
port-mirror session <ID> span direction ingress|egress|both
port-mirror session <ID> span truncate <64-9236>
port-mirror session <ID> span congestion-mode independent|correlated
port-mirror session <ID> span policer <name>
```

### ACL Mirror Actions

```text
ip access-list <ACL_NAME>
  <SEQ> permit ip any any
  action erspan source-ip <SRC> dest-ip <DST> [direction ingress]
  action span destination <PORT>
  action span cpu
!
interface Ethernet<N>
  ip access-group <ACL_NAME> in|out
```

### Delete / Unset / Show

```text
no port-mirror session <ID>
no port-mirror
no port-mirror session <ID> erspan|span <field>
show port-mirror [--detail] [<session-id>]
show port-mirror counters
show port-mirror hardware
```

---

## Debugging

### Quick Commands

```bash
# Session status
ucli -c "show port-mirror"

# CONFIG_DB
redis-cli -n 4 HGETALL "MIRROR_SESSION|<id>"

# STATE_DB (oper status)
redis-cli -n 6 HGETALL "MIRROR_SESSION_TABLE|<id>"

# ASIC_DB (SAI objects)
redis-cli -n 1 KEYS "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION:*"
redis-cli -n 1 HGETALL "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION:<oid>"

# Samplepacket objects
redis-cli -n 1 KEYS "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET:*"

# Orchagent logs
show logging | grep -i "mirror\|MirrorOrch"
sudo docker exec swss cat /var/log/swss/swss.rec | grep -i mirror

# SAI-level logs
sudo docker exec syncd cat /var/log/swss/sairedis.rec | grep -i mirror

# Platform capabilities
redis-cli -n 6 HGETALL "SWITCH_CAPABILITY|switch" | grep -iE "mirror|sample|truncat"
```

### Common Issues

**ERSPAN session stuck inactive:** Resolution chain incomplete. Check: route
exists for dst_ip? ARP resolved? (`ip neigh show | grep <nexthop>`). For testing,
manually add: `ip neigh add <ip> lladdr <mac> dev <intf>`.

**SPAN deactivated by neighbor event:** `updateNeighbor()`/`updateNextHop()` must
skip SPAN sessions. Verify the `MIRROR_SESSION_SPAN` continue guard is present.

**Sampled mirror not working:** Check direction is `RX`, type is `ERSPAN`, no
sFlow on same port, SAMPLEPACKET exists in ASIC_DB, port bound via
`INGRESS_SAMPLE_MIRROR_SESSION` (not `INGRESS_MIRROR_SESSION`).

**Truncation not applied:** Check 4-byte alignment, minimum (38 IPv4 / 58 IPv6 /
20 SPAN). For sampled mirror, truncation uses `SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE`
(separate from `SAI_MIRROR_SESSION_ATTR_TRUNCATE_SIZE`).

**Max sessions reached:** Max 8 shared between SPAN + ERSPAN. Delete unused
sessions first.

### End-to-End State Verification

```bash
# 1. CLI → 2. CONFIG_DB → 3. STATE_DB → 4. ASIC_DB → 5. Port binding
ucli -c "show port-mirror --detail"
redis-cli -n 4 HGETALL "MIRROR_SESSION|<id>"
redis-cli -n 6 HGETALL "MIRROR_SESSION_TABLE|<id>"
redis-cli -n 1 HGETALL "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION:<oid>"
redis-cli -n 2 HGET "COUNTERS_PORT_NAME_MAP" "Ethernet0"  # get port OID
redis-cli -n 1 HGETALL "ASIC_STATE:SAI_OBJECT_TYPE_PORT:<oid>" | grep -i mirror
```

---

## Testing

### Unit Tests

```bash
# SWSS mock tests (inside docker)
cd /sonic/src/sonic-swss && make check TESTS='tests/mock_tests/mirrororch_ut'

# UCLI tests (from caspian root)
python3 -m pytest ucli/scripts/mirror/tests/ -v --tb=short

# YANG model tests (inside docker with libyang)
cd /sonic/src/sonic-yang-models && python3 setup.py build && python3 -m pytest tests/ -v -k mirror

# gNMI patch tests
cd /sonic/src/sonic-gnmi && python3 -m pytest test/test_gnmi_configdb_patch.py -v -k mirror
```

### On-Box Validation (T0 Topology: spine + fanout + server)

**SPAN basic:**

```bash
ucli -c "port-mirror session 1 span source Ethernet0"
ucli -c "port-mirror session 1 span destination Ethernet16"
ucli -c "port-mirror session 1 span direction ingress"
ucli -c "show port-mirror"        # verify active
redis-cli -n 1 KEYS "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION:*"  # verify SAI
ucli -c "no port-mirror session 1"
```

**ERSPAN basic:**

```bash
ucli -c "port-mirror session 2 erspan source Ethernet0"
ucli -c "port-mirror session 2 erspan source-ip 10.1.0.32"
ucli -c "port-mirror session 2 erspan dest-ip 192.168.240.25"
ucli -c "port-mirror session 2 erspan direction ingress"
ucli -c "show port-mirror"        # may be inactive until ARP resolves
ip neigh add 192.168.240.25 lladdr 00:11:22:33:44:55 dev Ethernet48
ucli -c "show port-mirror"        # now active
ucli -c "no port-mirror session 2"
```

**Truncation (in-place update):**

```bash
ucli -c "port-mirror session 1 span source Ethernet0"
ucli -c "port-mirror session 1 span destination Ethernet16"
ucli -c "port-mirror session 1 span truncate 128"
ucli -c "port-mirror session 1 span truncate 256"   # in-place, no teardown
show logging | grep -i truncate                       # verify no deactivate log
ucli -c "no port-mirror session 1"
```

**Sampled mirroring:**

```bash
ucli -c "port-mirror session 2 erspan source Ethernet0"
ucli -c "port-mirror session 2 erspan source-ip 10.1.0.32"
ucli -c "port-mirror session 2 erspan dest-ip 192.168.240.25"
ucli -c "port-mirror session 2 erspan direction ingress"
ucli -c "port-mirror session 2 erspan sample-rate 1000"
redis-cli -n 1 KEYS "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET:*"  # verify
ucli -c "port-mirror session 1 span source Ethernet4"
ucli -c "port-mirror session 1 span destination Ethernet20"
ucli -c "port-mirror session 1 span sample-rate 100"  # MUST fail (SPAN)
ucli -c "no port-mirror"
```

**Negative tests:**

```bash
# 9th session fails
for i in $(seq 1 9); do
  ucli -c "port-mirror session $i span source Ethernet$((i*4))"
  ucli -c "port-mirror session $i span destination Ethernet$((i*4+2))"
done
ucli -c "no port-mirror"

# Recursive mirror
ucli -c "port-mirror session 1 span source Ethernet0"
ucli -c "port-mirror session 1 span destination Ethernet0"  # fails

# ERSPAN loop
ucli -c "port-mirror session 2 erspan source-ip 10.1.0.1"
ucli -c "port-mirror session 2 erspan dest-ip 10.1.0.1"    # fails
```

**SPAN stability under neighbor events:**

```bash
ucli -c "port-mirror session 1 span source Ethernet0"
ucli -c "port-mirror session 1 span destination Ethernet16"
ip neigh add 10.0.0.100 lladdr aa:bb:cc:dd:ee:ff dev Ethernet48
ip neigh del 10.0.0.100 dev Ethernet48
ucli -c "show port-mirror"   # session 1 still active
show logging | grep -i "mirror.*session.*1"  # no deactivate
ucli -c "no port-mirror session 1"
```

### Test Log Collection

```bash
show logging | grep -i "mirror\|MirrorOrch\|SAMPLEPACKET" > /tmp/mirror_syslog.log
sudo docker exec swss cat /var/log/swss/swss.rec | grep -i mirror > /tmp/mirror_swss.log
redis-cli -n 1 KEYS "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION:*" > /tmp/mirror_asic.log
redis-cli -n 6 HGETALL "SWITCH_CAPABILITY|switch" | grep -i mirror > /tmp/mirror_caps.log
```

### On-Box Test Log Format (MANDATORY for PRs and Jira)

When attaching on-box testing evidence to PRs or Jira tickets, **always include
full raw command outputs** — never summaries, paraphrased results, or scripted
test-runner logs. The reader must see exactly what was typed and what the switch
returned.

**Required per test:**

1. The raw command as typed
2. The **verbatim, unedited output** returned by the switch
3. Verification commands with full output (CONFIG_DB, STATE_DB, ASIC_DB, show)

**Example of correct log format:**

```text
==========================================
TEST: UPSW-1735 — port-mirror session 1 span truncate 128
==========================================

$ ucli -c "port-mirror session 1 span truncate 128"

$ redis-cli -n 4 HGET "MIRROR_SESSION|1" "truncate_size"
128

$ redis-cli -n 6 HGETALL "MIRROR_SESSION_TABLE|1"
1) "status"
2) "active"
3) "monitor_port"
4) "Ethernet16"

$ ucli -c "show port-mirror --detail"
Session  Type  Status  Direction  Source       Destination  Truncate  Congestion
-------  ----  ------  ---------  ------       -----------  --------  ----------
1        SPAN  active  ingress    Ethernet0    Ethernet16   128       independent

$ ucli -c "no port-mirror session 1"
```

**What NOT to do:**

- Do NOT summarize outputs (e.g., "CONFIG_DB updated to 128" without the redis-cli output)
- Do NOT use `[PASS]`/`[FAIL]` annotations without the underlying raw output
- Do NOT truncate outputs unless extremely long (>100 lines)
- Do NOT paraphrase STATE_DB/ASIC_DB entries — paste the full block

**Required sections for a complete test log:**

1. **Baseline** — `redis-cli KEYS`, `show port-mirror`, before any changes
2. **Each config change** — UCLI command, then all four verification layers:
   - CONFIG_DB via `redis-cli -n 4`
   - STATE_DB via `redis-cli -n 6`
   - `show port-mirror --detail`
   - `show running-config` (verify setting survives `config save`/`config reload`)
3. **ERSPAN encap verification** — `tcpdump -nn 'ip proto 47'` on collector for GRE
4. **Validation rejection** — each guard must show the error message
5. **Delete / revert** — `no port-mirror session <id>`, verify cleanup
6. **Summary table** — after all raw logs, a table mapping UPSW ticket to result

---

## Building

### Full SONiC Image (primary workflow)

All builds happen from `caspian-sonic-buildimage`. Submodule SHAs must be bumped
first so the build picks up the latest code.

```bash
cd ~/caspian-sonic-buildimage

# 1. Update submodule SHAs to latest branches
cd src/sonic-swss && git fetch origin && git checkout <branch> && cd ../..
cd ucli && git fetch origin && git checkout <branch> && cd ..
# Commit the submodule pointer updates
git add src/sonic-swss ucli
git commit -m "chore: bump sonic-swss and ucli submodules"

# 2. Build the full image
make init
make configure PLATFORM=mellanox
make target/sonic-mellanox.bin

# 3. Load on switch
# SCP the image to switch, then: sudo sonic-installer install <image>.bin
```

### SWSS-only Build (inside docker container)

For faster iteration on orchagent changes without a full image rebuild:

```bash
# Enter the SWSS build container
docker exec -it <swss-build-container> bash

cd /sonic/src/sonic-swss
./autogen.sh && ./configure --with-mock-tests
make -j$(nproc)
make check   # runs mock tests including mirrororch_ut
```

### UCLI-only Deployment (no build needed)

UCLI is Python + XML — copy directly to the switch without building:

```bash
cd ~/caspian-sonic-buildimage
tar czf /tmp/ucli-test.tar.gz --exclude='.git' --exclude='__pycache__' ucli/
scp /tmp/ucli-test.tar.gz admin@spine7:~/
ssh admin@spine7 'tar xzf ucli-test.tar.gz && \
  sudo docker cp ~/ucli/scripts/. mgmt-framework:/usr/sbin/cli/scripts/ && \
  sudo docker cp ~/ucli/command-tree/. mgmt-framework:/usr/sbin/ucli/command-tree/ && \
  sudo docker cp ~/ucli/startup.xml mgmt-framework:/usr/sbin/ucli/command-tree/startup.xml'
```

---

## Platform Constraints (Spectrum-4)

| Constraint | Value |
|------------|-------|
| Max mirror sessions (SPAN + ERSPAN combined) | 8 |
| Truncation alignment | 4-byte |
| Sampled mirroring direction | Ingress (RX) only |
| sFlow + sampled mirror on same port | Not allowed |
| HW mirror counters (COUNTER_ID) | Not supported by Mellanox SAI |
| IPv6 ERSPAN underlay | Supported |

## Known Gotchas

1. **SPAN stability** — `updateNeighbor()`/`updateNextHop()` must skip SPAN and direct_path sessions
2. **Direct path vs resolved path** — `monitor_port` triggers immediate activation; without it, session depends on RouteOrch/NeighOrch resolution
3. **Counter lifecycle** — counter object created once in `createEntry()`, attached/detached in activate/deactivate, destroyed in `deleteEntry()`. Survives route/ARP flaps.
4. **Truncation minimum** — IPv4: 38B, IPv6: 58B, SPAN: 20B. UCLI floor: 64B
5. **sample_rate CREATE_ONLY on SAMPLEPACKET** — rate change requires SAMPLEPACKET teardown+recreate; mirror session stays
6. **Mode transition (full ↔ sampled)** — requires full session teardown (port binding changes)
7. **ERSPAN ID capability-gated** — stored if unsupported, not programmed to SAI
8. **UCLI runs inside mgmt-framework container** — `docker cp` for deployment, not host paths
9. **monitor_port loop prevention** — `monitor_port` must not appear in `src_port` list (same as SPAN dst_port check)
10. **dst_mac without monitor_port** — only meaningful with monitor_port; CLI warns if set without it
11. **HW mirror counters** — `SAI_MIRROR_SESSION_ATTR_COUNTER_ID` not supported on Spectrum-4; code handles gracefully with WARN

## Safety Rails

- Never modify `sonic-swss` or `ucli-dev` without running unit tests first
- `erspan_id` and `sample_rate` must be rejected for SPAN sessions (type guard)
- Truncation must work for both full and sampled mirror (separate SAI attributes)
- Always test with both IPv4 and IPv6 ERSPAN
- Check `SWITCH_CAPABILITY` before assuming SAI accepts an attribute

## PR Reviewers

| Repo | Reviewers |
|------|-----------|
| `sonic-swss` | `ramanan-net` |
| `ucli-dev` | `vpallipadi-us`, `thongal-upscale`, `skesa-k`, `mkim-upscaleai`, `skthodupunoori-upscaleai` |
| `engineering-notes` | `skthodupunoori-upscaleai` |
