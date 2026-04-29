package(default_visibility = ["//visibility:public"])

# trick to export headers in a convenient way
cc_library(
    name = "swsscommon_headers",
    hdrs = glob([
        "bazel/swsscommon/swss_hdrs/*.h",
    ]),
    includes = [
        "bazel/swsscommon/swss_hdrs",
    ],
    strip_include_prefix = "bazel/swsscommon/swss_hdrs",
)

cc_import(
    name = "swsscommon",
    hdrs = [],  # see cc_library rule above
    shared_library = "bazel/swsscommon/swss_so/libswsscommon.so",
    deps = [
        ":swsscommon_headers"
    ]
)
