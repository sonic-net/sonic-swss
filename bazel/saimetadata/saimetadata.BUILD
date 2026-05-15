package(default_visibility = ["//visibility:public"])

# trick to export headers in a convenient way
cc_library(
    name = "saimetadata_headers",
    hdrs = glob([
        "bazel/saimetadata/saimetadata_hdrs/**/*.h",
    ], exclude = ["bazel/saimetadata/saimetadata_hdrs/sairedis.h"]),
    includes = [
        "bazel/saimetadata/saimetadata_hdrs",
    ],
    strip_include_prefix = "bazel/saimetadata/saimetadata_hdrs",
    deps = [
        "@local_sairedis//:sairedis_headers",
    ]
)

cc_import(
    name = "saimetadata",
    hdrs = [],  # see cc_library rule above
    shared_library = "bazel/saimetadata/saimetadata_so/libsaimetadata.so",
    deps = [
        ":saimetadata_headers",
    ],
)

cc_import(
    name = "saimeta",
    hdrs = [],  # see cc_library rule above
    shared_library = "bazel/saimetadata/saimetadata_so/libsaimeta.so",
    deps = [
        ":saimetadata_headers",
    ],
)
