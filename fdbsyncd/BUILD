load("//:swss_helpers.bzl", "add_debug_to_copts", "add_gcovpreload_to_linkopts")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "fdbsyncd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = add_debug_to_copts(),
    linkopts = add_gcovpreload_to_linkopts(),
    deps = [
        "//warmrestart:warmrestartassist_lib",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_swsscommon//:swsscommon",
    ],
)
