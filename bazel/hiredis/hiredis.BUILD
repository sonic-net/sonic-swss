package(default_visibility = ["//visibility:public"])

# trick to export headers in a convenient way
cc_library(
    name = "hiredis_headers",
    hdrs = glob([
        "bazel/hiredis/hiredis_hdrs/**/*.h",
    ]),
    includes = [
        "bazel/hiredis/hiredis_hdrs",
    ],
    strip_include_prefix = "bazel/hiredis/hiredis_hdrs",
)

cc_import(
    name = "hiredis",
    hdrs = [],  # see cc_library rule above
    shared_library = "bazel/hiredis/hiredis_so/libhiredis.so",
    deps = [
        ":hiredis_headers"
    ]
)
