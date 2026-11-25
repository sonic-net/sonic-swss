"""Expose orchagent's declared shared libraries for installed-loader checks."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _runtime_libraries_impl(ctx):
    libraries = []
    for dep in ctx.attr.deps:
        for linker_input in dep[CcInfo].linking_context.linker_inputs.to_list():
            for library in linker_input.libraries:
                dynamic = library.resolved_symlink_dynamic_library or library.dynamic_library
                if dynamic:
                    libraries.append(dynamic)
    return [DefaultInfo(files = depset(libraries))]

runtime_libraries = rule(
    implementation = _runtime_libraries_impl,
    attrs = {"deps": attr.label_list(providers = [CcInfo])},
)
