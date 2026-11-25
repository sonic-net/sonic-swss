"""Dependencies shared by SWSS targets.

The common set uses the SONiC and Debian modules from the reference
sonic-swss-common migration.
"""

_LIBNL_DEPS = [
    "@libnl3//:libnl_3",
    "@libnl3//:libnl_genl_3",
    "@libnl3//:libnl_nf_3",
    "@libnl3//:libnl_route_3",
]

_SWSS_BASE_DEPS = [
    "@sonic_swss_common//:libswsscommon_shared",
    "@trixie//libboost-dev:libboost",
    "@trixie//libhiredis-dev:libhiredis",
    "@trixie//nlohmann-json3-dev:nlohmann-json3",
]

SWSS_DEPS = _SWSS_BASE_DEPS + _LIBNL_DEPS

_CFGMGR_BASE_DEPS = _SWSS_BASE_DEPS + [
    "@sonic_sairedis//meta:saimeta_shared",
    "@trixie//libzmq3-dev:libzmq3",
]

CFGMGR_DEPS = _CFGMGR_BASE_DEPS + _LIBNL_DEPS

ORCHAGENT_DEPS = _CFGMGR_BASE_DEPS + [
    "@sonic_dash_api//:dashapi",
    "@sonic_sairedis//lib:sairedis_shared",
    "@swss_debian//libjemalloc-dev:libjemalloc",
    "@protobuf_legacy//:libprotobuf",
    "@swss_debian//libyaml-cpp-dev:libyaml-cpp",
] + _LIBNL_DEPS

# SONiC-patched libteam owns the headers, shared libraries and runtime TARs.
TEAM_DEPS = _SWSS_BASE_DEPS + [
    "@libteam//:libteam",
    "@libteam//:libteamdctl",
] + _LIBNL_DEPS

TLM_TEAM_DEPS = _SWSS_BASE_DEPS + [
    "@libteam//:libteamdctl",
    "@swss_debian//libjansson-dev:libjansson",
] + _LIBNL_DEPS
