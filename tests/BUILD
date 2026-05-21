load("//:swss_helpers.bzl", "add_debug_to_copts")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "tests",
    srcs = [
        "blackhole_counters_ut.cpp",
        "congestion_counters_ut.cpp",
        "quoted_ut.cpp",
        "request_parser_ut.cpp",
        "swssnet_ut.cpp",
    ],
    copts = add_debug_to_copts(),
    linkopts = [
        "-lpthread",
    ],
    deps = [
        "//cfgmgr:shellcmd",
        "//orchagent:orchagent_lib",
        "//orchagent:request_parser_lib",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-genl-3",
        "@local_saimetadata//:saimeta",
        "@local_swsscommon//:swsscommon",
    ],
)
