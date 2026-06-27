package(default_visibility = ["//visibility:public"])

DEBUG_COPTS = select({
    "//:debug_mode": ["-ggdb -DDEBUG"],
    "//conditions:default": ["-g -DNDEBUG"],
})

DEFAULT_COPTS = [] + DEBUG_COPTS

DEFAULT_LINKOPTS = ["-lpthread"]

cc_library(
    name = "shellcmd",
    srcs = ["shellcmd.h"],
    hdrs = ["shellcmd.h"],
    deps = [
        "@local_swsscommon//:swsscommon",
    ]
)

filegroup(
    name = "cfgmgr_state_verification_data",
    srcs = [
        "config_mgr_state_verification_map.json",
    ],
)

filegroup(
    name = "confg_mgr_data",
    srcs = glob(["*.json"]),
)

filegroup(
    name = "cfgmgr_lua_scripts",
    srcs = glob(["*.lua"]),
)

cc_library(
    name = "cfgmgr_lib",
    srcs = [
        "dynamicportbreakouthelper.cpp",
        "hostsignalingmgr.cpp",
        "portdpb.cpp",
    ],
    hdrs = [
        "dynamicportbreakouthelper.h",
        "hostsignalingmgr.h",
        "portdpb.h",
        "portmgr.h",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    local_defines = ["GOOGLE_ONLY"],
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "vlanmgrd",
    srcs = [
        "vlanmgr.cpp",
        "vlanmgr.h",
        "vlanmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "teammgrd",
    srcs = [
        "teammgr.cpp",
        "teammgr.h",
        "teammgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":cfgmgr_lib",
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "portmgrd",
    srcs = [
        "portmgr.cpp",
        "portmgr.h",
        "portmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    local_defines = ["GOOGLE_ONLY"],
    deps = [
        ":cfgmgr_lib",
        "//cfgmgr:shellcmd",
    ],
)

cc_binary(
    name = "intfmgrd",
    srcs = [
        "intfmgr.cpp",
        "intfmgr.h",
        "intfmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":cfgmgr_lib",
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "buffermgrd",
    srcs = [
        "buffermgr.cpp",
        "buffermgr.h",
        "buffermgrd.cpp",
        "buffermgrdyn.cpp",
        "buffermgrdyn.h",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "vrfmgrd",
    srcs = [
        "vrfmgr.cpp",
        "vrfmgr.h",
        "vrfmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "nbrmgrd",
    srcs = [
        "nbrmgr.cpp",
        "nbrmgr.h",
        "nbrmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "vxlanmgrd",
    srcs = [
        "vxlanmgr.cpp",
        "vxlanmgr.h",
        "vxlanmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "sflowmgrd",
    srcs = [
        "sflowmgr.cpp",
        "sflowmgr.h",
        "sflowmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":cfgmgr_lib",
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "coppmgrd",
    srcs = [
        "coppmgr.cpp",
        "coppmgr.h",
        "coppmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "switchmgrd",
    srcs = [
        "switchmgr.cpp",
        "switchmgr.h",
        "switchmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "tunnelmgrd",
    srcs = [
        "tunnelmgr.cpp",
        "tunnelmgr.h",
        "tunnelmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "bookkeepmgrd",
    srcs = [
        "bookkeepmgr.h",
        "bookkeepmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_library(
    name = "stateverificationmgrlib",
    srcs = [
        "stateverificationmgr.h",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib",
    ],
)

cc_library(
    name = "stateverificationlib",
    srcs = [
        "stateverificationhelper.cpp",
        "stateverificationhelper.h",
    ],
    copts = DEFAULT_COPTS,
    linkopts = ["-lpthread"],
    deps = [
        ":stateverificationmgrlib",
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib",
    ],
)

cc_binary(
    name = "stateverificationmgrd",
    srcs = [
        "stateverificationmgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":stateverificationlib",
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

cc_binary(
    name = "bridgemgrd",
    srcs = [
        "bridgemgr.cpp",
        "bridgemgr.h",
        "bridgemgrd.cpp",
    ],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    local_defines = ["GOOGLE_ONLY"],
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib_impl",
    ],
)

# -------------- Base lib to use in external targets -------------

cc_library(
    name = "cfgmgr_base_lib",
    srcs = [
        "buffermgrdyn.cpp",
        "buffermgrdyn.h",
        "coppmgr.cpp",
        "coppmgr.h",
        "dynamicportbreakouthelper.cpp",
        "dynamicportbreakouthelper.h",
        "hostsignalingmgr.cpp",
        "hostsignalingmgr.h",
        "intfmgr.cpp",
        "intfmgr.h",
        "portdpb.cpp",
        "portdpb.h",
        "portmgr.cpp",
        "portmgr.h",
        "sflowmgr.cpp",
        "sflowmgr.h",
        "teammgr.cpp",
        "teammgr.h",
    ],
    deps = [
        "//cfgmgr:shellcmd",
        "//cfgmgr:stateverificationlib",
        "//lib:lib_utils",
        "//orchagent:orchagent_lib",
        "//warmrestart:warmrestarthelper_lib",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-genl-3",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_saivs//:saivs",
        "@local_swsscommon//:swsscommon",
        "@local_zmq//:zmq",
    ],
)
