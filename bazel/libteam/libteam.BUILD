package(default_visibility = ["//visibility:public"])

cc_library(
    name = "libteam_headers",
    hdrs = glob([
        "bazel/libteam/libteam_hdrs/team*.h",
    ]),
    includes = [
        "bazel/libteam/libteam_hdrs",
    ],
    strip_include_prefix = "bazel/libteam/libteam_hdrs",
)

cc_import(
    name = "libteam",
    hdrs = [],
    shared_library = "bazel/libteam/libteam_so/libteam.so",
    deps = [
        ":libteam_headers"
    ]
)

cc_import(
    name = "libteamdctl",
    hdrs = [],
    shared_library = "bazel/libteam/libteam_so/libteamdctl.so",
    deps = [
        ":libteam_headers"
    ]
)
