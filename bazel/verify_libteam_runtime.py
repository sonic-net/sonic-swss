"""Check SWSS loading against the source owner's installed libteam TARs."""

import json
import os
from pathlib import Path
import re
import shutil
import tarfile
import tempfile

from verify_protobuf_runtime import (
    SYSTEM_RUNTIME, check_debug, command, require, sha, soname, unpack,
)

TARGETS = {
    "libteam-runtime.tar": "@libteam//:libteam_runtime",
    "libteam-debug.tar": "@libteam//:libteam_runtime.debug_symbols",
    "libteamdctl-runtime.tar": "@libteam//:libteamdctl_runtime",
    "libteamdctl-debug.tar": "@libteam//:libteamdctl_runtime.debug_symbols",
}
LIBRARIES = "//bazel:libteam_consumer_runtime_libraries"
CONSUMER_TEST = "@libteam//:consumer_test"
EXPECTED = {
    "teamsyncd": {"libteam.so.5", "libteamdctl.so.0"},
    "tlm_teamd": {"libteamdctl.so.0"},
    "teammgrd": set(),
}
EXPECTED_NEEDED_NETLINK = {
    "teamsyncd": {"libnl-route-3.so.200"},
    "tlm_teamd": set(),
    "teammgrd": {"libnl-3.so.200", "libnl-route-3.so.200"},
}
EXPECTED_LOADED_NETLINK = {
    "teamsyncd": {"libnl-3.so.200", "libnl-route-3.so.200"},
    "tlm_teamd": {"libnl-3.so.200", "libnl-route-3.so.200"},
    "teammgrd": set(),
}


def check_needed(program, output):
    needed = set(re.findall(r"Shared library: \[([^]]+)\]", output))
    team = {name for name in needed if name.startswith("libteam")}
    require(team == EXPECTED[program], "Unexpected libteam dependencies for " + program + ": " + repr(sorted(team)))
    netlink = {name for name in needed if name.startswith("libnl-")}
    require(netlink == EXPECTED_NEEDED_NETLINK[program],
            "Unexpected libnl dependencies for " + program + ": " + repr(sorted(netlink)))
    return sorted(needed)


def check_no_defined_netlink(program, output):
    defined = sorted(set(re.findall(r"\b(?:nl|rtnl)_[A-Za-z0-9_]+", output)))
    require(not defined,
            "Consumer embeds static libnl symbols for " + program + ": " + repr(defined[:10]))


def check_loaded(program, output, libraries, declared=None):
    require("not found" not in output, "Unresolved shared dependency for " + program)
    loaded = re.findall(r"(libteam[^\s]*) => (\S+)", output)
    require(len(loaded) == len(EXPECTED[program]) and
            {name for name, _ in loaded} == EXPECTED[program],
            "Unexpected loaded libteam libraries for " + program)
    for name, path in loaded:
        require(Path(path).resolve() == libraries[name].resolve(),
                program + " loaded libteam outside the installed owner TAR: " + path)
    if EXPECTED_LOADED_NETLINK[program]:
        loaded_netlink = re.findall(r"(libnl-[^\s]*) => (\S+)", output)
        netlink_paths = {name: path for name, path in loaded_netlink}
        require(EXPECTED_LOADED_NETLINK[program] <= set(netlink_paths),
                "Missing loaded libnl libraries for " + program + ": " + repr(sorted(netlink_paths)))
        if declared is not None:
            for name in EXPECTED_LOADED_NETLINK[program]:
                require(Path(netlink_paths[name]).resolve() == (declared / name).resolve(),
                        program + " loaded libnl outside the declared closure: " + netlink_paths[name])


