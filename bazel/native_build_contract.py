#!/usr/bin/env python3
"""Record configured Automake sources and native build settings for CI.

Component BUILD files are maintained directly. This reader provides independent
native expectations for comparing their resolved Bazel targets in CI; normal
Bazel builds do not invoke it or require an Automake configuration step.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shlex
import subprocess
import tempfile
from typing import Any


HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc"}
CXX_SUFFIXES = {".cc", ".cpp", ".cxx", ".C"}
PRIMARY = re.compile(r"^((?:(?:dist|nodist|nobase)_)*)([A-Za-z][A-Za-z0-9_]*)_(PROGRAMS|DATA|SCRIPTS|HEADERS|LIBRARIES|LTLIBRARIES)$")
SOURCE_ROOT = "<swss>"
MODULE_INCLUDE_ROOTS = {
    "/usr/include": "<system-headers>",
    "/usr/include/libnl3": "<libnl3-headers>",
    "/usr/include/sai": "<sai-headers>",
    "/usr/include/swss": "<swsscommon-headers>",
}
# These mappings describe the selected normal-release module contract. Options
# absent from these classifications remain literal requirements for the Bazel
# configured-rule comparison; the reader does not discard unfamiliar flags.
MODULE_BUILD_MAPPINGS = {
    "reviewed_infra": {
        "module": "sonic-build-infra",
        "version": "0.0.45-77ef625a4ddafc8cdc15f3cc6aee31eabac8a5de",
    },
    "include_roots": {
        "<swss>": "component_headers",
        "<system-headers>": "managed_toolchain_and_declared_dependencies",
        "<libnl3-headers>": "@libnl3",
        "<sai-headers>": "@sonic_sairedis",
        "<swsscommon-headers>": "@sonic_swss_common",
    },
    "source_prefix_maps": ["-fdebug-prefix-map", "-ffile-prefix-map", "-fmacro-prefix-map"],
    "native_only_compile_options": ["-DHAVE_CONFIG_H"],
    "toolchain_compile_options": [
        "-O2", "-fstack-protector-strong", "-fstack-clash-protection",
    ],
    "target_toolchain_compile_options": {
        "aarch64": ["-mbranch-protection=standard"],
        "x86_64": ["-fcf-protection"],
    },
    "toolchain_link_options": ["-Wl,-z,relro"],
    "native_link_driver_options": [
        "-g", "-O2", "-fstack-protector-strong", "-fstack-clash-protection",
        "-Wformat", "-Werror=format-security",
    ],
    "compile_replacements": {
        "-D_FORTIFY_SOURCE=2": ["-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=3"],
        "-Wformat": ["-Wformat=2"],
        "-Werror=format-security": ["-Werror"],
    },
    "bazel_only_compile_options": ["-Wno-missing-include-dirs"],
    # Recorded under the reviewed infra boundary, not inspected in cquery.
    "reviewed_toolchain_additional_link_options": ["-Wl,-z,now", "-Wl,--as-needed"],
    # Source-reviewed link search paths; <sysroot> denotes the declared input.
    # Like the additional options above, these are not checked by cquery.
    "reviewed_toolchain_target_link_options": {
        "aarch64": ["-L<sysroot>/usr/lib/gcc/aarch64-linux-gnu/14", "-L<sysroot>/usr/lib/aarch64-linux-gnu"],
        "x86_64": ["-L<sysroot>/usr/lib/gcc/x86_64-linux-gnu/14", "-L<sysroot>/usr/lib/x86_64-linux-gnu"],
    },
    "implicit_system_header_providers": {
        "all_programs": [
            "@trixie//libboost-dev:libboost",
            "@trixie//nlohmann-json3-dev:nlohmann-json3",
        ],
        "programs": {"orchagent": ["@swss_debian//libyaml-cpp-dev:libyaml-cpp"]},
    },
    "local_header_providers": [
        "//cfgmgr:production_headers",
        "//fdbsyncd:production_headers",
        "//fpmsyncd:production_headers",
        "//gearsyncd:production_headers",
        "//lib:production_headers",
        "//mclagsyncd:production_headers",
        "//natsyncd:production_headers",
        "//neighsyncd:production_headers",
        "//orchagent:production_headers",
        "//portsyncd:production_headers",
        "//teamsyncd:production_headers",
        "//tlm_teamd:production_headers",
        "//warmrestart:production_headers",
    ],
    "rule_support_providers": ["@rules_cc//:link_extra_lib"],
    "feature_support": {
        "debug": {"source_labels": [], "dependency_labels": []},
        "asan": {
            "source_labels": ["//lib:asan.cpp", "//lib:asan_ctor.cpp"],
            "dependency_labels": [],
        },
        "gcov": {
            "source_labels": ["//gcovpreload:gcovpreload.cpp"],
            "dependency_labels": ["//gcovpreload:gcovpreload_shared"],
        },
    },
    "library_providers": {
        "dashapi": "@sonic_dash_api//:dashapi",
        "hiredis": "@trixie//libhiredis-dev:libhiredis",
        "jansson": "@swss_debian//libjansson-dev:libjansson",
        "jemalloc": "@swss_debian//libjemalloc-dev:libjemalloc",
        "m": "toolchain:libm",
        "nl-3": "@libnl3//:libnl_3",
        "nl-genl-3": "@libnl3//:libnl_genl_3",
        "nl-nf-3": "@libnl3//:libnl_nf_3",
        "nl-route-3": "@libnl3//:libnl_route_3",
        "protobuf": "@protobuf_legacy//:libprotobuf",
        "pthread": "toolchain:libc",
        "saimeta": "@sonic_sairedis//meta:saimeta_shared",
        "saimetadata": "@sonic_sairedis//meta:saimetadata_shared",
        "sairedis": "@sonic_sairedis//lib:sairedis_shared",
        "swsscommon": "@sonic_swss_common//:libswsscommon_shared",
        "team": "@libteam//:libteam",
        "teamdctl": "@libteam//:libteamdctl",
        "zmq": "@trixie//libzmq3-dev:libzmq3",
    },
}


def relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        raise ValueError(f"invalid relative path: {value}")
    return path


def package_mode(install_path: str, mode: int) -> int:
    # Files installed in executable directories must remain executable.
    executable_directories = ("bin/", "sbin/", "usr/bin/", "usr/sbin/", "usr/games/", "etc/init.d/")
    return 0o755 if install_path.startswith(executable_directories) else mode


def query_make(directory: Path) -> dict[str, tuple[str, str]]:
    makefile = directory / "Makefile"
    if not makefile.is_file():
        raise ValueError(f"missing configured Makefile: {makefile}")
    contents = makefile.read_text()
    if re.search(r"^install-(?:exec|data)-(?:local|hook)\s*:", contents, re.MULTILINE):
        raise ValueError(f"custom Automake install hooks are not supported: {makefile}")
    with tempfile.TemporaryDirectory(prefix="sonic-swss-make-") as temporary:
        work = Path(temporary)
        # Reading a copy prevents Make from rebuilding the configured Makefile.
        (work / "configured.mk").write_text(contents)
        output = work / "variables"
        variables = " ".join(
            [
                "SUBDIRS top_srcdir abs_top_srcdir top_builddir abs_top_builddir host_cpu",
                "DEFS DEFAULT_INCLUDES INCLUDES CPPFLAGS CFLAGS CXXFLAGS LDFLAGS LIBS LDADD",
                "AM_CPPFLAGS AM_CFLAGS AM_CXXFLAGS AM_LDFLAGS CC CXX EXEEXT BUILT_SOURCES",
                "CFLAGS_COMMON",
                "$(filter %dir %_PROGRAMS %_DATA %_SCRIPTS %_HEADERS %_LIBRARIES %_LTLIBRARIES %_SOURCES %_OBJECTS %_CPPFLAGS %_CFLAGS %_CXXFLAGS %_LDFLAGS %_LDADD %_DEPENDENCIES,$(.VARIABLES))",
            ]
        )
        # The public fpmsyncd tree contains fpm/fpm.h. Supply that local include
        # root for the optional legacy FPM_PATH only in this Bazel inventory
        # query; leave explicit Make/environment values and native builds alone.
        local_defaults = "FPM_PATH ?= $(abs_top_srcdir)/fpmsyncd\n" if directory.name == "fpmsyncd" else ""
        (work / "query.mk").write_text(
            local_defaults + ".PHONY: sonic-bazel-query\n"
            "sonic-bazel-query:\n"
            f"\t@$(file >{output},SONIC_BAZEL_QUERY_V1)"
            f"$(foreach v,$(sort {variables}),$(file >>{output},$(v)\t$(origin $(v))\t$(strip $($(v))))) :\n"
        )
        environment = os.environ.copy()
        environment.update(LC_ALL="C", MAKEFLAGS="", MFLAGS="")
        subprocess.run(
            ["make", "--silent", "--no-print-directory", "-f", str(work / "configured.mk"), "-f", str(work / "query.mk"), "sonic-bazel-query"],
            cwd=directory,
            env=environment,
            check=True,
        )
        lines = output.read_text().splitlines()
    if not lines or lines[0] != "SONIC_BAZEL_QUERY_V1":
        raise ValueError(f"invalid Make query output for {directory}")
    result = {}
    for line in lines[1:]:
        fields = line.split("\t", 2)
        if len(fields) != 3 or fields[0] in result:
            raise ValueError(f"unsupported multiline or duplicate Make value in {directory}: {line}")
        result[fields[0]] = (fields[1], fields[2])
    return result


def value(variables: dict[str, tuple[str, str]], name: str, fallback: str | None = None) -> str:
    origin, result = variables.get(name, ("undefined", ""))
    if origin == "undefined" and fallback is not None:
        return value(variables, fallback)
    return result


def parse_path_option(options: list[str], index: int) -> tuple[str, str, int] | None:
    """Read a joined or separate path option, returning the next token index."""
    option = options[index]
    for prefix in ("-isystem", "-iquote", "-idirafter", "-I", "-L"):
        if option == prefix:
            if index + 1 == len(options) or options[index + 1].startswith("-"):
                raise ValueError(f"missing path after {prefix}")
            return prefix, options[index + 1], index + 2
        if option.startswith(prefix):
            return prefix, option[len(prefix):], index + 1
    return None


class Generator:
    def __init__(self, source: Path, configured: Path):
        self.source = source.resolve()
        self.configured = configured.resolve()
        self.inputs: dict[str, Path] = {}
        self.programs: list[dict[str, Any]] = []
        self.automake_install: list[dict[str, Any]] = []
        self.target_cpus: set[str] = set()

    def add_input(self, relative: str, path: Path) -> str:
        name = relative_path(relative).as_posix()
        resolved = path.resolve()
        if not resolved.is_file():
            raise ValueError(f"missing declared SWSS input: {path}")
        if not (resolved.is_relative_to(self.source) or resolved.is_relative_to(self.configured)):
            raise ValueError(f"SWSS input leaves the source and configured trees: {path}")
        previous = self.inputs.get(name)
        if previous is not None and previous.read_bytes() != resolved.read_bytes():
            raise ValueError(f"source and configured inputs collide at {name}")
        self.inputs[name] = resolved
        return name

    def resolve_source(self, token: str, subdirectory: str) -> str:
        candidate = Path(token)
        candidates = [candidate] if candidate.is_absolute() else [
            self.configured / subdirectory / candidate,
            self.source / subdirectory / candidate,
        ]
        for path in candidates:
            resolved = path.resolve()
            if resolved.is_file():
                for root in (self.source, self.configured):
                    if resolved.is_relative_to(root):
                        return self.add_input(resolved.relative_to(root).as_posix(), resolved)
                raise ValueError(f"configured source leaves SWSS inputs: {token}")
        raise ValueError(f"configured source does not exist in {subdirectory}: {token}")

    def rebase_path(self, token: str, directory: Path, local_libraries: bool = False) -> str:
        path = Path(token)
        resolved = (path if path.is_absolute() else directory / path).resolve()
        for root in (self.source, self.configured):
            if resolved.is_relative_to(root):
                if local_libraries:
                    raise ValueError(f"local library paths need explicit Bazel targets: {token}")
                relative = resolved.relative_to(root).as_posix()
                return SOURCE_ROOT + (f"/{relative}" if relative != "." else "")
        if path.is_absolute():
            return token
        raise ValueError(f"relative compiler path leaves the SWSS inputs: {token}")

    def options(self, strings: list[str], subdirectory: str, link: bool = False) -> list[str]:
        tokens = shlex.split(" ".join(strings))
        result = []
        directory = self.configured / subdirectory
        index = 0
        while index < len(tokens):
            token = tokens[index]
            parsed = parse_path_option(tokens, index)
            if parsed is not None:
                prefix, path, next_index = parsed
                rebased = self.rebase_path(path, directory, prefix == "-L")
                # Keep the native spelling in the inventory, including whether
                # the path was a separate argument. Profiles normalize it later.
                result.extend([prefix, rebased] if next_index == index + 2 else [prefix + rebased])
                index = next_index
            else:
                prefix_map = re.match(r"^(-f(?:debug|file|macro)-prefix-map=)([^=]+)=(.*)$", token)
                if prefix_map:
                    result.append(prefix_map[1] + self.rebase_path(prefix_map[2], directory) + "=" + prefix_map[3])
                else:
                    if str(self.source) in token or str(self.configured) in token:
                        raise ValueError(f"unhandled configured path in compiler option: {token}")
                    if token in ("-include", "-imacros", "-isysroot", "--sysroot"):
                        raise ValueError(f"compiler option needs an explicit Bazel input: {token}")
                    if link and not token.startswith("-") and not Path(token).is_absolute():
                        raise ValueError(f"local link input needs an explicit Bazel target: {token}")
                    result.append(token)
                index += 1
        return result

    def installed_path(self, directory: str, source: str, nobase: bool) -> str:
        path = PurePosixPath(directory)
        if not path.is_absolute() or ".." in path.parts:
            raise ValueError(f"configured install directory must be absolute: {directory}")
        suffix = relative_path(source) if nobase else PurePosixPath(source).name
        return (path.relative_to("/") / suffix).as_posix()

    def read_program(self, name: str, directory: str, install_directory: str, variables: dict[str, tuple[str, str]]) -> None:
        if not re.fullmatch(r"[A-Za-z0-9_+.-]+", name):
            raise ValueError(f"unsupported program name: {name}")
        sources = []
        for token in shlex.split(value(variables, name + "_SOURCES") + " " + value(variables, "nodist_" + name + "_SOURCES")):
            relative = self.resolve_source(token, directory)
            suffix = Path(relative).suffix
            if suffix in CXX_SUFFIXES:
                sources.append(relative)
            elif suffix not in HEADER_SUFFIXES:
                raise ValueError(f"unsupported source language for {name}: {relative}")
        if not sources or len(sources) != len(set(sources)):
            raise ValueError(f"empty or duplicate configured C++ source list for {name}")
        objects = shlex.split(value(variables, name + "_OBJECTS"))
        if len(objects) != len(sources) or any(not item.endswith(".o") for item in objects):
            raise ValueError(f"configured object list does not match C++ sources for {name}")
        if value(variables, name + "_DEPENDENCIES"):
            raise ValueError(f"local program dependencies need explicit Bazel targets: {name}")
        compile_options = self.options(
            [
                value(variables, "DEFS"), value(variables, "DEFAULT_INCLUDES"), value(variables, "INCLUDES"),
                value(variables, name + "_CPPFLAGS", "AM_CPPFLAGS"), value(variables, "CPPFLAGS"),
                value(variables, name + "_CXXFLAGS", "AM_CXXFLAGS"), value(variables, "CXXFLAGS"),
            ],
            directory,
        )
        link_options = self.options(
            [
                value(variables, name + "_CXXFLAGS", "AM_CXXFLAGS"), value(variables, "CXXFLAGS"),
                value(variables, name + "_LDFLAGS", "AM_LDFLAGS"), value(variables, "LDFLAGS"),
                value(variables, name + "_LDADD", "LDADD"), value(variables, "LIBS"),
            ],
            directory,
            link=True,
        )
        self.programs.append(
            {
                "name": name,
                "directory": directory,
                "sources": sources,
                "compile_options": compile_options,
                "link_options": link_options,
                "install_path": self.installed_path(install_directory, name, False),
            }
        )

    def read_make_tree(self) -> None:
        pending = ["."]
        visited = set()
        while pending:
            directory = pending.pop(0)
            if directory in visited:
                raise ValueError(f"duplicate or recursive configured SUBDIRS entry: {directory}")
            visited.add(directory)
            build_directory = self.configured / directory
            variables = query_make(build_directory)
            top_source = value(variables, "abs_top_srcdir") or value(variables, "top_srcdir")
            if not top_source or (build_directory / top_source).resolve() != self.source:
                raise ValueError(f"configured Makefile does not belong to --source: {build_directory}")
            if value(variables, "EXEEXT"):
                raise ValueError("the SWSS native build contract supports Linux executables only")
            if value(variables, "BUILT_SOURCES"):
                raise ValueError(f"generated sources need explicit Bazel targets: {build_directory}")
            if "-fprofile-arcs" in value(variables, "CFLAGS_COMMON"):
                raise ValueError("the native build contract requires a release configuration without GCOV")
            if value(variables, "host_cpu"):
                self.target_cpus.add(value(variables, "host_cpu"))
            for child in shlex.split(value(variables, "SUBDIRS")):
                child_path = (PurePosixPath(directory) / child)
                relative_path(child_path.as_posix())
                pending.append(child_path.as_posix())
            for variable in sorted(variables):
                # Automake's install commands are not installed data primaries.
                if variable in ("INSTALL_DATA", "install_sh_DATA"):
                    continue
                match = PRIMARY.fullmatch(variable)
                if not match or variable.startswith("am__") or not value(variables, variable):
                    continue
                modifiers, prefix, kind = match.groups()
                if prefix in ("noinst", "check", "EXTRA"):
                    continue
                if kind in ("LIBRARIES", "LTLIBRARIES"):
                    raise ValueError(f"installed libraries need explicit Bazel targets: {variable}")
                install_directory = value(variables, prefix + "dir")
                if not install_directory:
                    raise ValueError(f"missing configured install directory for {variable}")
                for token in shlex.split(value(variables, variable)):
                    if kind == "PROGRAMS":
                        if "nobase_" in modifiers:
                            raise ValueError(f"nobase installed programs are not supported: {variable}")
                        self.read_program(token, directory, install_directory, variables)
                    else:
                        relative = self.resolve_source(token, directory)
                        install_path = self.installed_path(install_directory, token, "nobase_" in modifiers)
                        self.automake_install.append(
                            {
                                "source": relative,
                                "install_path": install_path,
                                "mode": package_mode(install_path, 0o755 if kind == "SCRIPTS" else 0o644),
                            }
                        )
        names = [program["name"] for program in self.programs]
        if not names or len(names) != len(set(names)):
            raise ValueError("configured SWSS programs are empty or have duplicate names")

    def production_inventory(self) -> dict[str, Any]:
        installed = [program["install_path"] for program in self.programs]
        installed += [entry["install_path"] for entry in self.automake_install]
        if len(installed) != len(set(installed)):
            raise ValueError("configured build has duplicate install paths")
        return {
            "programs": sorted(self.programs, key=lambda item: item["name"]),
            "automake_install": sorted(self.automake_install, key=lambda item: item["install_path"]),
        }

    def program_inventory(self, inventory: dict[str, Any]) -> dict[str, Any]:
        def source_label(name: str) -> str:
            parts = relative_path(name).parts
            return "//" + parts[0] + ":" + "/".join(parts[1:]) if len(parts) > 1 else "//:" + parts[0]

        def local_include_directories(program: dict[str, Any]) -> list[str]:
            result = []
            options = program["compile_options"]
            index = 0
            while index < len(options):
                parsed = parse_path_option(options, index)
                if parsed is None:
                    index += 1
                    continue
                prefix, path, index = parsed
                if prefix != "-L" and (path == SOURCE_ROOT or path.startswith(SOURCE_ROOT + "/")):
                    if prefix != "-I":
                        raise ValueError(f"local module include class needs an explicit mapping for {program['name']}: {prefix}")
                    relative = path[len(SOURCE_ROOT):].removeprefix("/")
                    result.append(relative_path(relative).as_posix() if relative else ".")
            return result

        programs = {
            program["name"]: {
                "directory": program["directory"],
                "sources": [source_label(name) for name in program["sources"]],
                "install_path": program["install_path"],
                "local_include_directories": local_include_directories(program),
            }
            for program in inventory["programs"]
        }
        return programs

    def build_contract(self, inventory: dict[str, Any], git_revision: str) -> dict[str, Any]:
        """Describe this runner's native release options for Bazel comparison."""
        def normalized_path(path: str, include: bool = False) -> str:
            if path == SOURCE_ROOT or path.startswith(SOURCE_ROOT + "/"):
                return path
            if include and path in MODULE_INCLUDE_ROOTS:
                return MODULE_INCLUDE_ROOTS[path]
            raise ValueError(f"native path has no module build-contract mapping: {path}")

        def normalized_options(options: list[str], link: bool) -> list[dict[str, str]]:
            result = []
            index = 0
            target_options = {
                option
                for values in MODULE_BUILD_MAPPINGS["target_toolchain_compile_options"].values()
                for option in values
            }
            while index < len(options):
                option = options[index]
                argument = option
                classification = "literal"
                parsed = parse_path_option(options, index)
                if parsed is not None:
                    prefix, path, index = parsed
                    if prefix == "-L":
                        raise ValueError(f"native library search path has no module build-contract mapping: {path}")
                    argument = prefix + normalized_path(path, include=True)
                    classification = "include"
                else:
                    index += 1
                    prefix_map = re.match(r"^(-f(?:debug|file|macro)-prefix-map=)([^=]+)=(.*)$", option)
                    if prefix_map:
                        if Path(prefix_map[3]).is_absolute():
                            raise ValueError(f"native prefix-map destination has no module build-contract mapping: {prefix_map[3]}")
                        argument = prefix_map[1] + normalized_path(prefix_map[2]) + "=" + prefix_map[3]
                        classification = "source_prefix_map"
                    elif link and option.startswith("-l"):
                        if option[2:] not in MODULE_BUILD_MAPPINGS["library_providers"]:
                            raise ValueError(f"native library has no module build-contract provider: {option}")
                        classification = "library"
                    elif link and Path(option).is_absolute():
                        raise ValueError(f"native link input has no module build-contract provider: {option}")
                    elif link and option in MODULE_BUILD_MAPPINGS["native_link_driver_options"]:
                        classification = "native_link_driver"
                    elif link and option in target_options:
                        classification = "target_native_link_driver"
                    elif link and option in MODULE_BUILD_MAPPINGS["toolchain_link_options"]:
                        classification = "toolchain"
                    elif not link and option in MODULE_BUILD_MAPPINGS["native_only_compile_options"]:
                        classification = "native_only"
                    elif not link and option in MODULE_BUILD_MAPPINGS["compile_replacements"]:
                        classification = "replacement"
                    elif not link and option in MODULE_BUILD_MAPPINGS["toolchain_compile_options"]:
                        classification = "toolchain"
                    elif not link and option in target_options:
                        classification = "target_toolchain"
                result.append({"argument": argument, "classification": classification})
            return result

        def option_profiles(key: str, link: bool) -> tuple[dict[str, list[dict[str, str]]], dict[str, str]]:
            profiles: dict[str, list[dict[str, str]]] = {}
            references = {}
            for program in inventory["programs"]:
                options = normalized_options(program[key], link)
                profile = next((name for name, existing in profiles.items() if existing == options), None)
                if profile is None:
                    profile = program["name"]
                    profiles[profile] = options
                references[program["name"]] = profile
            return profiles, references

        programs = self.program_inventory(inventory)
        compile_profiles, compile_references = option_profiles("compile_options", False)
        link_profiles, link_references = option_profiles("link_options", True)
        return {
            "schema_version": 2,
            "git_revision": git_revision,
            "automake_install": inventory["automake_install"],
            "architecture": {
                "execution_machine": platform.machine(),
                "configured_target_cpus": sorted(self.target_cpus),
            },
            "compile_profiles": compile_profiles,
            "link_profiles": link_profiles,
            "mappings": MODULE_BUILD_MAPPINGS,
            "programs": {
                program["name"]: {
                    **programs[program["name"]],
                    "compile_profile": compile_references[program["name"]],
                    "link_profile": link_references[program["name"]],
                }
                for program in inventory["programs"]
            },
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--configured-build", required=True, type=Path)
    parser.add_argument("--output-build-contract", required=True, type=Path, metavar="FILE")
    parser.add_argument("--git-revision", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.git_revision):
        parser.error("--git-revision must be a full Git commit SHA")
    generator = Generator(args.source, args.configured_build)
    generator.read_make_tree()
    contract = generator.build_contract(generator.production_inventory(), args.git_revision)
    args.output_build_contract.parent.mkdir(parents=True, exist_ok=True)
    args.output_build_contract.write_text(json.dumps(contract, indent=2, sort_keys=True) + "\n")
    print(f"Recorded {len(contract['programs'])} native programs in {args.output_build_contract}")


if __name__ == "__main__":
    main()
