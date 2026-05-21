"""libsaivs cc_import setup"""

def _impl(repository_ctx):
    # headers are at /usr/include/*.h
    # from dpkg -L libsaivs3-dev
    hdrs_path = "/usr/include/"
    repository_ctx.symlink(hdrs_path, "bazel/saivs/saivs_hdrs")
    so_path = "/usr/lib/x86_64-linux-gnu/libsaivs.so"
    repository_ctx.symlink(so_path, "bazel/saivs/saivs_so/libsaivs.so")

    repository_ctx.symlink(Label("//:bazel/saivs/saivs.BUILD"), "BUILD")

saivs_configure = repository_rule(
    implementation = _impl,
    local = True,
)
