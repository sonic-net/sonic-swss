package(default_visibility = ["//visibility:public"])

#trick to export headers in a convenient way
cc_library(
    name = "sairedis_headers",
    hdrs = [
        "bazel/sairedis/sairedis_hdrs/sairedis.h"
    ],
    includes = [
        "bazel/sairedis/sairedis_hdrs",
    ],
    strip_include_prefix = "bazel/sairedis/sairedis_hdrs",
)

cc_import(
    name = "sairedis",
    hdrs = ["bazel/sairedis/sairedis_hdrs/sairedis.h"],  # see cc_library rule above
    shared_library = "bazel/sairedis/sairedis_so/libsairedis.so",
    deps = [
        ":sairedis_headers"
    ]
)
