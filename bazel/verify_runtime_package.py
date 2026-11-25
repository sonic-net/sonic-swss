#!/usr/bin/env python3
"""Validate SWSS runtime tar contents and matching detached ELF debug files."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import tarfile
import tempfile
import zlib


def require(condition, message):
    if not condition:
        raise ValueError(message)


def relative_path(value):
    require(isinstance(value, str) and value, "missing relative path")
    path = PurePosixPath(value)
    require(not path.is_absolute() and ".." not in path.parts and str(path) == value,
            "unsafe contract path: " + value)
    return path


def read_contract(source, native_contract, architecture):
    """Read expected installed files from the independently configured Make build."""
    raw = native_contract.read_bytes()
    contract = json.loads(raw)
    require(contract.get("schema_version") == 2, "native contract schema must be 2")
    revision = command("git", "-C", str(source), "rev-parse", "HEAD").strip()
    require(contract.get("git_revision") == revision, "native contract revision differs from checkout")
    host = {"amd64": "x86_64", "arm64": "aarch64"}[architecture]
    require(contract.get("architecture") == {
        "execution_machine": host,
        "configured_target_cpus": [host],
    }, "native contract architecture differs from requested package")
    programs = contract.get("programs")
    require(isinstance(programs, dict) and programs, "native contract has no programs")
    installs = contract.get("automake_install")
    require(isinstance(installs, list), "native contract has no Automake install inventory")
    expected = {}
    source_root = source.resolve()

    def add(path, mode, origin):
        relative_path(path)
        require(path not in expected, "duplicate install path: " + path)
        if origin is not None:
            require(origin.is_file(), "missing install source: " + str(origin))
            require(origin.resolve().is_relative_to(source_root),
                    "install source escapes checkout: " + str(origin))
        expected[path] = (mode, origin)

    for program in programs.values():
        add(program["install_path"], 0o755, None)
    for item in installs:
        require(isinstance(item["mode"], int) and item["mode"] in (0o644, 0o755),
                "unsupported Automake install mode")
        origin = source / relative_path(item["source"])
        add(item["install_path"], item["mode"], origin)
    for line in (source / "debian/swss.install").read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        origin, directory = line.split()
        relative_path(origin)
        relative_path(directory)
        path = str(PurePosixPath(directory) / PurePosixPath(origin).name)
        add(path, 0o755 if directory == "usr/bin" else 0o644,
            None if origin == "target/release/countersyncd" else source / origin)
    provenance = {
        "path": str(native_contract),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "schema_version": contract["schema_version"],
        "git_revision": revision,
        "architecture": contract["architecture"],
    }
    return expected, provenance


def read_archive(path):
    files = {}
    with tarfile.open(path) as archive:
        for member in archive:
            name = str(PurePosixPath(member.name))
            require(not PurePosixPath(name).is_absolute() and ".." not in PurePosixPath(name).parts,
                    "unsafe archive path: " + name)
            require(member.uid == 0 and member.gid == 0, "non-root owner: " + name)
            if member.isdir():
                require(member.mode == 0o755, "directory permissions: " + name)
                continue
            require(member.isfile(), "unexpected archive entry type: " + name)
            require(name not in files, "duplicate archive member: " + name)
            files[name] = (member.mode, archive.extractfile(member).read())
    return files


def command(*args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    require(result.returncode == 0, "command failed: " + repr(args) + "\n" + result.stdout)
    return result.stdout


def build_id(path):
    match = re.search(r"Build ID: ([0-9a-f]+)", command("readelf", "-n", str(path)))
    require(match is not None, "missing build ID: " + str(path))
    return match[1]


def verify_inventory(runtime, expected):
    require(set(runtime) == set(expected),
            "runtime install mismatch: missing=" + repr(sorted(set(expected) - set(runtime))) +
            " extra=" + repr(sorted(set(runtime) - set(expected))))
    for path, (mode, origin) in expected.items():
        actual_mode, data = runtime[path]
        require(actual_mode == mode, "runtime permissions: " + path)
        if origin is not None:
            require(data == origin.read_bytes(), "installed data differs from source: " + path)


def verify(source, runtime_tar, debug_tar, architecture, native_contract):
    expected, provenance = read_contract(source, native_contract, architecture)
    runtime = read_archive(runtime_tar)
    debug = read_archive(debug_tar)
    verify_inventory(runtime, expected)
    machine = {"amd64": 62, "arm64": 183}[architecture]
    pairs = []
    used_debug = set()
    with tempfile.TemporaryDirectory(prefix="swss-package-check-") as temporary:
        staging = Path(temporary)
        for path, (mode, origin) in expected.items():
            if origin is not None:
                continue
            _, data = runtime[path]
            require(data[:6] == b"\x7fELF\x02\x01", "expected little-endian ELF64: " + path)
            require(struct.unpack_from("<H", data, 18)[0] == machine, "wrong ELF architecture: " + path)
            binary = staging / path
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(data)
            identifier = build_id(binary)
            debug_path = "usr/lib/debug/.build-id/" + identifier[:2] + "/" + identifier[2:] + ".debug"
            require(debug_path in debug, "missing detached debug file: " + path)
            debug_data = debug[debug_path][1]
            symbols = staging / debug_path
            symbols.parent.mkdir(parents=True, exist_ok=True)
            symbols.write_bytes(debug_data)
            require(build_id(symbols) == identifier, "runtime/debug build IDs differ: " + path)
            require(".debug_info" not in command("readelf", "-SW", str(binary)), "runtime still contains debug info: " + path)
            require(".debug_info" in command("readelf", "-SW", str(symbols)), "debug file has no source information: " + path)
            link = staging / "debuglink"
            command("objcopy", "--dump-section", ".gnu_debuglink=" + str(link), str(binary))
            raw = link.read_bytes()
            end = raw.index(0)
            require(raw[:end].decode() == symbols.name, "debuglink filename: " + path)
            crc = struct.unpack_from("<I", raw, (end + 4) & ~3)[0]
            require(crc == zlib.crc32(debug_data), "debuglink checksum: " + path)
            used_debug.add(debug_path)
            pairs.append({"path": path, "build_id": identifier, "debug_path": debug_path})
        require(set(debug) == used_debug, "unexpected detached debug files")
        lookups = []
        for executable, symbol in (("orchagent", "main"), ("countersyncd", "countersyncd::main")):
            output = command("gdb", "--batch", "-nx", "-nh",
                             "-ex", "set debuginfod enabled off",
                             "-ex", "set auto-load off",
                             "-ex", "set debug-file-directory " + str(staging / "usr/lib/debug"),
                             "-ex", "file " + str(staging / "usr/bin" / executable),
                             "-ex", "info line " + symbol,
                             "-ex", "python print('SWSS_DEBUG_FILES=' + repr([f.filename for f in gdb.objfiles()]))")
            pair = next(item for item in pairs if item["path"] == "usr/bin/" + executable)
            require(str(staging / pair["debug_path"]) in output,
                    "GDB did not load the packaged symbols: " + output)
            require(re.search(r'Line [1-9][0-9]* of "[^"]+"', output), "GDB source-line lookup failed: " + output)
            lookups.append({"program": executable, "source_line": output.strip()})
    return {"architecture": architecture, "native_contract": provenance, "installed_files": len(runtime),
            "elf_pairs": pairs, "gdb": lookups}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--native-contract", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--debug", type=Path, required=True)
    parser.add_argument("--architecture", choices=("amd64", "arm64"), required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.source, args.runtime, args.debug, args.architecture, args.native_contract)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
