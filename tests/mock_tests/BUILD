package(default_visibility = ["//visibility:public"])

cc_library(
    name = "fake_response_publisher_lib",
    srcs = ["fake_response_publisher.cpp"],
    deps = [
        "//orchagent:mock_response_publisher_lib",
        "//orchagent:orchagent_lib",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)

COMMON_COPTS = [
    "-Wno-sign-compare",
    "-fno-stack-protector",
] + select({
    "//:debug_mode": ["-ggdb -DDEBUG"],
    "//conditions:default": ["-g -DNDEBUG"],
})

COMMON_LINKOPTS = []

ALL_TEST_HDRS = glob(["*.h"])

cc_library(
    name = "common_test_lib",
    srcs = [
        "common/mock_shell_command.cpp",
        "fake_netlink.cpp",
        "fake_warmstarthelper.cpp",
        "mock_dbconnector.cpp",
        "mock_hiredis.cpp",
        "mock_orchagent_main.cpp",
        "mock_redisreply.cpp",
        "mock_table.cpp",
    ],
    hdrs = ALL_TEST_HDRS + [],
    copts = COMMON_COPTS,
    defines = [
        "GOOGLE_ONLY",
        "DRAIN_MMU_WHEN_APPLYING_BUFFER_CONFIG",
    ],
    linkopts = COMMON_LINKOPTS,
    deps = [
        "//cfgmgr:cfgmgr_base_lib",
        "//cfgmgr:shellcmd",
        "//cfgmgr:stateverificationlib",
        "//lib:lib_utils",
        "//orchagent:orchagent_lib",
        "//orchagent:orchagent_test_lib",
        "//warmrestart:warmrestarthelper_lib",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-genl-3",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_saivs//:saivs",
        "@local_swsscommon//:swsscommon",
        "@local_zmq//:zmq",
    ],
)

cc_test(
    name = "tests",
    srcs = [
        "aclorch_ut.cpp",
        "portsorch_ut.cpp",
        "routeorch_ut.cpp",
        "qosorch_ut.cpp",
        "bufferorch_ut.cpp",
        "buffermgrdyn_ut.cpp",
        "fdborch/flush_syncd_notif_ut.cpp",
        # todo(lmeninato): add "copp_ut.cpp"
        "copporch_ut.cpp",
        "saispy_ut.cpp",
        "consumer_ut.cpp",
        "sfloworh_ut.cpp",
        "shell_command_ut.cpp",
        "ut_saihelper.cpp",
        "mock_orchagent_main.cpp",
        "mock_dbconnector.cpp",
        "mock_consumerstatetable.cpp",
        "common/mock_shell_command.cpp",
        "mock_table.cpp",
        "mock_hiredis.cpp",
        "mock_redisreply.cpp",
        "mock_sai_port.cpp",
        "bulker_ut.cpp",
        "portmgr_ut.cpp",
        "sflowmgrd_ut.cpp",
        "swssnet_ut.cpp",
        "flowcounterrouteorch_ut.cpp",
        "orchdaemon_ut.cpp",
        "intfsorch_ut.cpp",
        "mux_rollback_ut.cpp",
        "warmrestartassist_ut.cpp",
        "test_failure_handling.cpp",
    ] + ALL_TEST_HDRS,
    copts = COMMON_COPTS,
    linkopts = COMMON_LINKOPTS,
    deps = [
        ":fake_response_publisher_lib",
        "//cfgmgr:cfgmgr_base_lib",
        "//cfgmgr:stateverificationlib",
        "//lib:lib_utils",
        "//orchagent:orchagent_test_lib",
        "//warmrestart:warmrestartassist_lib",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-genl-3",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_saimetadata//:saimeta",
        "@local_saimetadata//:saimetadata",
        "@local_saivs//:saivs",
        "@local_swsscommon//:swsscommon",
        "@local_zmq//:zmq",
    ],
)

# only works with --config=portsyncd_tests_opts
cc_test(
    name = "portsyncd_tests",
    srcs = [
        "portsyncd/portsyncd_ut.cpp",
    ],
    copts = COMMON_COPTS,
    linkopts = COMMON_LINKOPTS,
    deps = [
        ":common_test_lib",
        ":fake_response_publisher_lib",
        "//lib:lib_utils",
        "//portsyncd:portsyncd_lib",
    ],
)

cc_test(
    name = "intfmgrd_tests",
    srcs = [
        "intfmgrd/intfmgr_ut.cpp",
    ] + ALL_TEST_HDRS,
    copts = COMMON_COPTS,
    linkopts = COMMON_LINKOPTS,
    deps = [
        ":common_test_lib",
        ":fake_response_publisher_lib",
    ],
)

# todo(lmeninato): only works in kokoro
cc_test(
    name = "teammgrd_tests",
    srcs = [
        "teammgrd/teammgr_ut.cpp",
    ] + ALL_TEST_HDRS,
    copts = COMMON_COPTS,
    linkopts = COMMON_LINKOPTS,
    deps = [
        ":common_test_lib",
        ":fake_response_publisher_lib",
    ],
)

cc_test(
    name = "fpmsyncd_tests",
    srcs = [
        "fpmsyncd/test_fpmlink.cpp",
        "fpmsyncd/test_routesync.cpp",
    ] + ALL_TEST_HDRS,
    copts = COMMON_COPTS,
    linkopts = COMMON_LINKOPTS,
    deps = [
        ":common_test_lib",
        ":fake_response_publisher_lib",
        "//fpmsyncd:fpmsyncd_lib",
    ],
)

cc_test(
    name = "response_publisher_tests",
    srcs = [
        "response_publisher/response_publisher_ut.cpp",
    ] + ALL_TEST_HDRS,
    copts = COMMON_COPTS,
    linkopts = COMMON_LINKOPTS,
    deps = [
        ":common_test_lib",
        "//orchagent:response_publisher_lib",
        "@local_saimetadata//:saimeta",
        "@local_saimetadata//:saimetadata",
    ],
)