def validate(execution_root, runtime, libraries, archives, architecture):
    machine = {"amd64": 62, "arm64": 183}[architecture]
    multiarch = {"amd64": "x86_64-linux-gnu", "arm64": "aarch64-linux-gnu"}[architecture]
    with tempfile.TemporaryDirectory(prefix="swss-libteam-runtime-") as temporary:
        root = Path(temporary)
        declared = root / "declared-libraries"
        declared.mkdir()
        inputs = []
        for library in libraries:
            with library.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
            name = soname(library, required=False)
            if name in SYSTEM_RUNTIME or name.startswith("libteam"):
                continue
            destination = declared / name
            require(not destination.exists() or sha(destination) == sha(library),
                    "Conflicting declared shared library: " + name)
            shutil.copyfile(library, destination)
            inputs.append({"soname": name, "source": str(library.relative_to(execution_root)), "sha256": sha(library)})
        for archive in archives.values():
            unpack(archive, root)
        installed = {name: root / "usr/lib" / multiarch / name
                     for name in ("libteam.so.5", "libteamdctl.so.0")}
        for name, path in installed.items():
            require(path.is_file() and path.resolve().is_relative_to(root), "Missing installed " + name)
            require(soname(path) == name, "Wrong installed libteam SONAME: " + str(path))
        pairs = [check_debug(path.resolve(), root, machine) for path in installed.values()]
        environment = os.environ.copy()
        for variable in ("LD_PRELOAD", "LD_AUDIT", "RUNFILES_DIR", "RUNFILES_MANIFEST_FILE", "TEST_SRCDIR"):
            environment.pop(variable, None)
        environment["LD_BIND_NOW"] = "1"
        environment["LD_LIBRARY_PATH"] = ":".join(map(str, [root / "usr/lib" / multiarch, declared]))
        programs = {}
        with tarfile.open(runtime) as archive:
            for program in EXPECTED:
                members = [member for member in archive.getmembers()
                           if member.name.removeprefix("./") == "usr/bin/" + program]
                require(len(members) == 1 and members[0].isfile(), "Missing installed SWSS program: " + program)
                binary = root / "usr/bin" / program
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(archive.extractfile(members[0]).read())
                binary.chmod(0o755)
                needed = check_needed(program, command("readelf", "-d", str(binary)))
                check_no_defined_netlink(program, command("nm", "-D", "--defined-only", str(binary)))
                record = {"sha256": sha(binary), "needed": needed}
                # teammgrd has no libteam edge; its independent cfgmgr closure
                # is covered by the existing production/package validation.
                if EXPECTED[program]:
                    match = re.search(r"Requesting program interpreter: ([^]]+)", command("readelf", "-l", str(binary)))
                    require(match and Path(match[1]).is_file(), "Missing native ELF interpreter")
                    loader = command(match[1], "--inhibit-rpath", "", "--library-path",
                                     environment["LD_LIBRARY_PATH"], "--list", str(binary), env=environment)
                    check_loaded(program, loader, installed, declared)
                    record["loader"] = loader.replace(str(root), "<stage>")
                programs[program] = record
    return {"architecture": architecture, "programs": programs, "dependency_debug_pairs": pairs,
            "declared_libraries": inputs,
            "scope": "Installed SWSS ELF dependency checks with the source owner's runtime/debug TARs. The native Trixie loader ignores main-program RPATH for this temporary stage. This does not start the SWSS daemons, validate full-image default search paths or prove forwarding behavior."}


def build_and_validate(source, bazel, options, run, architecture, runtime, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    run(bazel + ["test"] + options + ["--nocache_test_results", "--test_output=errors",
        "--build_event_json_file=" + str(evidence / "consumer-test.bep.json"), CONSUMER_TEST],
        log=evidence / "consumer-test.log")
    run(bazel + ["build"] + options + list(TARGETS.values()) + [LIBRARIES], log=evidence / "build.log")
    output_link = source / "bazel-out"
    require(output_link.is_symlink(), "Missing Bazel output symlink")
    execution_root = output_link.resolve(strict=True).parent
    require(execution_root.parent.name == "execroot" and
            (execution_root / "MODULE.bazel").samefile(source / "MODULE.bazel"),
            "Bazel output symlink does not identify this checkout execution root")

    def outputs(target):
        return [execution_root / path for path in run(bazel + ["cquery"] + options + [target, "--output=files"], capture=True).splitlines()]

    archives = {}
    for name, label in TARGETS.items():
        files = outputs(label)
        require(len(files) == 1 and files[0].is_file(), "Missing dependency archive: " + label)
        archives[name] = evidence / name
        shutil.copyfile(files[0], archives[name])
    libraries = outputs(LIBRARIES)
    require(libraries, "Empty libteam consumer dependency closure")
    report = validate(execution_root, runtime, libraries, archives, architecture)
    report["consumer_test"] = {"target": CONSUMER_TEST, "status": "passed", "test_result_cache": False}
    report["archives"] = {name: {"target": TARGETS[name], "sha256": sha(path)} for name, path in archives.items()}
    (evidence / "libteam-runtime.json").write_text(json.dumps(report, indent=2) + "\n")
    return report
