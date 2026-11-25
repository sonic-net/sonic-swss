"""Compiler and linker flags shared by the SWSS Bazel targets."""

# CFLAGS_COMMON from configure.ac, excluding host include paths.
CXXFLAGS_COMMON = [
    "-std=c++14",
    "-Wall",
    "-fPIC",
    "-Wno-write-strings",
    "-Werror",
    "-Wno-reorder",
    "-Wcast-align",
    "-Wcast-qual",
    "-Wconversion",
    "-Wdisabled-optimization",
    "-Wextra",
    "-Wfloat-equal",
    "-Wformat=2",
    "-Wformat-nonliteral",
    "-Wformat-security",
    "-Wformat-y2k",
    "-Wimport",
    "-Winit-self",
    "-Winvalid-pch",
    "-Wlong-long",
    "-Wmissing-field-initializers",
    "-Wmissing-format-attribute",
    "-Wno-aggregate-return",
    "-Wno-padded",
    "-Wno-switch-enum",
    "-Wno-unused-parameter",
    "-Wpacked",
    "-Wpointer-arith",
    "-Wredundant-decls",
    "-Wstack-protector",
    "-Wstrict-aliasing=3",
    "-Wswitch",
    "-Wswitch-default",
    "-Wunreachable-code",
    "-Wunused",
    "-Wvariadic-macros",
    "-Wno-switch-default",
    "-Wno-long-long",
    "-Wno-redundant-decls",
    "-Wno-error=missing-field-initializers",
    "-Wno-error=overloaded-virtual",
    "-Wno-psabi",
    # rules_distroless may publish optional include directories that are absent.
    "-Wno-missing-include-dirs",
]

# The managed GCC toolchain supplies optimization, stack/CPU hardening, and
# linker hardening. Preserve the additional normal Trixie SWSS release flags:
# its nested debian/rules invocation uses fortification level 3 rather than the
# toolchain's baseline level 2. The warning set above includes -Wformat=2/-Werror.
RELEASE_HARDENING_COPTS = select({
    "//tools/bazel:release_link": [
        "-Wdate-time",
        "-U_FORTIFY_SOURCE",
        "-D_FORTIFY_SOURCE=3",
    ],
    "//conditions:default": [],
})

DBGFLAGS = select({
    "@sonic_build_infra//:debug_enabled": ["-ggdb", "-DDEBUG", "-O0", "-U_FORTIFY_SOURCE"],
    "//conditions:default": ["-g"],
})

DBGFLAGS_NDEBUG = select({
    "@sonic_build_infra//:debug_enabled": ["-ggdb", "-DDEBUG", "-O0", "-U_FORTIFY_SOURCE"],
    "//conditions:default": ["-g", "-DNDEBUG"],
})

ASAN_COPTS = select({
    "//tools/bazel:asan_enabled": [
        "-fsanitize=address",
        "-DASAN_ENABLED",
        "-ggdb",
        "-fno-omit-frame-pointer",
        "-U_FORTIFY_SOURCE",
        "-Wno-maybe-uninitialized",
    ],
    "//conditions:default": [],
})

ASAN_LINKOPTS = select({
    "//tools/bazel:asan_enabled": ["-fsanitize=address"],
    "//conditions:default": [],
})

GCOV_COPTS = select({
    "//tools/bazel:gcov_enabled": [
        "-O0",
        # Keep coverage explicitly unfortified when overriding the toolchain
        # optimization default.
        "-U_FORTIFY_SOURCE",
        "-fprofile-arcs",
        "-ftest-coverage",
        "-DGCOV_ENABLED",
    ],
    "//conditions:default": [],
})

GCOV_LINKOPTS = select({
    "//tools/bazel:gcov_enabled": ["-fprofile-arcs", "-lgcov"],
    "//conditions:default": [],
})
