"""Read production source selections from the configured Bazel graph."""

from __future__ import annotations

import re
from pathlib import PurePosixPath
from typing import Any


AGGREGATE = "//dist:cpp_binaries"
PRODUCTION_QUERY = AGGREGATE + " union labels(srcs, " + AGGREGATE + ")"
ARCHITECTURES = {
    "amd64": ("x86_64", 62, "@sonic_build_infra//platforms:x86_64_trixie"),
    "arm64": ("aarch64", 183, "@sonic_build_infra//platforms:aarch64_trixie"),
}
DEPENDENCY_ATTRIBUTES = ("actual", "deps", "dynamic_deps", "exports", "implementation_deps")


def program_labels(programs: dict[str, dict[str, Any]]) -> dict[str, str]:
    return {
        name: f"//{'' if program['directory'] == '.' else program['directory']}:{name}"
        for name, program in sorted(programs.items())
    }


def canonical_label(label: str, repository_mapping: dict[str, str]) -> str:
    if label.startswith("@@"):
        return label
    if label.startswith("//"):
        return "@@" + label
    match = re.fullmatch(r"@([^/]+)(//.+)", label)
    if match is None or match[1] not in repository_mapping:
        raise ValueError(f"label has no root repository mapping: {label}")
    return "@@" + repository_mapping[match[1]] + match[2]


def mapped_labels(mappings: dict[str, Any]) -> list[str]:
    system = mappings["implicit_system_header_providers"]
    labels = [label for label in mappings["library_providers"].values() if not label.startswith("toolchain:")]
    labels += system["all_programs"]
    labels += [label for values in system["programs"].values() for label in values]
    labels += mappings["local_header_providers"] + mappings["rule_support_providers"]
    return sorted(set(labels))


def cquery_expression(programs: dict[str, dict[str, Any]], mappings: dict[str, Any]) -> str:
    selected = "set(" + " ".join(program_labels(programs).values()) + ")"
    endpoints = "set(" + " ".join(mapped_labels(mappings)) + ")"
    return selected + " union allpaths(" + selected + ", " + endpoints + ")"


def list_attribute(rule: dict[str, Any], name: str) -> list[str]:
    attribute = rule["attributes"].get(name)
    if attribute is None:
        return []
    values = attribute.get("stringListValue", [])
    if attribute.get("type") not in ("LABEL_LIST", "STRING_LIST") or not isinstance(values, list) or not all(isinstance(value, str) for value in values):
        raise ValueError(f"invalid {name} attribute for {rule['name']}")
    return values


def rule_graph(evidence: dict[str, Any]) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    data = evidence["cquery"]
    configurations = {str(item["id"]): item for item in data["configurations"]}
    if len(configurations) != len(data["configurations"]):
        raise ValueError("duplicate cquery configuration ID")
    rules = {}
    for item in data["results"]:
        rule = item["target"].get("rule")
        if not isinstance(rule, dict) or not rule.get("name", "").startswith("@@"):
            raise ValueError("configured-rule evidence must contain consistently labeled rules")
        name = rule["name"]
        attributes = {attribute["name"]: attribute for attribute in rule.get("attribute", [])}
        if len(attributes) != len(rule.get("attribute", [])):
            raise ValueError(f"duplicate configured attribute for {name}")
        configuration = str(item.get("configurationId"))
        if configuration not in configurations:
            raise ValueError(f"missing configured identity for {name}")
        edges = set()
        for key in DEPENDENCY_ATTRIBUTES:
            attribute = attributes.get(key, {})
            if attribute.get("type") == "LABEL":
                if attribute.get("stringValue"):
                    edges.add(attribute["stringValue"])
            elif attribute.get("type") == "LABEL_LIST":
                edges.update(attribute.get("stringListValue", []))
        if not all(isinstance(edge, str) and edge.startswith("@@") for edge in edges):
            raise ValueError(f"inconsistent dependency labels for {name}")
        if name in rules:
            previous = rules[name]
            actual = attributes.get("actual", {})
            previous_actual = previous["attributes"].get("actual", {})
            if (
                previous["kind"] != "alias" or rule["ruleClass"] != "alias"
                or actual.get("type") != "LABEL" or edges != {actual.get("stringValue")}
                or previous_actual.get("type") != "LABEL" or previous["edges"] != {previous_actual.get("stringValue")}
                or previous["edges"] != edges or configuration in previous["configurations"]
            ):
                raise ValueError(f"selected label has multiple configured results: {name}")
            previous["configurations"].add(configuration)
            continue
        rules[name] = {"name": name, "kind": rule["ruleClass"], "attributes": attributes, "configuration": configuration, "configurations": {configuration}, "edges": edges}
    return rules, configurations


