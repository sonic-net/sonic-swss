load("//:swss_helpers.bzl", "add_debug_to_copts", "add_gcovpreload_to_linkopts")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "lib_utils_srcs",
    srcs = [
        "gearboxutils.cpp",
        "gearboxutils.h",
        "subintf.cpp",
        "subintf.h",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "lib_utils",
    srcs = [
        "gearboxutils.cpp",
        "subintf.cpp",
    ],
    hdrs = [
        "gearboxutils.h",
        "subintf.h",
    ],
    copts = [
        "-I/usr/include/sai",
    ] + add_debug_to_copts(),
    linkopts = add_gcovpreload_to_linkopts(),
    deps = [
        "@local_swsscommon//:swsscommon",
    ],
)

cc_library(
    name = "asan",
    srcs = ["asan.cpp"],
    copts = add_debug_to_copts(),
    linkopts = add_gcovpreload_to_linkopts(),
    deps = [
        "@local_swsscommon//:swsscommon",
    ],
)
