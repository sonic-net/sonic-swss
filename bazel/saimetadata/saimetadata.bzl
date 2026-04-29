"""libsaimetadata cc_import setup"""

def _impl(repository_ctx):
    hdrs_path = "/usr/include/sai"
    repository_ctx.symlink(hdrs_path, "bazel/saimetadata/saimetadata_hdrs")
    so_path = "/usr/lib/x86_64-linux-gnu/libsaimetadata.so"
    repository_ctx.symlink(so_path, "bazel/saimetadata/saimetadata_so/libsaimetadata.so")
    meta_so_path = "/usr/lib/x86_64-linux-gnu/libsaimeta.so"
    repository_ctx.symlink(meta_so_path, "bazel/saimetadata/saimetadata_so/libsaimeta.so")

    repository_ctx.symlink(Label("//:bazel/saimetadata/saimetadata.BUILD"), "BUILD")

saimetadata_configure = repository_rule(
    implementation = _impl,
    local = True,
)
