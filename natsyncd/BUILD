load("//:swss_helpers.bzl", "add_debug_to_copts")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "natsyncd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = [
        "-Wno-reorder",
    ] + add_debug_to_copts(),
    deps = [
        "//warmrestart:warmrestartassist_lib",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-nf-3",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_swsscommon//:swsscommon",
    ],
)
