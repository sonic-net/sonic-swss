"""
Test scenarios and related constants for dualtor neighbor miss.

Each item in NEIGH_MISS_TESTS is a test case, comprising of a list of steps.
Each step is a dictionary containing the action to be performed during that
step, as well as the expected result.
The expected result itself is another dictionary, containing the following
attributes:
    - (bool) EXPECT_ROUTE: if we expect a route entry in ASIC_DB
    - (bool) EXPECT_NEIGH: if we expect a neighbor entry in ASIC_DB
    - (bool) REAL_MAC: If a real MAC address is expected in the
                       APPL_DB neighbor table entry, as opposed
                       to a zero/empty MAC

All expected result attributes will be verified agains the DVS
after each test step is executed

Note: EXPECT_ROUTE and EXPECT_NEIGH cannot both be True

Note: for the purposes of this test, there is a distinction made
      between 'server' IPs and 'neighbor' IPs. Server IPs are
      IP addresses explicitly configured on a specific mux cable
      interface in the MUX_CABLE table in config DB. Neighbor IPs
      are any other IPs within the VLAN subnet.


"""

__all__ = [
    'TEST_ACTION', 'EXPECTED_RESULT', 'ACTIVE', 'STANDBY', 'PING_SERV', 'PING_NEIGH',
    'RESOLVE_ENTRY', 'DELETE_ENTRY', 'EXPECT_ROUTE', 'EXPECT_NEIGH', 'REAL_MAC',
    'INTF', 'IP', 'MAC', 'NEIGH_MISS_TESTS'
]

TEST_ACTION = 'action'
EXPECTED_RESULT = 'result'

# Possible test actions
ACTIVE = 'active'          # Switch the test interface to active
STANDBY = 'standby'         # Switch the test interface to standby
PING_SERV = 'ping_serv'       # Ping the server mux cable IP, used to trigger a netlink fail message
PING_NEIGH = 'ping_neigh'      # Ping the neighbor IP (not configured on a specific mux cable port)
RESOLVE_ENTRY = 'resolve_entry'   # Resolve the test IP neighbor entry in the kernel
DELETE_ENTRY = 'delete_entry'    # Delete the test IP neighbor entry from the kernel

# Test expectations
EXPECT_ROUTE = 'expect_route'
EXPECT_NEIGH = 'expect_neigh'
REAL_MAC = 'real_mac'

INTF = 'intf'
IP = 'ip'
MAC = 'mac'

# Note: For most test cases below, after the neighbor entry is deleted, we must
# still set `REAL_MAC` to `True` in the expected result since a prior step in the
# test should have resolved the neighbor entry and confirmed that the APPL_DB
# neighbor entry contained a real MAC address. Thus, when we verify that APPL_DB
# no longer contains a neighbor table entry, we need to check for the real MAC.
# The exception to this is test cases where the neighbor entry is never resolved
# in the kernel. In that case, APPL_DB will never contain the real MAC address.

STANDBY_MUX_CABLE_TESTS = [
    [
        {
            TEST_ACTION: STANDBY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_SERV,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: ACTIVE,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: RESOLVE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: True, REAL_MAC: True}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: True}
        }
    ],
    [
        {
            TEST_ACTION: STANDBY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_SERV,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: RESOLVE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: True}
        },
        {
            TEST_ACTION: ACTIVE,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: True, REAL_MAC: True}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: True}
        }
    ],
    [
        {
            TEST_ACTION: STANDBY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_SERV,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        }
    ]
]

ACTIVE_MUX_CABLE_TESTS = [
    [
        {
            TEST_ACTION: ACTIVE,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_SERV,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: STANDBY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: RESOLVE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: True}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: True}
        }
    ],
    [
        {
            TEST_ACTION: ACTIVE,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_SERV,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: RESOLVE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: True, REAL_MAC: True}
        },
        {
            TEST_ACTION: STANDBY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: True}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: True}
        }
    ],
    [
        {
            TEST_ACTION: ACTIVE,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_SERV,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        }
    ]
]

NEIGH_IP_TESTS = [
    [
        {
            TEST_ACTION: STANDBY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_NEIGH,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: RESOLVE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: True}
        },
        {
            TEST_ACTION: ACTIVE,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: True, REAL_MAC: True}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: True}
        }
    ],
    [
        {
            TEST_ACTION: ACTIVE,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: PING_NEIGH,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: RESOLVE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: True, REAL_MAC: True}
        },
        {
            TEST_ACTION: STANDBY,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: True}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: True}
        }
    ],
    [
        {
            TEST_ACTION: PING_NEIGH,
            EXPECTED_RESULT: {EXPECT_ROUTE: True, EXPECT_NEIGH: False, REAL_MAC: False}
        },
        {
            TEST_ACTION: DELETE_ENTRY,
            EXPECTED_RESULT: {EXPECT_ROUTE: False, EXPECT_NEIGH: False, REAL_MAC: False}
        }
    ]

]

NEIGH_MISS_TESTS = ACTIVE_MUX_CABLE_TESTS + STANDBY_MUX_CABLE_TESTS + NEIGH_IP_TESTS
