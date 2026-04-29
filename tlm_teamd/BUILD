load("//:swss_helpers.bzl", "add_debug_to_copts")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "tlm_teamd",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = add_debug_to_copts(),
    local_defines = [
        "GOOGLE_ONLY",
        "DRAIN_MMU_WHEN_APPLYING_BUFFER_CONFIG",
    ],
    deps = [
        "//lib:lib_utils",
        "@local_hiredis//:hiredis",
        "@local_jansson//:jansson",
        "@local_libteam//:libteamdctl",
        "@local_swsscommon//:swsscommon",
    ],
)
