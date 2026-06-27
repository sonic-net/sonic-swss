package(default_visibility = ["//visibility:public"])

cc_library(
    name = "libnl3_headers",
    hdrs = glob([
        "bazel/libnl3/libnl3_hdrs/**/*.h",
    ]),
    includes = [
        "bazel/libnl3/libnl3_hdrs",
    ],
    strip_include_prefix = "bazel/libnl3/libnl3_hdrs",
)

cc_import(
    name = "libnl3",
    hdrs = [],
    shared_library = "bazel/libnl3/libnl3_so/libnl-3.so",
    deps = [
        ":libnl3_headers"
    ]
)

cc_import(
    name = "libnl-route-3",
    hdrs = [],
    shared_library = "bazel/libnl3/libnl3_so/libnl-route-3.so",
    deps = [
        ":libnl3_headers"
    ]
)

cc_import(
    name = "libnl-genl-3",
    hdrs = [],
    shared_library = "bazel/libnl3/libnl3_so/libnl-genl-3.so",
    deps = [
        ":libnl3_headers",
    ]
)

cc_import(
    name = "libnl-nf-3",
    hdrs = [],
    shared_library = "bazel/libnl3/libnl3_so/libnl-nf-3.so",
    deps = [
        ":libnl3_headers"
    ]
)
