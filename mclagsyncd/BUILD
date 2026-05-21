load("//:swss_helpers.bzl", "add_debug_to_copts")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "mclagsyncd_binary",
    srcs = ["mclagsyncd"],
)

cc_binary(
    name = "mclagsyncd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = add_debug_to_copts(),
    deps = [
        "//warmrestart:warmrestartassist_lib",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-nf-3",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_swsscommon//:swsscommon",
    ],
)
