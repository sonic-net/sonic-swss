# Native build contract reference

The [CI guide](README.md) explains how to run the production checks. For routine
source and dependency changes, start with the [maintainer guide](../tools/bazel/README.md).
Normal Bazel builds read the maintained component BUILD files directly; this
contract is independent validation performed by CI.

## Configure the native source inventory

Run the commands below from the SWSS repository root in a native Linux build
environment with the Automake configuration dependencies installed. The
[Bazel workflow](../.github/workflows/bazel.yml) lists those host dependencies.
Use the normal Linux VS release configuration, without ASAN or GCOV:

```sh
./autogen.sh
debian/rules override_dh_auto_configure
```

This runs the native configuration step. It does not compile programs or create
a Debian package. The source and configured trees must come from the same
checkout; an in-source configured tree is supported.

## Compare native and Bazel build settings

Each native CI runner records Automake's selected sources, install paths, local
include order, compiler options, and linker options in an independent JSON
contract. Target hardening options can differ between AMD64 and ARM64.
`native_build_contract.py` only produces this CI artifact; normal builds do not invoke it.

```sh
python3 bazel/native_build_contract.py \
  --source "$PWD" \
  --configured-build "$PWD" \
  --output-build-contract native-build-contract.json \
  --git-revision "$(git rev-parse HEAD)"
```

The JSON contract uses schema version 2. It records the supplied Git revision,
runner machine, configured Automake `host_cpu` values, programs, installed data,
and normalized compile/link profiles. The comparison validates the revision
against the build receipt and checkout. An AMD64 contract cannot validate ARM64.
An unset legacy `FPM_PATH` uses the local `fpmsyncd` header directory; explicit
values are preserved.

`compile_profiles` and `link_profiles` contain ordered argument records with an
`argument` and `classification`. Identical argument lists share a profile named
after the first program using it. Each `programs` entry refers to one compile
profile and one link profile. This retains option order and duplicate native
library arguments while keeping repeated program settings in one place.

The normal-release comparison uses these classifications:

| Classification | Bazel comparison |
| --- | --- |
| `literal` | Compare the resolved target `cxxopts` or `linkopts`. An unfamiliar non-path flag remains literal. |
| `include` | Require exact local `-I` order and duplicates, or a reachable mapped external dependency label. |
| `source_prefix_map` | Record the native mapping with source and build roots replaced by `<swss>`; its toolchain-action equivalent is outside this check. |
| `toolchain` | Accept the mapped option under the exact reviewed infra version; toolchain actions are not inspected. |
| `target_toolchain` | Accept the mapped option only for the named native CPU and reviewed infra version. |
| `replacement` | Apply only the explicit native-to-Bazel replacement in `compile_replacements`. |
| `native_only` | Omit only the documented native option from target `cxxopts`. |
| `native_link_driver` | Omit the listed compile options repeated by Automake on an ordinary non-LTO link command from target `linkopts`. |
| `target_native_link_driver` | Apply the same omission only for the CPU named in `target_toolchain_compile_options`. |
| `library` | Require the mapped dependency label to be reachable, or record the named toolchain library under the reviewed infra boundary. |

The known include roots are explicit: `<swss>` names local component paths,
`<swsscommon-headers>` names Common, `<libnl3-headers>` names libnl3, and
`<sai-headers>` maps to the public sairedis header dependencies.
`<system-headers>` records the managed toolchain and named dependency headers
that replace native `/usr/include`. The contract output rejects an include or
library path without a mapping and a native library without a provider.

The current intentional differences are named in the contract and comparison
result. The selected production sources do not include `config.h`, so the
mapping omits `-DHAVE_CONFIG_H`; review that omission when source membership
changes. The target replaces native fortification level 2 with level 3 and uses
the listed `-Wformat=2` and `-Werror` replacements. It also adds
`-Wno-missing-include-dirs`. Repeated macro, warning, language-standard, debug,
and PIC controls compare by their final setting; other target compile literals
and all target link literals retain order. Local include order is checked
separately. `reviewed_toolchain_additional_link_options` records the intended
immediate-binding and `--as-needed` additions under the reviewed infra version;
this configured-rule check does not mechanically verify those options.

CI discovers production binaries and their selected sources through
`//dist:cpp_binaries` using a configured Bazel query. It retains that graph in
`production-graph.json`, together with the checkout revision, architecture,
repository mapping, and a digest of the maintained build definitions. The
build receipt binds the graph to its checksum. CodeQL coverage reads this
same retained graph from its build receipt and needs no Automake configuration.

CI captures the root workspace's resolved production `cc_binary` rules with
the build's release, architecture, and dependency settings. The bounded query
uses the production labels selected by `//dist:cpp_binaries` as its universe and follows paths only to
the explicitly mapped provider, local header, and rule-support labels. It uses
`--noimplicit_deps --notool_deps --consistent_labels --output=jsonproto`.
`configured-rules.json` wraps that JSON with schema version 1, the query string,
the ordered universe, and the root repository mapping from
`bazel mod dump_repo_mapping ''`. The build receipt records the native contract,
configured rules, and module graph paths and hashes.

The helper invokes the comparison after writing its build receipt. It can also
be run directly with those retained artifacts:

```sh
python3 bazel/compare_build_contract.py \
  --architecture amd64 \
  --native-contract artifacts/production/native-build-contract.json \
  --configured-rules artifacts/production/configured-rules.json \
  --build-receipt artifacts/production/manifest.json \
  --output artifacts/production/native-contract-check.json
```

The comparison requires the native source set, resolved target `cxxopts`
and `linkopts`, local include order, and mapped dependency-label reachability
to match. It rejects an unclassified target option or direct dependency and
requires production header targets to add inputs without propagated options.
It verifies the checkout revision, build-definition and artifact hashes, native architecture, and
the exact reviewed `sonic-build-infra` version. The separate result records all
input hashes, the checked programs, and named intentional differences.

This evidence does not inspect toolchain actions, response files, transitive
provider header or library paths, or external header search precedence. It
collapses repeated dependency aliases only when their resolved `actual` label
agrees; configuration-specific provider behavior remains outside this check.
It does not establish native link ordering or runtime parity. DEBUG, ASAN,
GCOV, Rust, Cargo, and runtime targets remain separate validation scope; this
check does not assess their completion.

## Unsupported native inputs

The reader accepts the normal Linux VS release configuration. It rejects GCOV
inputs, installed libraries, custom Automake install hooks, generated
`BUILT_SOURCES`, and local link dependencies until those inputs have explicit
Bazel rules.
