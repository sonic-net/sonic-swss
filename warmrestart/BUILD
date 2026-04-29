package(default_visibility = ["//visibility:public"])

cc_library(
    name = "warmrestartassist_lib",
    srcs = ["warmRestartAssist.cpp"],
    hdrs = ["warmRestartAssist.h"],
    copts = [
        "-Wno-error=reorder",
    ],
    deps = [
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_swsscommon//:swsscommon",
    ],
)

cc_library(
    name = "warmrestarthelper_lib",
    srcs = ["warmRestartHelper.cpp"],
    hdrs = ["warmRestartHelper.h"],
    copts = [
        "-Wno-error=reorder",
    ],
    deps = [
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_swsscommon//:swsscommon",
    ],
)
