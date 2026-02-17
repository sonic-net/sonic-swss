# Copilot Instructions for sonic-swss

## Project Overview

sonic-swss (Switch State Service) is the core data-plane programming service in SONiC. It provides a database interface for communication with and state representation of network applications and switch hardware. SWSS translates high-level network intent from CONFIG_DB into hardware-level SAI API calls, orchestrating routes, ACLs, interfaces, neighbors, and more.

## Architecture

```
sonic-swss/
├── orchagent/        # Main orchestration daemon — the heart of SWSS
│   ├── orch.cpp/h    # Base Orch class (all orchestrators inherit from this)
│   ├── routeorch.*   # Route orchestration
│   ├── portsorch.*   # Port orchestration
│   ├── aclorch.*     # ACL orchestration
│   ├── neighorch.*   # Neighbor orchestration
│   └── ...           # Many more orchestrators
├── cfgmgr/           # Configuration managers (bridge CONFIG_DB → APP_DB)
├── fpmsyncd/         # FPM (Forwarding Plane Manager) sync daemon — routes from zebra
├── neighsyncd/       # Neighbor sync daemon (ARP/NDP → APP_DB)
├── portsyncd/        # Port state sync daemon
├── fdbsyncd/         # FDB (MAC table) sync daemon
├── natsyncd/         # NAT sync daemon
├── mclagsyncd/       # MC-LAG sync daemon
├── teamsyncd/        # LAG/teamd sync daemon
├── swssconfig/       # SWSS configuration utilities
├── warmrestart/      # Warm restart support
├── tests/            # VS (virtual switch) integration tests and mock tests
│   ├── mock_tests/   # C++ unit tests using gmock
│   └── *.py          # Python pytest integration tests
├── debian/           # Debian packaging
├── crates/           # Rust components
└── lib/              # Shared libraries
```

### Key Concepts
- **Orchagent pattern**: Each network feature has an Orch class that subscribes to DB tables, processes changes, and calls SAI APIs
- **Producer/Consumer**: Components communicate via Redis DB tables using producer/consumer pattern
- **DB pipeline**: CONFIG_DB → cfgmgr → APP_DB → orchagent → SAI → ASIC_DB

## Language & Style

- **Primary language**: C++ (orchagent, cfgmgr), Python (tests, scripts)
- **C++ standard**: C++14/17
- **Indentation**: 4 spaces (C++ and Python)
- **Naming conventions**:
  - Classes: `PascalCase` (e.g., `RouteOrch`, `PortsOrch`)
  - Methods: `camelCase` (e.g., `doTask`, `addRoute`)
  - Variables: `camelCase` or `snake_case`
  - Constants/macros: `UPPER_CASE`
  - File names: lowercase (e.g., `routeorch.cpp`)
- **Header guards**: Use `#pragma once` or traditional guards
- **Braces**: Opening brace on same line for functions and control structures

## Build Instructions

```bash
# Install dependencies (see README for full list)
sudo apt install redis-server libhiredis0.14 libzmq5 libzmq3-dev \
  libboost-serialization1.74.0 libboost1.71-dev libtool autoconf automake \
  dh-exec nlohmann-json3-dev libgmock-dev

# Install SONiC dependencies from VS build artifacts or build yourself
# (libswsscommon, libsairedis, libsaivs, etc.)

# Build from source
./autogen.sh
./configure
make && sudo make install

# Build Debian package
./autogen.sh
fakeroot debian/rules binary
```

## Testing

### Mock Tests (C++ unit tests)
```bash
# Built as part of the Debian package build
# Located in tests/mock_tests/
# Use Google Test/Google Mock framework
```

### VS Integration Tests (Python)
```bash
# Run from tests/ directory
# Require VS (virtual switch) environment
cd tests
sudo pytest -v test_route.py
# Tests use pytest framework with custom fixtures for VS setup
```

### Test Structure
- `tests/mock_tests/` — C++ unit tests with gmock for orchagent components
- `tests/*.py` — Python integration tests running against VS platform
- Tests interact with Redis databases to simulate CONFIG_DB changes and verify ASIC_DB state

## PR Guidelines

- **Commit format**: `[component]: Description` (e.g., `[orchagent]: Add IPv6 route support`)
- **Signed-off-by**: REQUIRED on all commits (`git commit -s`)
- **CLA**: Sign Linux Foundation EasyCLA
- **Testing**: Include mock tests for new orchagent features; VS tests for integration
- **Single commit per PR**: Squash commits before merge
- **Reference issues**: Link related GitHub issues in PR description

## Common Patterns

### Adding a New Orchestrator
1. Create `newfeatureorch.cpp/.h` in `orchagent/`
2. Inherit from `Orch` base class
3. Implement `doTask(Consumer& consumer)` to process table entries
4. Register table subscriptions in constructor
5. Add to `orchdaemon.cpp` initialization
6. Add mock tests in `tests/mock_tests/`

### Database Tables
- **CONFIG_DB** → User/management configuration
- **APP_DB** → Application-level state (input to orchagent)
- **ASIC_DB** → SAI object representations (output of orchagent)
- **STATE_DB** → Operational state
- **COUNTERS_DB** → Statistics and counters

### Producer/Consumer Pattern
```cpp
// Producer side (e.g., cfgmgr)
ProducerStateTable producer(&db, APP_TABLE_NAME);
producer.set(key, fieldValues);

// Consumer side (orchagent)
void MyOrch::doTask(Consumer& consumer) {
    auto it = consumer.m_toSync.begin();
    // Process entries...
}
```

## Dependencies

- **sonic-swss-common**: Database access, netlink wrappers, common utilities
- **sonic-sairedis**: SAI Redis interface (communicates with syncd)
- **SAI headers**: Switch Abstraction Interface API definitions
- **Redis**: In-memory database for inter-process communication
- **libnl**: Netlink library for kernel communication
- **nlohmann/json**: JSON parsing

## Gotchas

- **SAI API changes**: When SAI headers change, orchagent code must be updated accordingly
- **Thread safety**: Orchagent is single-threaded; don't add blocking calls in doTask()
- **Warm restart**: New features should consider warm restart compatibility
- **Table dependencies**: Orch classes may depend on other Orchs being initialized first
- **Error handling**: Always check SAI return codes; use `task_process_status` pattern
- **Memory leaks**: Use RAII and smart pointers; avoid raw new/delete
- **VS testing**: VS platform doesn't support all SAI features — check vslib for coverage
- **Consumer sync**: Always consume all entries in doTask() or properly handle retry
