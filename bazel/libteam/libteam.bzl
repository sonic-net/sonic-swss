"""libteamd cc_import setup"""

def _impl(repository_ctx):
    hdrs_path = "/usr/include/"
    repository_ctx.symlink(hdrs_path, "bazel/libteam/libteam_hdrs")
    so_path = "/usr/lib/x86_64-linux-gnu/libteam.so"
    repository_ctx.symlink(so_path, "bazel/libteam/libteam_so/libteam.so")
    ctl_so_path = "/usr/lib/x86_64-linux-gnu/libteamdctl.so"
    repository_ctx.symlink(ctl_so_path, "bazel/libteam/libteam_so/libteamdctl.so")

    repository_ctx.symlink(Label("//:bazel/libteam/libteam.BUILD"), "BUILD")

libteam_configure = repository_rule(
    implementation = _impl,
    local = True,
)
