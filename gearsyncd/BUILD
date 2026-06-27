load("//:swss_helpers.bzl", "add_debug_to_copts")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "gearsyncd_binary",
    srcs = ["gearsyncd"],
)

cc_binary(
    name = "gearsyncd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = add_debug_to_copts(),
    deps = [
        "//cfgmgr:shellcmd",
        "//lib:lib_utils",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_swsscommon//:swsscommon",
    ],
)
