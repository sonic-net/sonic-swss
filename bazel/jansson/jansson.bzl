"""libjansson cc_import setup"""

def _impl(repository_ctx):
    hdrs_path = "/usr/include/jansson.h"
    repository_ctx.symlink(hdrs_path, "bazel/jansson/jansson_hdrs/jansson.h")
    hdrs_config_path = "/usr/include/jansson_config.h"
    repository_ctx.symlink(hdrs_config_path, "bazel/jansson/jansson_hdrs/jansson_config.h")
    so_path = "/usr/lib/x86_64-linux-gnu/libjansson.so"
    repository_ctx.symlink(so_path, "bazel/jansson/jansson_so/libjansson.so")

    repository_ctx.symlink(Label("//:bazel/jansson/jansson.BUILD"), "BUILD")

jansson_configure = repository_rule(
    implementation = _impl,
    local = True,
)
