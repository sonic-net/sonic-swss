"""libzmq cc_import setup"""

def _impl(repository_ctx):
    # headers are at /usr/include/*.h
    # from dpkg -L libzmq3-dev
    hdrs_path = "/usr/include/"
    repository_ctx.symlink(hdrs_path, "bazel/zmq/zmq_hdrs")
    so_path = "/usr/lib/x86_64-linux-gnu/libzmq.so"
    repository_ctx.symlink(so_path, "bazel/zmq/zmq_so/libzmq.so")

    repository_ctx.symlink(Label("//:bazel/zmq/zmq.BUILD"), "BUILD")

zmq_configure = repository_rule(
    implementation = _impl,
    local = True,
)
