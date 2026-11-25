#!/usr/bin/env python3
"""Build and inspect the production C++ graph used by SWSS CI."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from typing import Any

from production_graph import (
    AGGREGATE, ARCHITECTURES, PRODUCTION_QUERY, cquery_expression,
    parse_program_graph, program_labels, validate_program_inventory,
)


ROOT = Path(__file__).resolve().parents[1]
GCOV_PRELOAD_TEST = "//gcovpreload:gcovpreload_test"
PROTOBUF_RUNTIME_TEST = "//bazel:protobuf_runtime_test"
CODEQL_COMPILE_TARGETS = [GCOV_PRELOAD_TEST, PROTOBUF_RUNTIME_TEST]
NATIVE_TEST_TARGETS = [GCOV_PRELOAD_TEST, PROTOBUF_RUNTIME_TEST]
CODEQL_TEST_SOURCES = {"gcovpreload/gcovpreload_test.cpp", "bazel/protobuf_runtime_test.cpp"}
CODEQL_FEATURE_SOURCES = {"gcovpreload/gcovpreload.cpp"}
FEATURE_SOURCE_BOUNDARIES = CODEQL_FEATURE_SOURCES | {"lib/asan.cpp", "lib/asan_ctor.cpp"}
NATIVE_SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def relative_path(value: str) -> str:
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or path.as_posix() != value:
        raise ValueError(f"expected a normalized relative path: {value!r}")
    return value


def checkout_revision() -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()


def build_definitions_sha256() -> str:
    """Bind graph evidence to the tracked build definitions in this checkout."""
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    definitions = {}
    for name in sorted(set(tracked) - {""}):
        path = Path(name)
        if path.suffix == ".bzl" or path.name in {"BUILD", "BUILD.bazel", "MODULE.bazel", ".bazelrc", ".bazelversion"}:
            relative_path(name)
            definitions[name] = sha256(ROOT / name)
    if not definitions:
        raise ValueError("checkout has no tracked Bazel build definitions")
    return hashlib.sha256(json.dumps(definitions, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def inventory_sha256(programs: dict[str, dict[str, Any]]) -> str:
    return hashlib.sha256(json.dumps(programs, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def receipt_artifact(receipt_path: Path, receipt: dict[str, Any], key: str, supplied: Path | None = None) -> Path:
    """Resolve a retained artifact and verify its location and recorded bytes."""
    record = receipt[key]
    path = receipt_path.parent / relative_path(record["artifact"])
    if not path.resolve().is_relative_to(receipt_path.parent.resolve()):
        raise ValueError(f"{key} input leaves the build receipt directory")
    if supplied is not None and supplied.resolve() != path.resolve():
        raise ValueError(f"{key} input does not match the build receipt path")
    if sha256(path) != record["sha256"]:
        raise ValueError(f"{key} input does not match the build receipt hash")
    return path


def load_retained_graph(receipt_path: Path, receipt: dict[str, Any] | None = None) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    """Replay production selection from a checksummed build artifact, without Bazel."""
    if receipt is None:
        receipt = json.loads(receipt_path.read_text())
    if receipt.get("schema_version") != 3 or receipt.get("artifact_type") != "cpp_executables":
        raise ValueError("unsupported production build receipt schema")
    revision = checkout_revision()
    definitions = build_definitions_sha256()
    if receipt.get("git_revision") != revision:
        raise ValueError("build receipt and checkout revisions must match")
    if receipt.get("build_definitions_sha256") != definitions:
        raise ValueError("build receipt and checkout build definitions must match")
    architecture = receipt.get("architecture")
    if architecture not in ARCHITECTURES or receipt.get("target_platform") != ARCHITECTURES[architecture][2]:
        raise ValueError("build receipt must use the selected native architecture and platform")
    path = receipt_artifact(receipt_path, receipt, "production_graph")
    graph = json.loads(path.read_text())
    if graph.get("git_revision") != revision or graph.get("architecture") != architecture:
        raise ValueError("production graph revision and architecture must match the build receipt")
    if graph.get("build_definitions_sha256") != definitions:
        raise ValueError("production graph and checkout build definitions must match")
    programs = parse_program_graph(graph, architecture)
    if graph.get("programs") != programs or graph.get("programs_sha256") != inventory_sha256(programs):
        raise ValueError("normalized production inventory does not match its configured graph")
    recorded = receipt.get("programs", [])
    if len(recorded) != len(programs) or {item["name"]: item["label"] for item in recorded} != program_labels(programs):
        raise ValueError("build receipt programs do not match the configured production graph")
    if receipt.get("source_files") != sorted(tracked_source_paths(programs)):
        raise ValueError("build receipt sources do not match the configured production graph")
    return graph, programs


def tracked_paths(paths: set[str]) -> set[str]:
    for path in paths:
        relative_path(path)
        source = ROOT / path
        if not source.is_file() or not source.resolve().is_relative_to(ROOT):
            raise ValueError(f"source is missing or leaves the checkout: {path}")
    tracked = set(subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0"))
    missing = sorted(paths - tracked)
    if missing:
        raise ValueError(f"sources are not tracked by Git: {missing}")
    return paths


def tracked_source_paths(programs: dict[str, dict[str, Any]]) -> set[str]:
    paths = set()
    for program in programs.values():
        for label in program["sources"]:
            match = re.fullmatch(r"//([^:]*):(.+)", label)
            if match is None:
                raise ValueError(f"production source must be a local file label: {label}")
            package, name = match.groups()
            path = relative_path((PurePosixPath(package) / name).as_posix())
            paths.add(path)
    return tracked_paths(paths)


def tracked_native_source_paths() -> set[str]:
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    return {path for path in tracked if path.endswith(NATIVE_SOURCE_SUFFIXES)}


def inspect_elf(path: Path, machine: int) -> dict[str, Any]:
    file_stat = path.stat()
    if not stat.S_ISREG(file_stat.st_mode) or file_stat.st_size < 64 or not file_stat.st_mode & 0o111:
        raise ValueError(f"output is not a nonempty executable file: {path}")
    with path.open("rb") as stream:
        header = stream.read(64)
        if header[:7] != b"\x7fELF\x02\x01\x01":
            raise ValueError(f"output is not a little-endian ELF64 file: {path}")
        fields = struct.unpack("<HHIQQQIHHHHHH", header[16:])
        elf_type, elf_machine = fields[:2]
        program_offset, header_size, entry_size, entry_count = fields[4], fields[7], fields[8], fields[9]
        if elf_machine != machine or elf_type != 3:
            raise ValueError(f"output is not a PIE for machine {machine}: {path}")
        if header_size != 64 or entry_size < 56 or not entry_count or program_offset + entry_size * entry_count > file_stat.st_size:
            raise ValueError(f"invalid ELF program header table: {path}")
        program_types = set()
        dynamic_segments = []
        for index in range(entry_count):
            stream.seek(program_offset + index * entry_size)
            entry = struct.unpack("<IIQQQQQQ", stream.read(56))
            program_types.add(entry[0])
            if entry[0] == 2:  # PT_DYNAMIC
                dynamic_segments.append((entry[2], entry[5]))
        if len(dynamic_segments) != 1:
            raise ValueError(f"output must have one ELF dynamic segment: {path}")
        dynamic_offset, dynamic_size = dynamic_segments[0]
        if dynamic_size % 16 or dynamic_offset + dynamic_size > file_stat.st_size:
            raise ValueError(f"invalid ELF dynamic segment: {path}")
        stream.seek(dynamic_offset)
        dynamic = {}
        for _ in range(dynamic_size // 16):
            tag, value = struct.unpack("<qQ", stream.read(16))
            if tag == 0:  # DT_NULL
                break
            dynamic[tag] = dynamic.get(tag, 0) | value
    if 1 not in program_types or 3 not in program_types:
        raise ValueError(f"output does not have PIE program headers: {path}")
    if 0x6474E552 not in program_types:
        raise ValueError(f"output is missing GNU_RELRO: {path}")
    # DT_BIND_NOW, DF_BIND_NOW in DT_FLAGS, or DF_1_NOW in DT_FLAGS_1.
    if 24 not in dynamic and not dynamic.get(30, 0) & 0x8 and not dynamic.get(0x6FFFFFFB, 0) & 0x1:
        raise ValueError(f"output is missing immediate binding (BIND_NOW): {path}")
    return {
        "elf_class": 64,
        "elf_machine": elf_machine,
        "elf_type": "ET_DYN",
        "pie": True,
        "gnu_relro": True,
        "bind_now": True,
        "mode": oct(stat.S_IMODE(file_stat.st_mode)),
        "size": file_stat.st_size,
        "sha256": sha256(path),
    }


def parse_cquery(contents: str, labels: set[str]) -> dict[str, list[str]]:
    outputs = {}
    for line in contents.splitlines():
        fields = line.split("\t")
        label = fields[0]
        if label.startswith("@@//"):
            label = label[2:]
        elif label.startswith("@//"):
            label = label[1:]
        if label not in labels or label in outputs:
            raise ValueError(f"unexpected or duplicate cquery label: {label}")
        outputs[label] = [relative_path(item) for item in fields[1:] if item]
    if set(outputs) != labels:
        raise ValueError(f"cquery did not return all requested labels: {sorted(labels - set(outputs))}")
    return outputs


def run(command: list[str], *, capture: bool = False, log: Path | None = None) -> str:
    display_command = "+ " + shlex.join(command)
    print(display_command, flush=True)
    if log is not None:
        if capture:
            raise ValueError("captured commands cannot also write a build log")
        with log.open("w") as stream:
            stream.write(display_command + "\n")
            process = subprocess.Popen(
                command, cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            assert process.stdout is not None
            with io.TextIOWrapper(process.stdout, encoding="utf-8", errors="replace") as output:
                for line in output:
                    print(line, end="", flush=True)
                    stream.write(line)
            returncode = process.wait()
        if returncode:
            raise subprocess.CalledProcessError(returncode, command)
        return ""
    result = subprocess.run(command, cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE if capture else None)
    return result.stdout or ""


def query_configured_rules(bazel: list[str], options: list[str], query: str, universe_scope: list[str]) -> dict[str, Any]:
    """Read configured rules with consistent labels and explicit dependencies."""
    return json.loads(run(
        bazel + ["cquery"] + options + [
            query, "--universe_scope=" + ",".join(universe_scope),
            "--noimplicit_deps", "--notool_deps", "--consistent_labels", "--output=jsonproto",
        ],
        capture=True,
    ))


def build(args: argparse.Namespace) -> None:
    host, machine, target_platform = ARCHITECTURES[args.architecture]
    if platform.system() != "Linux" or platform.machine() != host:
        raise ValueError(f"{args.architecture} CI requires a native Linux {host} runner")
    os_release = platform.freedesktop_os_release()
    if os_release.get("ID") != "debian" or os_release.get("VERSION_CODENAME") != "trixie":
        raise ValueError("production CI requires Debian Trixie")
    native_contract = None
    if args.mode != "codeql":
        if args.native_contract is None:
            raise ValueError("native builds require --native-contract")
        native_contract = args.native_contract.resolve(strict=True)
        if not native_contract.is_file():
            raise ValueError(f"native build contract is not a file: {native_contract}")
    artifact_directory = args.artifact_directory.resolve()
    if artifact_directory.exists() and any(artifact_directory.iterdir()):
        raise ValueError(f"artifact directory must be empty: {artifact_directory}")
    artifact_directory.mkdir(parents=True, exist_ok=True)

    revision = checkout_revision()
    definitions_sha256 = build_definitions_sha256()
    native_contract_sha256 = sha256(native_contract) if native_contract is not None else None
    bazel_version = (ROOT / ".bazelversion").read_text().strip()
    if run([args.bazel, "--version"], capture=True).strip() != f"bazel {bazel_version}":
        raise ValueError(f"CI requires Bazel {bazel_version}")
    options = [
        "--config=release",
        "--lockfile_mode=update",
        f"--platforms={target_platform}",
        f"--host_platform={target_platform}",
        "--noshow_progress",
        "--color=no",
        "--curses=no",
    ]
    bazel = [args.bazel]
    fresh_output_base = args.mode in ("clean", "codeql")
    if fresh_output_base:
        output_base = os.environ.get("SWSS_CODEQL_OUTPUT_BASE") if args.mode == "codeql" else None
        if output_base:
            # The workflow only fetches the locked inspection tools before tracing.
            prepared = Path(output_base)
            if (prepared / "action_cache").exists() or any((prepared / "execroot").glob("*/bazel-out")):
                raise ValueError("CodeQL requires an output base without previous build actions")
        else:
            output_base = tempfile.mkdtemp(prefix=f"swss-{args.mode}-bazel-", dir=os.environ.get("RUNNER_TEMP"))
        bazel += [f"--output_base={output_base}", "--batch"]
        options += [
            "--nouse_action_cache",
            "--noremote_accept_cached",
            "--noremote_upload_local_results",
            "--disk_cache=",
            "--remote_cache=",
            "--remote_executor=",
        ]
    if args.mode == "codeql":
        options += ["--jobs=2", "--spawn_strategy=local"]
    else:
        options += ["--jobs=4"]
        if args.mode == "clean":
            options += ["--spawn_strategy=processwrapper-sandbox"]

    repository_mapping = json.loads(run(
        bazel + [
            "mod", "dump_repo_mapping", "", "--lockfile_mode=update",
            "--noshow_progress", "--color=no", "--curses=no",
        ],
        capture=True,
    ))
    if not isinstance(repository_mapping, dict):
        raise ValueError("Bazel returned an invalid root repository mapping")
    production_graph = {
        "schema_version": 1,
        "query": PRODUCTION_QUERY,
        "architecture": args.architecture,
        "git_revision": revision,
        "build_definitions_sha256": definitions_sha256,
        "root_repository_mapping": repository_mapping,
        "cquery": query_configured_rules(bazel, options, PRODUCTION_QUERY, [AGGREGATE]),
    }
    programs = parse_program_graph(production_graph, args.architecture)
    sources = tracked_source_paths(programs)
    labels = program_labels(programs)
    production_graph["programs"] = programs
    production_graph["programs_sha256"] = inventory_sha256(programs)
    production_graph_path = artifact_directory / "production-graph.json"
    production_graph_path.write_text(json.dumps(production_graph, sort_keys=True, separators=(",", ":")) + "\n")
    if native_contract is not None:
        native = json.loads(native_contract.read_text())
        if native.get("schema_version") != 2 or native.get("git_revision") != revision:
            raise ValueError("native contract schema and revision must match this build")
        validate_program_inventory(native["programs"], programs)

    requested = list(labels.values()) + [AGGREGATE]
    additional_compile_targets = CODEQL_COMPILE_TARGETS if args.mode == "codeql" else []
    build_log = artifact_directory / "build.log"
    run(bazel + ["build"] + options + requested + additional_compile_targets, log=build_log)

    if args.mode == "codeql":
        runtime_tests = {
            "targets": [],
            "status": "not_run",
            "reason": "CodeQL compiles the selected native tests for extraction without executing them.",
        }
    else:
        native_test_log = artifact_directory / "native-tests.log"
        run(
            bazel + ["test"] + options + ["--nocache_test_results", "--test_output=errors"] + NATIVE_TEST_TARGETS,
            log=native_test_log,
        )
        runtime_tests = {
            "targets": NATIVE_TEST_TARGETS,
            "status": "passed",
            "output_base_and_action_cache_policy": "same_as_build",
            "test_result_reuse": "disabled",
            "log": {"artifact": native_test_log.name, "sha256": sha256(native_test_log)},
        }

    module_graph = run(
        bazel + [
            "mod", "graph", "--lockfile_mode=update",
            "--output=json", "--verbose", "--noshow_progress", "--color=no", "--curses=no",
        ],
        capture=True,
    )
    module_graph_data = json.loads(module_graph)
    if not isinstance(module_graph_data, dict) or not module_graph_data:
        raise ValueError("Bazel returned an empty or invalid resolved module graph")
    module_graph_path = artifact_directory / "module-graph.json"
    module_graph_path.write_text(json.dumps(module_graph_data, indent=2, sort_keys=True) + "\n")

    expression = "set(" + " ".join(requested) + ")"
    configured_rules_path = None
    native_contract_path = None
    if native_contract is not None:
        from native_build_contract import MODULE_BUILD_MAPPINGS

        universe_scope = list(labels.values())
        query = cquery_expression(programs, MODULE_BUILD_MAPPINGS)
        configured_rules = query_configured_rules(bazel, options, query, universe_scope)
        if not isinstance(configured_rules, dict) or not configured_rules.get("results"):
            raise ValueError("Bazel returned empty or invalid configured production rules")
        configured_rules_path = artifact_directory / "configured-rules.json"
        configured_rules_path.write_text(json.dumps({
            "schema_version": 1,
            "query": query,
            "universe_scope": universe_scope,
            "root_repository_mapping": repository_mapping,
            "cquery": configured_rules,
        }, separators=(",", ":")) + "\n")
        native_contract_path = artifact_directory / "native-build-contract.json"
        shutil.copy2(native_contract, native_contract_path)

    output = run(
        bazel + ["cquery"] + options + [
            expression,
            "--output=starlark",
            '--starlark:expr=str(target.label) + "\\t" + "\\t".join([file.path for file in target.files.to_list()])',
        ],
        capture=True,
    )
    outputs = parse_cquery(output, set(requested))
    program_outputs = []
    for name, label in labels.items():
        if len(outputs[label]) != 1:
            raise ValueError(f"{label} must produce one executable, found {outputs[label]}")
        program_outputs.append(outputs[label][0])
    if len(program_outputs) != len(set(program_outputs)) or sorted(outputs[AGGREGATE]) != sorted(program_outputs):
        raise ValueError("//dist:cpp_binaries outputs do not match the explicit production programs")
    if native_contract is not None and (
        sha256(native_contract) != native_contract_sha256 or sha256(native_contract_path) != native_contract_sha256
    ):
        raise ValueError("the native build contract changed during the build")

    receipts = []
    if args.mode != "codeql":
        (artifact_directory / "bin").mkdir()
    for name, label in labels.items():
        output_path = outputs[label][0]
        receipt = {"name": name, "label": label, "bazel_output": output_path}
        receipt.update(inspect_elf(ROOT / output_path, machine))
        if args.mode != "codeql":
            destination = artifact_directory / "bin" / name
            shutil.copy2(ROOT / output_path, destination)
            if sha256(destination) != receipt["sha256"]:
                raise ValueError(f"artifact copy does not match {label}")
            receipt["artifact"] = f"bin/{name}"
        receipts.append(receipt)

    protobuf_runtime = None
    if args.mode != "codeql":
        from verify_protobuf_runtime import build_and_validate
        protobuf_runtime = build_and_validate(
            ROOT, bazel, options, run, args.architecture,
            ROOT / outputs[labels["orchagent"]][0], artifact_directory / "dependencies")
    generated_lock = artifact_directory / "MODULE.bazel.lock"
    require_lock = ROOT / "MODULE.bazel.lock"
    if not require_lock.is_file() or not require_lock.stat().st_size:
        raise ValueError("Bazel did not generate its dependency lock")
    shutil.copy2(require_lock, generated_lock)

    if build_definitions_sha256() != definitions_sha256 or checkout_revision() != revision:
        raise ValueError("the checkout or Bazel build definitions changed during the build")
    receipt = {
        "schema_version": 3,
        "artifact_type": "cpp_executables",
        "build_mode": args.mode,
        "cache_policy": {
            "fresh_output_base": fresh_output_base,
            "action_result_reuse": "disabled" if fresh_output_base else "allowed",
            "repository_download_reuse": "allowed",
            "spawn_strategy": {
                "codeql": "local",
                "clean": "processwrapper-sandbox",
                "normal": "native_default",
            }[args.mode],
        },
        "architecture": args.architecture,
        "target_platform": target_platform,
        "bazel_version": bazel_version,
        "git_revision": revision,
        "build_definitions_sha256": definitions_sha256,
        "production_graph": {"artifact": production_graph_path.name, "sha256": sha256(production_graph_path)},
        "dependency_provider_mode": "registry_modules",
        "build_log": {"artifact": build_log.name, "sha256": sha256(build_log)},
        "module_graph": {"artifact": module_graph_path.name, "sha256": sha256(module_graph_path)},
        "additional_compile_targets": additional_compile_targets,
        "runtime_tests": runtime_tests,
        "protobuf_runtime": protobuf_runtime,
        "dependency_artifacts": {
            str(path.relative_to(artifact_directory)): {"sha256": sha256(path), "bytes": path.stat().st_size}
            for path in sorted((artifact_directory / "dependencies").rglob("*")) if path.is_file()
        },
        "generated_module_lock": {"artifact": generated_lock.name, "sha256": sha256(generated_lock)},
        "source_files": sorted(sources),
        "programs": receipts,
    }
    if configured_rules_path is not None:
        receipt["configured_rules"] = {"artifact": configured_rules_path.name, "sha256": sha256(configured_rules_path)}
        receipt["native_contract"] = {"artifact": native_contract_path.name, "sha256": native_contract_sha256}
    receipt_path = artifact_directory / "manifest.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    if configured_rules_path is not None:
        run([
            sys.executable, str(ROOT / "bazel/compare_build_contract.py"),
            "--architecture", args.architecture,
            "--native-contract", str(native_contract_path),
            "--configured-rules", str(configured_rules_path),
            "--build-receipt", str(receipt_path),
            "--output", str(artifact_directory / "native-contract-check.json"),
        ])
    print(f"Verified {len(receipts)} production C++ executables and {len(sources)} tracked source files")


def check_codeql(args: argparse.Namespace) -> None:
    build_receipt = json.loads(args.build_receipt.read_text())
    graph, programs = load_retained_graph(args.build_receipt, build_receipt)
    policy = build_receipt.get("cache_policy", {})
    if (
        build_receipt.get("build_mode") != "codeql"
        or policy.get("fresh_output_base") is not True
        or policy.get("action_result_reuse") != "disabled"
        or policy.get("spawn_strategy") != "local"
        or build_receipt.get("additional_compile_targets") != CODEQL_COMPILE_TARGETS
    ):
        raise ValueError("CodeQL coverage requires its fresh, uncached production build receipt")
    selected = {
        "production": tracked_source_paths(programs),
        "tests": tracked_paths(CODEQL_TEST_SOURCES),
        "features": tracked_paths(CODEQL_FEATURE_SOURCES),
    }
    expected = set().union(*selected.values())
    if sum(len(paths) for paths in selected.values()) != len(expected):
        raise ValueError("selected CodeQL source categories overlap")
    native_sources = tracked_native_source_paths()
    test_sources = {
        path for path in native_sources
        if path.startswith("tests/") or "/tests/" in path
        or re.search(r"_test\.(?:c|cc|cpp|cxx)$", path)
    }
    feature_sources = native_sources & FEATURE_SOURCE_BOUNDARIES
    observed = set()
    with args.csv.open(newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["path"]:
            raise ValueError(f"unexpected CodeQL source CSV columns: {reader.fieldnames}")
        for row in reader:
            if set(row) != {"path"} or not isinstance(row["path"], str):
                raise ValueError(f"invalid CodeQL source CSV row: {row}")
            observed.add(relative_path(row["path"]))
    missing = sorted(expected - observed)
    receipt = {
        "schema_version": 3,
        "git_revision": graph["git_revision"],
        "architecture": graph["architecture"],
        "build_definitions_sha256": graph["build_definitions_sha256"],
        "production_graph": build_receipt["production_graph"],
        "build_receipt": {"artifact": args.build_receipt.name, "sha256": sha256(args.build_receipt)},
        "additional_compile_targets": CODEQL_COMPILE_TARGETS,
        "selected_source_files": {name: sorted(paths) for name, paths in selected.items()},
        "expected_source_files": sorted(expected),
        "observed_source_files": sorted(observed),
        "observed_sources_not_in_tracked_native_inventory": sorted(observed - native_sources),
        "observed_source_file_count": len(observed),
        "matched_source_file_count": len(expected) - len(missing),
        "missing_source_files": missing,
        "additional_observed_source_files": sorted(observed - expected),
        "excluded_source_files": {
            "tests": sorted(test_sources - selected["tests"]),
            "features": sorted(feature_sources - selected["features"]),
            "other_native": sorted(native_sources - expected - test_sources - feature_sources),
        },
        "native_source_inventory": {
            "scope": "Tracked native source paths in this checkout.",
            "suffixes": list(NATIVE_SOURCE_SUFFIXES),
            "test_path_rule": "A tests directory component or a filename ending in _test followed by a native source suffix.",
            "feature_paths": sorted(FEATURE_SOURCE_BOUNDARIES),
        },
        "exclusion_note": "Excluded files are not validated by this job; exclusion does not classify them as unsupported.",
        "generated_source_reporting": {
            "selected_source_requirement": "All selected SWSS translation units must be tracked source files.",
            "generation_status_of_tracked_sources": "not_classified",
            "outside_source_root_reporting": "not_verified",
        },
        "sarif_source_reporting": "not_verified_by_this_extraction_receipt",
        "historical_coverage_comparison": {
            "status": "not_performed",
            "reason": "This check has no prior-pipeline source inventory.",
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    if missing:
        raise ValueError(f"CodeQL did not extract selected C++ sources: {missing}")
    print(
        f"CodeQL extracted all {len(expected)} selected C++ source files "
        f"({len(selected['production'])} production, {len(selected['tests'])} test, "
        f"{len(selected['features'])} feature)"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build")
    build_parser.add_argument("--architecture", choices=ARCHITECTURES, required=True)
    build_parser.add_argument("--native-contract", type=Path)
    build_parser.add_argument("--artifact-directory", type=Path, required=True)
    build_parser.add_argument("--mode", choices=("normal", "clean", "codeql"), default="normal")
    build_parser.add_argument("--bazel", default="bazel")
    coverage_parser = commands.add_parser("check-codeql")
    coverage_parser.add_argument("--build-receipt", type=Path, required=True)
    coverage_parser.add_argument("--csv", type=Path, required=True)
    coverage_parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        (build if args.command == "build" else check_codeql)(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
