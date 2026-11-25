# Build SWSS from an external module

For builds within the SWSS checkout, use the [maintainer guide](../tools/bazel/README.md).

This caller builds the SWSS module's C++ targets with the managed GCC toolchain.
The example uses Bazel 8.5.1 and paths visible inside the build environment;
adjust those paths to match your mounts.

## Caller module

Create the caller's `MODULE.bazel` with the current iteration pins:

```starlark
module(name = "sonic_swss_caller")

bazel_dep(name = "rules_cc", version = "0.2.20")
bazel_dep(name = "rules_distroless", version = "0.9.4.sonic.26")
bazel_dep(name = "sonic-swss", version = "0.0.3", repo_name = "sonic_swss")
bazel_dep(
    name = "sonic-swss-common",
    version = "0.0.8-2a4ddd070edb719f2e969a880a5cfff711e64e72",
    repo_name = "sonic_swss_common",
)
bazel_dep(
    name = "sonic-build-infra",
    version = "0.0.45-77ef625a4ddafc8cdc15f3cc6aee31eabac8a5de",
    repo_name = "sonic_build_infra",
)

local_path_override(module_name = "sonic-swss", path = "/inputs/sonic-swss")

# Root overrides in imported modules do not propagate to this caller.
single_version_override(
    module_name = "tar.bzl",
    version = "0.10.5.sonic.2",
)
single_version_override(
    module_name = "rules_cc",
    version = "0.2.20",
)
single_version_override(
    module_name = "libnl3",
    version = "3.7.0.sonic.10",
)
single_version_override(
    module_name = "re2",
    version = "2024-07-02.bcr.1",
)

gcc = use_extension("@sonic_build_infra//toolchains/gcc:extensions.bzl", "gcc")
use_repo(gcc, "gcc_toolchains")
register_toolchains("@gcc_toolchains//:all")
```

Register GCC in the root caller so the managed toolchain takes priority over
`local_config_cc`. The local SWSS override points to the source tree containing
the component BUILD files. Declare Common directly so the caller can enable its
YANG C++ sources. The selected Common revision
generates the configuration schema from its source inputs. The RE2 override
selects the BCR metadata repair for its obsolete local C++ extension and retains
the same upstream source archive.

## Caller Bazel configuration

Put `8.5.1` in the caller's `.bazelversion` and add these settings to its
`.bazelrc`:

```text
common --check_direct_dependencies=off
common --registry=https://raw.githubusercontent.com/sonic-net/sonic-bazel-registry/main
common --registry=https://bcr.bazel.build/
common --lockfile_mode=update
common --noincompatible_disallow_empty_glob
common --platforms=@sonic_build_infra//platforms:x86_64_trixie
common --@sonic_swss_common//tools/bazel:yang_modules=True

build --remote_download_regex=(^|.*/)tools/build_tools/kernel_runtime_(amd64|arm64)(/.*)?$
build:release --compilation_mode=opt
```