def require_release_configuration(rule: dict[str, Any], configurations: dict[str, dict[str, Any]], architecture: str, repository_mapping: dict[str, str]) -> None:
    configuration = configurations[rule["configuration"]]
    options = {
        option["name"]: option.get("value")
        for fragment in configuration.get("fragmentOptions", [])
        for option in fragment.get("options", [])
    }
    platform = canonical_label(ARCHITECTURES[architecture][2], repository_mapping)
    if options.get("compilation_mode") != "opt" or options.get("platforms") != "[" + platform + "]":
        raise ValueError(f"production rule is not in the selected release platform: {rule['name']}")
    if any(options.get(key) != "[]" for key in ("copt", "cxxopt", "linkopt", "per_file_copt", "features")):
        raise ValueError(f"production rule has unclassified command-line options: {rule['name']}")


def local_label(label: str) -> tuple[str, str]:
    match = re.fullmatch(r"@@//([^:]*):(.+)", label)
    if match is None:
        raise ValueError(f"production inputs must use local file or target labels: {label}")
    package, name = match.groups()
    for path in (package, name):
        parsed = PurePosixPath(path)
        if parsed.is_absolute() or ".." in parsed.parts or (path and parsed.as_posix() != path):
            raise ValueError(f"production label contains an invalid path: {label}")
    return package, name


def parse_program_graph(evidence: dict[str, Any], architecture: str) -> dict[str, dict[str, Any]]:
    """Discover only the configured binaries declared by the production aggregate."""
    if evidence.get("schema_version") != 1 or evidence.get("query") != PRODUCTION_QUERY:
        raise ValueError("production graph does not match the configured aggregate query")
    rules, configurations = rule_graph(evidence)
    aggregate_label = canonical_label(AGGREGATE, {})
    aggregate = rules.get(aggregate_label)
    if aggregate is None or aggregate["kind"] != "filegroup":
        raise ValueError("production graph must contain the cpp_binaries filegroup")
    selected = list_attribute(aggregate, "srcs")
    if not selected or len(selected) != len(set(selected)):
        raise ValueError("production aggregate must select nonempty, unique program labels")
    if set(rules) != {aggregate_label, *selected}:
        raise ValueError("production graph contains missing or extra aggregate members")
    programs = {}
    for label in selected:
        rule = rules[label]
        package, name = local_label(label)
        if rule["kind"] != "cc_binary" or not re.fullmatch(r"[A-Za-z0-9_+.-]+", name) or name in programs:
            raise ValueError(f"production aggregate must select uniquely named local cc_binary rules: {label}")
        require_release_configuration(rule, configurations, architecture, evidence["root_repository_mapping"])
        sources = list_attribute(rule, "srcs")
        if not sources or len(sources) != len(set(sources)):
            raise ValueError(f"production sources must be nonempty and unique: {label}")
        for source in sources:
            _, filename = local_label(source)
            if not filename.endswith((".cc", ".cpp", ".cxx", ".C")):
                raise ValueError(f"production source must be a C++ file label: {source}")
        programs[name] = {
            "directory": package or ".",
            "sources": sorted(source.removeprefix("@@") for source in sources),
        }
    return dict(sorted(programs.items()))


def validate_program_inventory(expected: dict[str, dict[str, Any]], observed: dict[str, dict[str, Any]]) -> None:
    """Compare independently collected native and configured Bazel selections."""
    expected_labels = program_labels(expected)
    actual_labels = program_labels(observed)
    if expected_labels != actual_labels:
        raise ValueError(
            "native and Bazel production program sets differ: "
            f"expected={expected_labels}, actual={actual_labels}"
        )
    for name in expected:
        sources = expected[name]["sources"]
        if len(sources) != len(set(sources)) or set(sources) != set(observed[name]["sources"]):
            raise ValueError(
                f"native and Bazel production sources differ for {name}: "
                f"missing={sorted(set(sources) - set(observed[name]['sources']))}, "
                f"extra={sorted(set(observed[name]['sources']) - set(sources))}"
            )
