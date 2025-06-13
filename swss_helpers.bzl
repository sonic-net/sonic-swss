"""Helpers for building SWSS."""

load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files")

DebugInfo = provider(
    "debug symbols for binaries",
    fields = {
        "debug_symbols_file": "The File containing the separated, compressed debug symbols.",
    },
)

def _strip_binary_and_extract_debug_impl(ctx):
    """Rule implementation function."""

    input_binary = ctx.file.src
    stripped_binary_out = ctx.outputs.stripped_binary
    debug_symbols_out = ctx.outputs.debug_symbols

    # todo: update bazel version to have a hermetic objcopy path
    # objcopy_path = ctx.toolchains["@bazel_tools//tools/cpp:toolchain_type"].cc.objcopy_executable
    objcopy_path = "/usr/bin/objcopy"
    if not objcopy_path:
        fail("Could not find objcopy executable in C++ toolchain")

    args = ctx.actions.args()
    args.add(input_binary.path)
    args.add(stripped_binary_out.path)
    args.add(debug_symbols_out.path)
    args.add(objcopy_path)

    # Define the set of input files needed by the action
    input_files = depset(
        transitive = [
            depset([input_binary]),
        ],
    )

    ctx.actions.run_shell(
        mnemonic = "objcopy",
        inputs = input_files,
        outputs = [stripped_binary_out, debug_symbols_out],
        arguments = [args],
        progress_message = "Stripping and extracting debug symbols from %{input}",
        command = """
#!/bin/bash
set -euo pipefail

INPUT_RO="$1"
STRIPPED_BINARY_OUTPUT="$2"
DEBUG_SYMBOLS_OUTPUT="$3"
OBJCOPY_PATH="$4"

TEMP_COPY="${INPUT_RO}.input_copy.tmp"

# echo "--- Processing: ${INPUT_RO} ---"
# echo "Temp Writable Copy: ${TEMP_COPY}"
# echo "Stripped Output: ${STRIPPED_BINARY_OUTPUT}"
# echo "Debug Output: ${DEBUG_SYMBOLS_OUTPUT}"
# echo "Objcopy: ${OBJCOPY_PATH}"

# echo "Step 0: Copying input to writable location..."
cp "${INPUT_RO}" "${TEMP_COPY}" || { echo "ERROR: Failed to copy input."; exit 1; }

# echo "Step 1: Extracting debug symbols..."
"$OBJCOPY_PATH" \
    --compress-debug-sections \
    --only-keep-debug \
    "${TEMP_COPY}" \
    "${DEBUG_SYMBOLS_OUTPUT}" \
    || { echo "ERROR: Failed to extract debug symbols."; rm -f "${TEMP_COPY}"; exit 1; }

# echo "Step 2: Stripping binary and adding debug link..."
"$OBJCOPY_PATH" \
    --strip-debug \
    --add-gnu-debuglink="${DEBUG_SYMBOLS_OUTPUT}" \
    "${TEMP_COPY}" \
    "${STRIPPED_BINARY_OUTPUT}" \
    || { echo "ERROR: Failed to strip or add debug link."; rm -f "${TEMP_COPY}"; exit 1; }

# echo "Step 3: Cleaning up temporary copy..."
rm -f "${TEMP_COPY}"

# echo "--- Successfully processed ${INPUT_RO} ---"
""",
    )

    return [
        DefaultInfo(files = depset([stripped_binary_out])),
        DebugInfo(debug_symbols_file = debug_symbols_out),
    ]

strip_binary_and_extract_debug = rule(
    implementation = _strip_binary_and_extract_debug_impl,
    attrs = {
        "src": attr.label(
            mandatory = True,
            allow_single_file = True,  # Expects the output file from cc_binary
            doc = "The original binary file target (e.g., cc_binary).",
        ),
    },
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    outputs = {
        # The key becomes accessible via ctx.outputs.<key> in the implementation
        "stripped_binary": "%{name}.stripped",  # File named like the rule instance (e.g., "orchagent")
        "debug_symbols": "%{name}.debug",  # File named <rule_name>.debug (e.g., "orchagent.debug")
    },
    doc = "Extracts compressed debug symbols to a .debug file, strips the binary, and adds a gnu_debuglink.",
)

def _create_stripped_binary_with_debug(name, target, **kwargs):
    strip_binary_and_extract_debug(
        name = name,
        src = target,
        **kwargs
    )

def create_pkg_files_of_stripped_binaries(name, targets):
    """Creates stripped binaries pkg_files and a pkg_files of their debug symbols.

    Args:
      name: Prefix of pkg_files to create
      targets: Label of the original cc_binary target to process.
    """
    if not targets:
        fail("The 'targets' list cannot be empty.")

    for target in targets:
        target_name = Label(target).name + "_stripped"
        _create_stripped_binary_with_debug(
            name = target_name,
            target = target,
        )

    binaries = [Label(target).name for target in targets]
    rename_map = {}
    for binary in binaries:
        key = binary + "_stripped.stripped"
        rename_map[key] = binary

    pkg_files(
        name = name + ".stripped",
        srcs = [binary + "_stripped.stripped" for binary in binaries],
        renames = rename_map,
        attributes = pkg_attributes(
            group = "root",
            mode = "0755",
            user = "root",
        ),
        prefix = "/usr/bin",
    )

    debug_rename_map = {}

    for binary in binaries:
        key = binary + "_stripped.debug"
        debug_rename_map[key] = binary

    pkg_files(
        name = name + ".debug",
        srcs = [binary + "_stripped.debug" for binary in binaries],
        renames = debug_rename_map,
        attributes = pkg_attributes(
            group = "root",
            mode = "0755",
            user = "root",
        ),
        prefix = "/usr/lib/debug/usr/bin/",
    )

def add_debug_to_copts():
    return select({
        "//:debug_mode": ["-ggdb -DDEBUG"],
        "//conditions:default": ["-g"],
    })  #remove this

def add_gcovpreload_to_linkopts():
    return select({
        "//:enable_gcov": [],
        "//conditions:default": [],
    })
