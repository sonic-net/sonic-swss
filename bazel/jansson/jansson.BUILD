package(default_visibility = ["//visibility:public"])

cc_library(
    name = "jansson_headers",
    hdrs = glob([
        "bazel/jansson/jansson_hdrs/*.h",
    ]),
    includes = [
        "bazel/jansson/jansson_hdrs",
    ],
    strip_include_prefix = "bazel/jansson/jansson_hdrs",
)

cc_import(
    name = "jansson",
    hdrs = [],  # see cc_library rule above
    shared_library = "bazel/jansson/jansson_so/libjansson.so",
    deps = [
        ":jansson_headers"
    ]
)
