load("//:swss_helpers.bzl", "add_debug_to_copts")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "restore_neighbors_py_src",
    srcs = ["restore_neighbors.py"],
)

filegroup(
    name = "neighsyncd_binary",
    srcs = ["neighsyncd"],
)

cc_binary(
    name = "neighsyncd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = [
        "-Wno-reorder",
    ] + add_debug_to_copts(),
    deps = [
        "//cfgmgr:shellcmd",
        "//lib:lib_utils",
        "//warmrestart:warmrestartassist_lib",
        "@local_hiredis//:hiredis",
        "@local_libnl3//:libnl-route-3",
        "@local_libnl3//:libnl3",
        "@local_swsscommon//:swsscommon",
    ],
)
