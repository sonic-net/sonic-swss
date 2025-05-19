workspace(name = "sonic_swss")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("swss_deps.bzl", "swss_deps")

swss_deps()

load("@rules_pkg//:deps.bzl", "rules_pkg_dependencies")
rules_pkg_dependencies()

load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")

rules_foreign_cc_dependencies()

load("//:bazel/swsscommon/swsscommon.bzl", "swsscommon_configure")
swsscommon_configure(name = "local_swsscommon")

load("//:bazel/hiredis/hiredis.bzl", "hiredis_configure")
hiredis_configure(name = "local_hiredis")

load("//:bazel/libnl3/libnl3.bzl", "libnl3_configure")
libnl3_configure(name = "local_libnl3")

load("//:bazel/sairedis/sairedis.bzl", "sairedis_configure")
sairedis_configure(name = "local_sairedis")

load("//:bazel/saimetadata/saimetadata.bzl", "saimetadata_configure")
saimetadata_configure(name = "local_saimetadata")

load("//:bazel/zmq/zmq.bzl", "zmq_configure")
zmq_configure(name = "local_zmq")

load("//:bazel/libteam/libteam.bzl", "libteam_configure")
libteam_configure(name = "local_libteam")

load("//:bazel/saivs/saivs.bzl", "saivs_configure")
saivs_configure(name = "local_saivs")

load("//:bazel/jansson/jansson.bzl", "jansson_configure")
jansson_configure(name = "local_jansson")

load("@com_github_grpc_grpc//bazel:grpc_deps.bzl", "grpc_deps")
grpc_deps()
load("@com_github_grpc_grpc//bazel:grpc_extra_deps.bzl", "grpc_extra_deps")
grpc_extra_deps()

load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")
protobuf_deps()
load("@rules_proto//proto:repositories.bzl", "rules_proto_dependencies", "rules_proto_toolchains")
rules_proto_dependencies()
rules_proto_toolchains()
