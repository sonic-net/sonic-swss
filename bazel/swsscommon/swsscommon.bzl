"""SWSS Common Import Setup"""

def _impl(repository_ctx):
    hdrs_path = "/usr/include/swss"
    repository_ctx.symlink(hdrs_path, "bazel/swsscommon/swss_hdrs")
    so_path = "/usr/lib/x86_64-linux-gnu/libswsscommon.so"
    repository_ctx.symlink(so_path, "bazel/swsscommon/swss_so/libswsscommon.so")

    repository_ctx.symlink(Label("//:bazel/swsscommon/swsscommon.BUILD"), "BUILD")

swsscommon_configure = repository_rule(
    implementation = _impl,
    local = True,
)
