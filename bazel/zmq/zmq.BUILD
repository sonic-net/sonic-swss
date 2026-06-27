package(default_visibility = ["//visibility:public"])

# trick to export headers in a convenient way
cc_library(
    name = "zmq_headers",
    hdrs = glob([
        "bazel/zmq/zmq_hdrs/zmq*.h",
        "bazel/zmq/zmq_hdrs/zmq*.hpp",
    ]),
    includes = [
        "bazel/zmq/zmq_hdrs",
    ],
    strip_include_prefix = "bazel/zmq/zmq_hdrs",
)

cc_import(
    name = "zmq",
    hdrs = [],  # see cc_library rule above
    shared_library = "bazel/zmq/zmq_so/libzmq.so",
    deps = [
        ":zmq_headers"
    ]
)
