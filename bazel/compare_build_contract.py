#!/usr/bin/env python3
"""Compare native release settings with resolved production Bazel rules.

This deliberately checks target attributes and dependency-label reachability.
It does not inspect toolchain actions, response files, or provider paths.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
from typing import Any

from ci_production import build_definitions_sha256, checkout_revision, load_retained_graph, receipt_artifact, sha256
from production_graph import (
    ARCHITECTURES, canonical_label, cquery_expression, list_attribute, mapped_labels,
    program_labels, require_release_configuration, rule_graph, validate_program_inventory,
)
from native_build_contract import MODULE_BUILD_MAPPINGS


UNVERIFIED_SCOPE = [
    "Toolchain actions and command-line response files are not inspected.",
    "Transitive provider include, header, and library paths are not inspected.",
    "Repeated aliases with the same resolved actual label are collapsed for label reachability; configuration-specific provider behavior is not inspected.",
    "External header search precedence and native link-order/runtime parity are not established.",
    "DEBUG, ASAN, GCOV, Rust, Cargo, and runtime targets are outside this normal-release comparison.",
]
INTENTIONAL_DIFFERENCES = {
    "native_config_header": "The selected production sources do not include config.h, so target cxxopts omit -DHAVE_CONFIG_H.",
    "target_fortification": "The target uses fortification level 3 in place of the standalone native configure profile's level 2.",
    "target_warnings": "The existing -Wformat=2 and -Werror controls cover the mapped native warning options; Bazel also disables missing-include-directory warnings.",
    "native_link_driver_flags": "Mapped ordinary compile flags repeated on the native link driver are omitted from target linkopts.",
    "source_prefix_maps": "Native source prefix maps are not target cxxopts; this check does not verify their toolchain-action equivalent.",
    "reviewed_toolchain": "Mapped optimization and hardening options, including the recorded additional link options, are accepted for the exact reviewed infra version without inspecting its actions.",
    "dependency_labels": "Native libraries and system include roots map to reviewed reachable labels; provider paths and link ordering are outside this check.",
    "local_header_inputs": "Named production header targets add declared inputs without propagating include directories or options.",
    "rule_support": "The named rules_cc link support label is retained separately from native libraries.",
}


def reachable(rules: dict[str, dict[str, Any]], start: str) -> set[str]:
    found = set()
    pending = [start]
    while pending:
        label = pending.pop()
        if label in found:
            continue
        found.add(label)
        if label in rules:
            pending.extend(rules[label]["edges"])
    return found


def normalize_compile_options(options: list[str]) -> dict[str, Any]:
    """Normalize only the repeated option families used by this release."""
    states = {}
    literals = []
    for option in options:
        macro = re.fullmatch(r"-([DU])([A-Za-z_][A-Za-z0-9_]*)(?:=(.*))?", option)
        if macro:
            states["macro:" + macro[2]] = option
        elif option in ("-g", "-g0", "-g1", "-g2", "-g3", "-ggdb"):
            states["debug"] = option
        elif option.startswith("-std="):
            states["standard"] = option
        elif option in ("-fPIC", "-fpic", "-fno-PIC", "-fno-pic"):
            states["pic"] = option
        elif option.startswith("-W") and not option.startswith("-Wl,"):
            warning = option[2:]
            disabled = warning.startswith("no-")
            if disabled:
                warning = warning[3:]
            if warning.startswith("error="):
                key = "warning-error:" + warning[6:]
            else:
                key = "warning:" + warning.split("=", 1)[0]
            states[key] = ("disabled:" if disabled else "enabled:") + warning
        else:
            literals.append(option)
    return {"states": states, "ordered_literals": literals}


def native_options(native: dict[str, Any], name: str, cpu: str) -> tuple[list[str], list[str], list[str], set[str], set[str]]:
    mappings = native["mappings"]
    program = native["programs"][name]
    compile_options = []
    local_includes = []
    include_repositories = set()
    for record in native["compile_profiles"][program["compile_profile"]]:
        option, kind = record["argument"], record["classification"]
        if kind == "include":
            match = re.fullmatch(r"(-I|-isystem|-iquote|-idirafter)(<[^>]+>)(/.*)?", option)
            if match is None or match[2] not in mappings["include_roots"]:
                raise ValueError(f"unmapped native include for {name}: {option}")
            if match[2] == "<swss>":
                if match[1] != "-I":
                    raise ValueError(f"unsupported native local include class for {name}: {option}")
                local_includes.append(match[3][1:] if match[3] else ".")
            elif mappings["include_roots"][match[2]].startswith("@"):
                include_repositories.add(mappings["include_roots"][match[2]])
        elif kind == "literal":
            compile_options.append(option)
        elif kind == "replacement" and option in mappings["compile_replacements"]:
            compile_options.extend(mappings["compile_replacements"][option])
        elif kind == "native_only" and option in mappings["native_only_compile_options"]:
            pass
        elif kind == "toolchain" and option in mappings["toolchain_compile_options"]:
            pass
        elif kind == "target_toolchain" and option in mappings["target_toolchain_compile_options"].get(cpu, []):
            pass
        elif kind == "source_prefix_map" and option.split("=", 1)[0] in mappings["source_prefix_maps"]:
            pass
        else:
            raise ValueError(f"unclassified native compile option for {name}: {record}")
    compile_options.extend(mappings["bazel_only_compile_options"])
    link_options = []
    libraries = set()
    for record in native["link_profiles"][program["link_profile"]]:
        option, kind = record["argument"], record["classification"]
        if kind == "library" and option.startswith("-l") and option[2:] in mappings["library_providers"]:
            label = mappings["library_providers"][option[2:]]
            if not label.startswith("toolchain:"):
                libraries.add(label)
        elif kind == "literal":
            link_options.append(option)
        elif kind == "native_link_driver" and option in mappings["native_link_driver_options"]:
            pass
        elif kind == "target_native_link_driver" and option in mappings["target_toolchain_compile_options"].get(cpu, []):
            pass
        elif kind == "toolchain" and option in mappings["toolchain_link_options"]:
            pass
        elif kind == "source_prefix_map" and option.split("=", 1)[0] in mappings["source_prefix_maps"]:
            pass
        else:
            raise ValueError(f"unclassified native link option for {name}: {record}")
    link_options.extend(mappings.get("bazel_only_link_options", []))
    return compile_options, link_options, local_includes, libraries, include_repositories


def target_compile_options(rule: dict[str, Any]) -> tuple[list[str], list[str]]:
    options = list_attribute(rule, "cxxopts")
    semantic = []
    includes = []
    index = 0
    while index < len(options):
        option = options[index]
        for prefix in ("-isystem", "-iquote", "-idirafter", "-I"):
            if option == prefix:
                index += 1
                if index == len(options):
                    raise ValueError(f"missing target include path for {rule['name']}")
                path = options[index]
            elif option.startswith(prefix) and len(option) > len(prefix):
                path = option[len(prefix):]
            else:
                continue
            if prefix != "-I":
                raise ValueError(f"unexpected target include class for {rule['name']}: {prefix}")
            includes.append(path)
            break
        else:
            semantic.append(option)
        index += 1
    return semantic, includes


def require_input_only_headers(rule: dict[str, Any]) -> None:
    if rule["kind"] != "cc_library":
        raise ValueError(f"local production headers must be a cc_library: {rule['name']}")
    for name in ("srcs", "deps", "copts", "cxxopts", "linkopts", "includes", "defines", "local_defines", "features"):
        if list_attribute(rule, name):
            raise ValueError(f"local production headers propagate {name}: {rule['name']}")
    for name in ("include_prefix", "strip_include_prefix"):
        if rule["attributes"].get(name, {}).get("stringValue", ""):
            raise ValueError(f"local production headers propagate {name}: {rule['name']}")
    package = rule["name"].split(":", 1)[0] + ":"
    if any(not label.startswith(package) for label in list_attribute(rule, "hdrs")):
        raise ValueError(f"local production headers leave their package: {rule['name']}")


def check_configured_rules(native: dict[str, Any], evidence: dict[str, Any], programs: dict[str, dict[str, Any]], architecture: str) -> list[dict[str, Any]]:
    validate_program_inventory(native["programs"], programs)
    programs = native["programs"]
    mappings = native["mappings"]
    labels = program_labels(programs)
    if evidence.get("schema_version") != 1 or evidence.get("query") != cquery_expression(programs, mappings) or evidence.get("universe_scope") != list(labels.values()):
        raise ValueError("configured-rule evidence does not match the selected production query")
    repository_mapping = evidence["root_repository_mapping"]
    canonical = lambda label: canonical_label(label, repository_mapping)
    rules, configurations = rule_graph(evidence)
    local_headers = {canonical(label) for label in mappings["local_header_providers"]}
    rule_support = {canonical(label) for label in mappings["rule_support_providers"]}
    provider_labels = {label: canonical(label) for label in mapped_labels(mappings)}
    feature_sources = {canonical(label) for feature in mappings["feature_support"].values() for label in feature["source_labels"]}
    feature_dependencies = {canonical(label) for feature in mappings["feature_support"].values() for label in feature["dependency_labels"]}
    cpu = ARCHITECTURES[architecture][0]
    checked = []
    for name, program in programs.items():
        label = canonical(labels[name])
        rule = rules.get(label)
        if rule is None or rule["kind"] != "cc_binary":
            raise ValueError(f"missing resolved production cc_binary: {label}")
        configuration = configurations[rule["configuration"]]
        require_release_configuration(rule, configurations, architecture, repository_mapping)
        sources = set(list_attribute(rule, "srcs"))
        if sources & feature_sources:
            raise ValueError(f"feature support sources are selected in normal release: {label}")
        if sources != {canonical(source) for source in program["sources"]}:
            raise ValueError(f"resolved production sources differ from the native inventory: {label}")
        for attribute in ("copts", "defines", "local_defines", "includes", "features"):
            if list_attribute(rule, attribute):
                raise ValueError(f"production rule uses unclassified {attribute}: {label}")
        expected_compile, expected_link, expected_includes, libraries, include_repositories = native_options(native, name, cpu)
        if expected_includes != program["local_include_directories"]:
            raise ValueError(f"native local includes differ from the declared inventory: {name}")
        actual_compile, actual_includes = target_compile_options(rule)
        if actual_includes != expected_includes:
            raise ValueError(f"resolved local include order differs from native: {label}")
        actual_normalized = normalize_compile_options(actual_compile)
        expected_normalized = normalize_compile_options(expected_compile)
        if actual_normalized != expected_normalized:
            differences = {}
            states = {
                key: {"expected": expected_normalized["states"].get(key), "actual": actual_normalized["states"].get(key)}
                for key in sorted(expected_normalized["states"].keys() | actual_normalized["states"].keys())
                if expected_normalized["states"].get(key) != actual_normalized["states"].get(key)
            }
            if states:
                differences["states"] = states
            if actual_normalized["ordered_literals"] != expected_normalized["ordered_literals"]:
                differences["ordered_literals"] = {"expected": expected_normalized["ordered_literals"], "actual": actual_normalized["ordered_literals"]}
            raise ValueError(f"resolved target cxxopts differ from native mapping: {label}; differences={json.dumps(differences, sort_keys=True)}")
        if list_attribute(rule, "linkopts") != expected_link:
            raise ValueError(f"resolved target linkopts differ from native mapping: {label}")
        system = mappings["implicit_system_header_providers"]
        implicit = set(system["all_programs"] + system["programs"].get(name, []))
        required = {canonical(value) for value in libraries | implicit}
        available = reachable(rules, label)
        missing = required - (available & rules.keys())
        if missing:
            raise ValueError(f"mapped dependencies are not reachable from {label}: {sorted(missing)}")
        for repository in include_repositories:
            candidates = {value for public, value in provider_labels.items() if public.startswith(repository + "//")}
            if not candidates & available & rules.keys():
                raise ValueError(f"native include repository is not reachable from {label}: {repository}")
        accepted_providers = set().union(*(reachable(rules, dependency) for dependency in required)) if required else set()
        accepted_providers &= set(provider_labels.values()) & rules.keys()
        dependencies = set(list_attribute(rule, "deps"))
        if dependencies & feature_dependencies:
            raise ValueError(f"feature support dependency is selected in normal release: {label}")
        unclassified = dependencies - local_headers - rule_support - accepted_providers
        if unclassified:
            raise ValueError(f"unclassified direct production dependencies for {label}: {sorted(unclassified)}")
        for header in dependencies & local_headers:
            if header not in rules:
                raise ValueError(f"missing local production header rule: {header}")
            require_input_only_headers(rules[header])
        checked.append({
            "name": name,
            "label": labels[name],
            "configuration_checksum": configuration["checksum"],
            "local_include_directories": expected_includes,
            "required_dependency_labels": sorted(libraries),
            "implicit_system_header_labels": sorted(implicit),
            "local_header_labels": sorted(dependencies & local_headers),
            "rule_support_labels": sorted(dependencies & rule_support),
        })
    return checked


def validate_provenance(native: dict[str, Any], receipt: dict[str, Any], module_graph: dict[str, Any], architecture: str, revision: str, definitions_sha256: str) -> None:
    cpu, _, platform = ARCHITECTURES[architecture]
    if native.get("schema_version") != 2 or native.get("mappings") != MODULE_BUILD_MAPPINGS:
        raise ValueError("native build contract uses a different schema or mapping")
    if native.get("git_revision") != revision or receipt.get("git_revision") != revision:
        raise ValueError("native contract, build receipt, and checkout revisions must match")
    if receipt.get("build_definitions_sha256") != definitions_sha256:
        raise ValueError("build receipt and checkout build definitions must match")
    if receipt.get("architecture") != architecture or receipt.get("target_platform") != platform or native.get("architecture") != {"execution_machine": cpu, "configured_target_cpus": [cpu]}:
        raise ValueError("native contract and build receipt must use the selected native architecture")
    reviewed = native["mappings"]["reviewed_infra"]
    versions = set()
    pending = [module_graph]
    while pending:
        node = pending.pop()
        if node.get("name") == reviewed["module"]:
            versions.add(node.get("version"))
        pending.extend(node.get("dependencies", []) + node.get("indirectDependencies", []))
    if versions != {reviewed["version"]}:
        raise ValueError(f"toolchain mappings require the reviewed infra version: {reviewed}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--architecture", required=True, choices=sorted(ARCHITECTURES))
    parser.add_argument("--native-contract", required=True, type=Path)
    parser.add_argument("--configured-rules", required=True, type=Path)
    parser.add_argument("--build-receipt", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    receipt = json.loads(args.build_receipt.read_text())
    native_path = receipt_artifact(args.build_receipt, receipt, "native_contract", args.native_contract)
    native = json.loads(native_path.read_text())
    rules_path = receipt_artifact(args.build_receipt, receipt, "configured_rules", args.configured_rules)
    module_path = receipt_artifact(args.build_receipt, receipt, "module_graph")
    evidence = json.loads(rules_path.read_text())
    module_graph = json.loads(module_path.read_text())
    revision = checkout_revision()
    definitions_sha256 = build_definitions_sha256()
    validate_provenance(native, receipt, module_graph, args.architecture, revision, definitions_sha256)
    _, programs = load_retained_graph(args.build_receipt, receipt)
    graph_path = receipt_artifact(args.build_receipt, receipt, "production_graph")
    checked = check_configured_rules(native, evidence, programs, args.architecture)
    result = {
        "schema_version": 2,
        "status": "passed",
        "verification_scope": "Configured normal-release production cc_binary target sources, local include order, cxxopts, linkopts, and mapped dependency-label reachability.",
        "unverified_scope": UNVERIFIED_SCOPE,
        "intentional_differences": INTENTIONAL_DIFFERENCES,
        "git_revision": revision,
        "build_definitions_sha256": definitions_sha256,
        "architecture": args.architecture,
        "reviewed_infra": native["mappings"]["reviewed_infra"],
        "inputs": {
            key: {"artifact": path.name, "sha256": sha256(path)}
            for key, path in {
                "native_contract": native_path,
                "production_graph": graph_path,
                "configured_rules": rules_path,
                "build_receipt": args.build_receipt,
                "module_graph": module_path,
            }.items()
        },
        "programs": checked,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
