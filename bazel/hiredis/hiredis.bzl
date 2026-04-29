"""libhiredis cc_import setup"""

def _impl(repository_ctx):
    hdrs_path = "/usr/include/hiredis"
    repository_ctx.symlink(hdrs_path, "bazel/hiredis/hiredis_hdrs")
    so_path = "/usr/lib/x86_64-linux-gnu/libhiredis.so"
    repository_ctx.symlink(so_path, "bazel/hiredis/hiredis_so/libhiredis.so")

    repository_ctx.symlink(Label("//:bazel/hiredis/hiredis.BUILD"), "BUILD")

hiredis_configure = repository_rule(
    implementation = _impl,
    local = True,
)
