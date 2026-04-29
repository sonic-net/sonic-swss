package(default_visibility = ["//visibility:public"])

cc_library(
    name = "gcovpreload_lib",
    srcs = ["gcovpreload.c"],
    copts = [
        "-shared",
        "-fpic",
    ],
)

cc_shared_library(
    name = "gcovpreload",
    shared_lib_name = "libgcovpreload.so",
    deps = [":gcovpreload_lib"],
)

filegroup(
    name = "lcov_cobertura",
    srcs = ["lcov_cobertura.py"],
)
