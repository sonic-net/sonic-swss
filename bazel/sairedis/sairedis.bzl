"""libsairedis cc_import setup"""

def _impl(repository_ctx):
    hdrs_path = "/usr/include/sai"
    repository_ctx.symlink(hdrs_path, "bazel/sairedis/sairedis_hdrs")
    so_path = "/usr/lib/x86_64-linux-gnu/libsairedis.so"
    repository_ctx.symlink(so_path, "bazel/sairedis/sairedis_so/libsairedis.so")

    repository_ctx.symlink(Label("//:bazel/sairedis/sairedis.BUILD"), "BUILD")

sairedis_configure = repository_rule(
    implementation = _impl,
    local = True,
)
