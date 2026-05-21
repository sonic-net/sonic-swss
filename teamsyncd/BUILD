load("//:swss_helpers.bzl", "add_debug_to_copts")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "teamsyncd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = add_debug_to_copts(),
    deps = [
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_libteam//:libteam",
        "@local_swsscommon//:swsscommon",
    ],
)
