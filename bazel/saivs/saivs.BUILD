package(default_visibility = ["//visibility:public"])

cc_library(
    name = "saivs_headers",
    hdrs = glob([
        "bazel/saivs/saivs_hdrs/sai*.h",
    ], exclude = ["bazel/saimetadata/saimetadata_hdrs/sairedis.h"]),
    includes = [
        "bazel/saivs/saivs_hdrs",
    ],
    strip_include_prefix = "bazel/saivs/saivs_hdrs",
)

cc_import(
    name = "saivs",
    hdrs = [],
    shared_library = "bazel/saivs/saivs_so/libsaivs.so",
    deps = [
        ":saivs_headers"
    ]
)
