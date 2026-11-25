"""Check staged orchagent dependencies and execute the DASH/Protobuf runtime."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import zlib


TARGETS = {
    "protobuf-runtime.tar": "@protobuf_legacy//:libprotobuf_pkg",
    "protobuf-debug.tar": "@protobuf_legacy//:libprotobuf_pkg.debug_symbols",
    "dash-runtime.tar": "@sonic_dash_api//:libdashapi_pkg",
    "dash-debug.tar": "@sonic_dash_api//:libdashapi_pkg.debug_symbols",
}
LIBRARIES = "//bazel:orchagent_runtime_libraries"
PROBE = "//bazel:protobuf_runtime_test"
# The native Trixie image supplies a matching loader and libc ABI foundation.
# Replacing only libc through LD_LIBRARY_PATH can mismatch its host interpreter.
SYSTEM_RUNTIME = {
    "libc.so.6", "libm.so.6", "libmvec.so.1", "libpthread.so.0",
    "librt.so.1", "libdl.so.2", "libutil.so.1",
    "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def command(*args, env=None):
    result = subprocess.run(args, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=60)
    require(result.returncode == 0, "Command failed: " + repr(args) + "\n" + result.stdout)
    return result.stdout


def notes(path):
    match = re.search(r"Build ID: ([0-9a-f]+)", command("readelf", "-n", str(path)))
    require(match, "Missing build ID: " + str(path))
    return match.group(1)


def soname(path, required=True):
    names = re.findall(r"Library soname: \[([^]]+)\]", command("readelf", "-d", str(path)))
    require(len(names) <= 1 and (names or not required), "Expected one SONAME: " + str(path))
    return names[0] if names else path.name


def unpack(archive, destination):
    def owned_data(member, root):
        require(member.uid == member.gid == 0, "Non-root dependency archive owner: " + member.name)
        return tarfile.data_filter(member, root)

    shutil.unpack_archive(archive, destination, format="tar", filter=owned_data)


def check_debug(binary, root, machine):
    header = binary.read_bytes()[:20]
    require(header[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", header, 18)[0] == machine,
            "Wrong dependency ELF architecture: " + str(binary))
    identifier = notes(binary)
    debug = root / "usr/lib/debug/.build-id" / identifier[:2] / (identifier[2:] + ".debug")
    require(debug.is_file() and notes(debug) == identifier, "Missing matching dependency symbols")
    require(".debug_info" not in command("readelf", "-SW", str(binary)), "Unstripped dependency runtime")
    require(".debug_info" in command("readelf", "-SW", str(debug)), "Dependency symbols have no DWARF")
    with tempfile.TemporaryDirectory(prefix="swss-dependency-debuglink-") as temporary:
        section = Path(temporary) / "debuglink"
        command("objcopy", "--dump-section", ".gnu_debuglink=" + str(section), str(binary),
                str(Path(temporary) / "copy.elf"))
        raw = section.read_bytes()
        end = raw.index(0)
        require(raw[:end].decode() == debug.name, "Dependency debuglink filename differs")
        require(struct.unpack_from("<I", raw, (end + 4) & ~3)[0] == zlib.crc32(debug.read_bytes()),
                "Dependency debuglink checksum differs")
    return {"path": str(binary.relative_to(root)), "build_id": identifier,
            "debug_path": str(debug.relative_to(root)), "runtime_sha256": sha(binary),
            "debug_sha256": sha(debug)}


def validate(execution_root, orchagent, probe, libraries, archives, architecture, evidence):
    """Inspect staged SWSS dependencies and execute a native DASH round trip."""
    machine = {"amd64": 62, "arm64": 183}[architecture]
    with tempfile.TemporaryDirectory(prefix="swss-protobuf-runtime-") as temporary:
        root = Path(temporary)
        declared = root / "declared-libraries"
        declared.mkdir()
        library_records = []
        skipped_libraries = []
        linked_inputs = {}
        for library in libraries:
            record = {"source": str(library.relative_to(execution_root)), "sha256": sha(library)}
            with library.open("rb") as stream:
                is_elf = stream.read(4) == b"\x7fELF"
            if not is_elf:
                skipped_libraries.append(dict(record, reason="linker script; no loadable ELF"))
                continue
            name = soname(library, required=False)
            if name in SYSTEM_RUNTIME:
                skipped_libraries.append(dict(record, soname=name, reason="native Trixie loader/libc ABI foundation"))
                continue
            # Deliver these dependencies from their checked runtime/debug pair.
            if name == "libdashapi.so" or name.startswith("libprotobuf"):
                require(name in ("libdashapi.so", "libprotobuf.so.32"), "Competing Protobuf runtime: " + name)
                linked_inputs.setdefault(name, []).append(dict(record, build_id=notes(library)))
                continue
            destination = declared / name
            require(not destination.exists() or sha(destination) == sha(library), "Conflicting shared runtime: " + name)
            shutil.copyfile(library, destination)
            library_records.append({"soname": name, "source": str(library.relative_to(execution_root)), "sha256": sha(library)})
        for archive in archives.values():
            unpack(archive, root)
        protobufs = [path for path in (root / "usr/lib").rglob("libprotobuf.so*") if path.is_file() and not path.is_symlink()]
        require(len(protobufs) == 1, "Expected exactly one packaged Protobuf shared runtime")
        protobuf = protobufs[0]
        dash = root / "usr/lib/libdashapi.so"
        require(dash.is_file(), "Missing packaged DASH library")
        require(soname(protobuf) == "libprotobuf.so.32" and soname(dash) == "libdashapi.so", "Wrong source dependency SONAME")
        require(set(linked_inputs) == {"libprotobuf.so.32", "libdashapi.so"},
                "Declared consumer closure is missing a source-built shared dependency")
        # Packages rebuild with debug information before splitting. Their build
        # IDs can differ from an ordinary release link; match each installed
        # runtime to its own debug file below and execute it with the consumer.
        runtime_directories = [protobuf.parent, dash.parent, declared]
        environment = os.environ.copy()
        for variable in ("LD_PRELOAD", "LD_AUDIT", "RUNFILES_DIR", "RUNFILES_MANIFEST_FILE", "TEST_SRCDIR"):
            environment.pop(variable, None)
        environment["LD_LIBRARY_PATH"] = ":".join(map(str, runtime_directories))
        environment["LD_BIND_NOW"] = "1"
        staged_orchagent = root / "usr/bin/orchagent"
        staged_orchagent.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(orchagent, staged_orchagent)
        require(sha(staged_orchagent) == sha(orchagent), "Staging changed the executable")
        match = re.search(r"Requesting program interpreter: ([^]]+)",
                          command("readelf", "-l", str(staged_orchagent)))
        require(match and Path(match[1]).is_file(), "Missing native ELF interpreter")
        # This temporary install root is outside /usr. Ignore the main ELF
        # RPATH so its old Bazel runfiles and host /usr cannot outrank the
        # explicit stage; exact library paths below make fallback a failure.
        native_loader = [match[1], "--inhibit-rpath", "",
                         "--library-path", environment["LD_LIBRARY_PATH"]]
        loader = command(*native_loader, "--list", str(staged_orchagent), env=environment)
        require("not found" not in loader, "Unresolved orchagent shared dependency: " + loader)
        loaded = re.findall(r"libprotobuf[^\s]* => (\S+)", loader)
        require(len(loaded) == 1 and Path(loaded[0]).resolve() == protobuf.resolve(),
                "orchagent did not select the packaged source Protobuf: " + loader)
        loaded_dash = re.findall(r"libdashapi\.so => (\S+)", loader)
        require(len(loaded_dash) == 1 and Path(loaded_dash[0]).resolve() == dash.resolve(), "orchagent did not load the packaged DASH library")
        for binary in (orchagent, dash):
            needed = re.findall(r"Shared library: \[([^]]+)\]", command("readelf", "-d", str(binary)))
            require("libprotobuf.so.32" in needed, "Missing shared Protobuf dependency: " + str(binary))
            defined = command("nm", "-D", "--defined-only", "--demangle", str(binary))
            require(not re.search(r"\b[TDB] google::protobuf::(?:Message::DebugString|DescriptorPool::generated_pool|internal::VerifyVersion)", defined),
                    "Consumer embeds Protobuf runtime implementation: " + str(binary))
        output = command(*native_loader, str(probe), env=environment)
        values = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
        require(values.get("protobuf_version") == "3021012" and values.get("dash_roundtrip") == "passed", "DASH serialization failed: " + output)
        require(Path(values["protobuf_library"]).resolve() == protobuf.resolve() and
                Path(values["protobuf_loaded"]).resolve() == protobuf.resolve(), "DASH probe loaded a different Protobuf runtime")
        pairs = [check_debug(path, root, machine) for path in (protobuf, dash)]
        result = {"architecture": architecture, "protobuf_version": "3.21.12", "runtime_count": 1,
                  "orchagent": {"sha256": sha(orchagent), "loader": loader.replace(str(root), "<stage>"),
                                "loader_command": [part.replace(str(root), "<stage>") for part in native_loader] + ["--list", "<stage>/usr/bin/orchagent"]},
                  "dash_roundtrip": "passed", "probe": output.replace(str(root), "<stage>"),
                  "dependency_debug_pairs": pairs, "declared_libraries": library_records,
                  "skipped_library_inputs": skipped_libraries, "linked_dependency_inputs": linked_inputs,
                  "scope": "Staged dependency selection for unchanged orchagent ELF bytes; a separate native probe executes DASH serialization with the packaged libraries. The native loader ignores main-program RPATH for the temporary stage; complete orchagent relocation, default installed-system search paths and daemon behavior are not verified."}
    (evidence / "protobuf-runtime.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def build_and_validate(source, bazel, options, run, architecture, orchagent, evidence):
    """Build explicitly named runtime/debug inputs and retain their evidence."""
    evidence.mkdir(parents=True, exist_ok=True)
    run(bazel + ["build"] + options + list(TARGETS.values()) + [LIBRARIES, PROBE],
        log=evidence / "build.log")

    # The preceding build creates this standard workspace symlink. Resolving it
    # avoids `bazel info` interpreting the external platform flags without the
    # module repository mapping that build/cquery use.
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
    require(libraries, "Empty orchagent dependency closure")
    probes = outputs(PROBE)
    require(len(probes) == 1 and probes[0].is_file(), "Missing native Protobuf test executable")
    probe = evidence / "protobuf-runtime-test"
    shutil.copy2(probes[0], probe)
    report = validate(execution_root, orchagent, probe, libraries, archives, architecture, evidence)
    (evidence / "archive-sha256.json").write_text(json.dumps({name: sha(path) for name, path in archives.items()}, indent=2) + "\n")
    return report
