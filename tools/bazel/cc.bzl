"""Small C++ macros that preserve SWSS's native build configurations."""

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load(
    ":flags.bzl",
    "ASAN_COPTS",
    "ASAN_LINKOPTS",
    "CXXFLAGS_COMMON",
    "DBGFLAGS",
    "DBGFLAGS_NDEBUG",
    "GCOV_COPTS",
    "GCOV_LINKOPTS",
    "RELEASE_HARDENING_COPTS",
)

_SWSS_REPOSITORY = Label("//:MODULE.bazel")

def _cxxopts(cxxopts, ndebug, asan):
    return CXXFLAGS_COMMON + (DBGFLAGS_NDEBUG if ndebug else DBGFLAGS) + (ASAN_COPTS if asan else []) + GCOV_COPTS + cxxopts

def _linkopts(linkopts, asan):
    return (ASAN_LINKOPTS if asan else []) + GCOV_LINKOPTS + linkopts

def _local_include_cxxopts(local_include_directories):
    """Returns ordered local -I options relative to the SWSS repository."""
    options = []
    repository_root = _SWSS_REPOSITORY.workspace_root
    for directory in local_include_directories:
        path = directory
        if repository_root:
            path = repository_root if directory == "." else repository_root + "/" + directory
        options.append("-I" + path)
    return options

def swss_headers(name, hdrs, includes):
    """Declares compatible public headers and production header inputs.

    The public target retains its propagated include directories. Production
    binaries use the companion target so their explicit native -I options
    determine the local header search order.

    Args:
      name: Public header target name.
      hdrs: Header labels shared by both targets.
      includes: Include directories propagated by the public target.
    """
    cc_library(
        name = name,
        hdrs = hdrs,
        cxxopts = CXXFLAGS_COMMON + DBGFLAGS + GCOV_COPTS,
        includes = includes,
        linkopts = GCOV_LINKOPTS,
    )

    cc_library(
        name = "production_" + name,
        hdrs = hdrs,
    )

def swss_cc_binary(name, srcs = [], deps = [], cxxopts = [], linkopts = [], ndebug = False, asan = True, gcov_preload = "static", local_include_directories = [], **kwargs):
    """Declares a daemon and retains its ASAN/GCOV startup objects.

    Args:
      name: Bazel target name.
      srcs: C++ source labels.
      deps: C++ dependency labels.
      cxxopts: Additional C++ compiler options.
      linkopts: Additional linker options.
      ndebug: Whether to define NDEBUG outside the debug configuration.
      asan: Whether to apply ASAN flags and startup sources when enabled.
      gcov_preload: GCOV startup mode: "static", "shared", or "none".
      local_include_directories: Ordered SWSS-root-relative native include paths.
      **kwargs: Additional cc_binary attributes.
    """
    if gcov_preload not in ["static", "shared", "none"]:
        fail("gcov_preload must be static, shared, or none")

    if asan:
        srcs = srcs + select({
            "//tools/bazel:asan_enabled": [
                "//lib:asan.cpp",
                "//lib:asan_ctor.cpp",
            ],
            "//conditions:default": [],
        })

    if gcov_preload == "static":
        srcs = srcs + select({
            "//tools/bazel:gcov_enabled": ["//gcovpreload:gcovpreload.cpp"],
            "//conditions:default": [],
        })
    elif gcov_preload == "shared":
        deps = deps + select({
            "//tools/bazel:gcov_enabled": ["//gcovpreload:gcovpreload_shared"],
            "//conditions:default": [],
        })

    cc_binary(
        name = name,
        srcs = srcs,
        deps = deps,
        cxxopts = _cxxopts(cxxopts, ndebug, asan) + _local_include_cxxopts(local_include_directories) + RELEASE_HARDENING_COPTS,
        linkopts = _linkopts(linkopts, asan),
        **kwargs
    )
