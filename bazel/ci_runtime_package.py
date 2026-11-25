#!/usr/bin/env python3
"""Build, inspect, and retain the native SWSS runtime/debug package pair."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile

from ci_production import ARCHITECTURES, ROOT, run, sha256
from verify_runtime_package import read_contract, verify
from verify_protobuf_runtime import build_and_validate
from verify_libteam_runtime import build_and_validate as validate_libteam
from verify_rust_dependencies import verify as verify_rust_dependencies


TARGETS = {
    "runtime": "//dist:swss_pkg",
    "debug": "//dist:swss_pkg.debug_symbols",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--architecture", choices=ARCHITECTURES, required=True)
    parser.add_argument("--artifact-directory", type=Path, required=True)
    parser.add_argument("--native-contract", type=Path, required=True)
    args = parser.parse_args()
    host, _, target_platform = ARCHITECTURES[args.architecture]
    if platform.system() != "Linux" or platform.machine() != host:
        raise ValueError("package validation requires a native " + host + " runner")
    os_release = platform.freedesktop_os_release()
    if os_release.get("ID") != "debian" or os_release.get("VERSION_CODENAME") != "trixie":
        raise ValueError("package validation requires Debian Trixie")
    if run(["dpkg", "--print-architecture"], capture=True).strip() != args.architecture:
        raise ValueError("userspace architecture differs from the requested target")
    bazel_version = (ROOT / ".bazelversion").read_text().strip()
    if run(["bazel", "--version"], capture=True).strip() != "bazel " + bazel_version:
        raise ValueError("unexpected Bazel version")
    read_contract(ROOT, args.native_contract, args.architecture)
    artifact_directory = args.artifact_directory.resolve()
    if artifact_directory.exists() and any(artifact_directory.iterdir()):
        raise ValueError("package artifact directory must be empty")
    artifact_directory.mkdir(parents=True, exist_ok=True)
    native_contract = artifact_directory / "native-build-contract.json"
    shutil.copy2(args.native_contract, native_contract)
    _, contract_provenance = read_contract(ROOT, native_contract, args.architecture)
    contract_provenance["path"] = native_contract.name
    options = [
        "--config=release",
        "--lockfile_mode=update",
        "--platforms=" + target_platform,
        "--host_platform=" + target_platform,
        "--jobs=4",
        "--noshow_progress",
        "--color=no",
        "--curses=no",
    ]
    rust_dependencies = verify_rust_dependencies(options, artifact_directory, run)
    rust_test = "//crates/countersyncd:common_rust_test"
    run(["bazel", "test"] + options + ["--test_output=errors", rust_test],
        log=artifact_directory / "common-rust-test.log")
    shutil.copy2(ROOT / "bazel-testlogs/crates/countersyncd/common_rust_test/test.xml",
                 artifact_directory / "common-rust-test.xml")
    run(["bazel", "build"] + options + list(TARGETS.values()),
        log=artifact_directory / "build.log")
    packages = {}
    for kind, label in TARGETS.items():
        outputs = run(["bazel", "cquery"] + options + [label, "--output=files"],
                      capture=True).strip().splitlines()
        if len(outputs) != 1:
            raise ValueError(label + " must produce exactly one archive")
        source = ROOT / outputs[0]
        if not source.is_file() or source.stat().st_size == 0:
            raise ValueError("missing package output: " + str(source))
        target = artifact_directory / source.name
        shutil.copy2(source, target)
        packages[kind] = target
    if packages["runtime"] == packages["debug"]:
        raise ValueError("runtime and debug outputs must be distinct")
    report = verify(ROOT, packages["runtime"], packages["debug"], args.architecture, native_contract)
    if report["native_contract"]["sha256"] != contract_provenance["sha256"]:
        raise ValueError("native contract changed during package build")
    report["native_contract"] = contract_provenance
    with tempfile.TemporaryDirectory(prefix="swss-installed-orchagent-") as temporary:
        binary = Path(temporary) / "orchagent"
        with tarfile.open(packages["runtime"]) as archive:
            members = [member for member in archive.getmembers()
                       if member.name.removeprefix("./") == "usr/bin/orchagent"]
            if len(members) != 1 or not members[0].isfile():
                raise ValueError("runtime package must contain one installed orchagent")
            binary.write_bytes(archive.extractfile(members[0]).read())
        binary.chmod(0o755)
        report["protobuf_runtime"] = build_and_validate(
            ROOT, ["bazel"], options, run, args.architecture, binary,
            artifact_directory / "dependencies")
    report["libteam_runtime"] = validate_libteam(
        ROOT, ["bazel"], options, run, args.architecture, packages["runtime"],
        artifact_directory / "libteam")
    (artifact_directory / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    graph = run(["bazel", "mod", "graph", "--lockfile_mode=update", "--output=json", "--verbose"],
                capture=True)
    graph_data = json.loads(graph)
    if not isinstance(graph_data, dict) or not graph_data:
        raise ValueError("empty resolved module graph")
    (artifact_directory / "module-graph.json").write_text(json.dumps(graph_data, indent=2) + "\n")
    lock = ROOT / "MODULE.bazel.lock"
    if not lock.is_file() or not lock.stat().st_size:
        raise ValueError("Bazel did not generate its dependency lock")
    shutil.copy2(lock, artifact_directory / lock.name)
    manifest = {
        "revision": run(["git", "rev-parse", "HEAD"], capture=True).strip(),
        "tree": run(["git", "rev-parse", "HEAD^{tree}"], capture=True).strip(),
        "tracked_changes": run(["git", "status", "--porcelain", "--untracked-files=no"], capture=True).strip(),
        "pr_head": os.environ.get("CI_HEAD_SHA"),
        "run_url": (os.environ.get("GITHUB_SERVER_URL", "https://github.com") + "/" +
                    os.environ.get("GITHUB_REPOSITORY", "") + "/actions/runs/" +
                    os.environ.get("GITHUB_RUN_ID", "")),
        "architecture": args.architecture,
        "native_contract": contract_provenance,
        "execution_machine": platform.machine(),
        "os_release": os_release,
        "bazel_version": bazel_version,
        "target_platform": target_platform,
        "configuration": "release, shared runtime/debug split",
        "targets": TARGETS,
        "rust_integration_test": {"target": rust_test, "status": "passed"},
        "rust_dependencies": rust_dependencies,
        "artifacts": {str(path.relative_to(artifact_directory)): {"sha256": sha256(path), "bytes": path.stat().st_size}
                      for path in sorted(artifact_directory.rglob("*")) if path.is_file()},
    }
    (artifact_directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"installed_files": report["installed_files"],
                      "elf_pairs": len(report["elf_pairs"]),
                      "gdb_lookups": len(report["gdb"])}, indent=2))


if __name__ == "__main__":
    main()