Use the same single SONiC registry branch as SWSS `.bazelrc`, followed by BCR.
CI and local builds use the published modules on registry `main`. Module versions, source archives/checksums, overlays and
toolchain inputs remain pinned. The root libnl3 override retains its selected SONiC fix; the current
Distroless version sorts above historical requests without an override.
The rules_cc 0.2.20 override keeps Distroless's private C++ import rule
compatible with Bazel 8.5.1; releases 0.2.21 and 0.2.22 require a runtime
toolchain type that this Bazel version does not provide.
Distroless `0.9.4.sonic.20` retains include fragments for shared infrastructure
APT imports; Protobuf headers and its runtime are now source-built.
Run `bazel mod graph` and confirm `libnl3@3.7.0.sonic.10`,
`rules_distroless@0.9.4.sonic.26`, `protobuf-legacy@3.21.12.sonic.4`, and
`sonic-build-infra@0.0.45-77ef625a4ddafc8cdc15f3cc6aee31eabac8a5de` are selected,
with `tar.bzl@0.10.5.sonic.2` and `sonic-dash-api@0.0.6-55310bf7437d67411418c974587adcdc1b194d6b`.
The managed GCC toolchain supplies `-O2`, stack and architecture hardening,
RELRO, immediate binding, and early `--as-needed`. SWSS adds its release-specific
`-Wdate-time` and `_FORTIFY_SOURCE=3` override. Infrastructure source
[`a12bc82`](https://github.com/securely1g/sonic-build-infra/commit/a12bc826d6f811a157db5c7c02a2fe033896cc14)
uses the declared shared Debian sysroot for library lookup. For AMD64 its joined
arguments include `-L<sysroot>/usr/lib/gcc/x86_64-linux-gnu/14` and
`-L<sysroot>/usr/lib/x86_64-linux-gnu`; `<sysroot>` abbreviates the declared input
path. These replace the older host `/lib/<multiarch>` link search. The reviewed
contract records both architecture variants; it does not infer them from cquery. The unused opt-in `as_needed` feature alias
was removed upstream; the default `--as-needed` argument remains enabled.
Keep the generated `MODULE.bazel.lock` out of Git and retain it with CI
resolution artifacts. The Common flag
enables its YANG C++ sources; this Common version generates the configuration
schema from source inputs.

## Build the C++ programs

Run Bazel from the caller workspace:

```sh
bazel build --config=release @sonic_swss//dist:cpp_binaries
```

This target selects all 29 C++ programs. Build a single program with:

```sh
bazel build --config=release @sonic_swss//orchagent:orchagent
```

## Rust callers

SWSS consumes Common's public Rust library target
`@sonic_swss_common//crates/swss-common:swss_common`. Common owns its bindings
and native-library linkage. Bazel resolves shared third-party targets from
[`sonic-rust-deps@0.0.5-4064a814e8530300a7acbad4a9aff0621e2fb98e`](https://github.com/sonic-net/sonic-bazel-registry/tree/main/modules/sonic-rust-deps/0.0.5-4064a814e8530300a7acbad4a9aff0621e2fb98e).
That shared module resolves its tracked Cargo inputs with `rules_rs`. Common
and SWSS select the same `serde` and `serde_core` targets, so their public Rust
types use compatible Serde traits without caller repository mappings.

Building `countersyncd` also needs Rust and bindgen toolchains registered by the
consuming root. Use the `rules_rs` 0.1.0 extensions and Rust 1.90.0 settings in
SWSS's `MODULE.bazel` as the reference. The standalone bindgen registration is
development-only and does not propagate to an external caller.

Rust 1.90 with the C++ linker also requires the following root build option.
Expose the `rules_rust` compatibility repository through `rules_rs`, as SWSS's
`MODULE.bazel` does, and add this to the caller's `.bazelrc`:

```text
build --@rules_rust//rust/settings:experimental_use_allocator_libraries_with_mangled_symbols=True
```

No preparation script, `Cargo.Bazel.lock`, repository mapping or Common source
override is needed. The Common source revision is on its maintained `master`
branch and is selected through the registered module version above. Keep any
Cargo Common pin in the caller aligned with that source revision.

The native checks validate Common as a dependency of the SWSS root. A separate
image root has not yet been validated with this version; test the same
Common/Serde regression before adopting it for image assembly.

After applying the root settings, build from the caller workspace:

```sh
bazel build --config=release @sonic_swss//crates/countersyncd:countersyncd
```

## Package callers

The runtime package includes `countersyncd`, so the consuming root needs the
same Rust toolchains described above. Build the runtime
tar and matching detached symbols from the caller workspace:

```sh
bazel build --config=release \
  @sonic_swss//dist:swss_pkg \
  @sonic_swss//dist:swss_pkg.debug_symbols
```

The consuming image supplies the matching shared libraries. See the
[maintainer guide](../tools/bazel/README.md#runtime-and-debug-packages) for the
payload and the [CI guide](README.md#runtime-and-debug-package-validation) for
the package checks and their scope.

## Dependency interfaces

The C++ module uses these public interfaces for programs and tests:

| Input | Public label |
| --- | --- |
| Common shared library | `@sonic_swss_common//:libswsscommon_shared` |
| Common Rust library | `@sonic_swss_common//crates/swss-common:swss_common` |
| SAI metadata libraries | `@sonic_sairedis//meta:saimetadata_shared`, `@sonic_sairedis//meta:saimeta_shared` |
| sairedis shared library | `@sonic_sairedis//lib:sairedis_shared` |
| DASH generated headers and shared library | `@sonic_dash_api//:dashapi` |

Common is fetched from
[securely1g/sonic-swss-common](https://github.com/securely1g/sonic-swss-common).
SAI and sairedis provide their headers and libraries through their own module
targets. DASH generates its headers and builds `libdashapi.so` with the
source-built Protobuf 3.21.12 compiler. SWSS and DASH both use
`@protobuf_legacy//:libprotobuf`, built as a shared runtime from that same upstream
release; neither consumer links a separate static Protobuf implementation.
The remaining Debian development libraries come from the declared `trixie` and
`swss_debian` sets in [MODULE.bazel](../MODULE.bazel).
