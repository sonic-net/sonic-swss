"""libnl cc_import setup"""

def _impl(repository_ctx):
    hdrs_path = "/usr/include/libnl3"
    repository_ctx.symlink(hdrs_path, "bazel/libnl3/libnl3_hdrs")
    nl3_so_path = "/lib/x86_64-linux-gnu/libnl-3.so"
    repository_ctx.symlink(nl3_so_path, "bazel/libnl3/libnl3_so/libnl-3.so")
    nl_route_3_so_path = "/usr/lib/x86_64-linux-gnu/libnl-route-3.so"
    repository_ctx.symlink(nl_route_3_so_path, "bazel/libnl3/libnl3_so/libnl-route-3.so")
    nl_genl_3_so_path = "/lib/x86_64-linux-gnu/libnl-genl-3.so"
    repository_ctx.symlink(nl_genl_3_so_path, "bazel/libnl3/libnl3_so/libnl-genl-3.so")
    nl_nf3_3_so_path = "/usr/lib/x86_64-linux-gnu/libnl-nf-3.so"
    repository_ctx.symlink(nl_nf3_3_so_path, "bazel/libnl3/libnl3_so/libnl-nf-3.so")

    repository_ctx.symlink(Label("//:bazel/libnl3/libnl3.BUILD"), "BUILD")

libnl3_configure = repository_rule(
    implementation = _impl,
    local = True,
)
