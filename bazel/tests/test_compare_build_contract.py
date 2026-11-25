#!/usr/bin/env python3
"""Focused regressions for the configured production rule comparison."""

from __future__ import annotations

import ast
import copy
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest


BAZEL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BAZEL_DIRECTORY))
import compare_build_contract as compare  # noqa: E402


class Fixture:
    def __init__(self):
        self.programs = {
            "example": {
                "directory": "app",
                "sources": ["//app:main.cpp"],
                "local_include_directories": ["app", ".", "app"],
            },
        }
        mappings = copy.deepcopy(compare.MODULE_BUILD_MAPPINGS)
        self.native = {
            "schema_version": 2,
            "git_revision": "a" * 40,
            "architecture": {"execution_machine": "x86_64", "configured_target_cpus": ["x86_64"]},
            "mappings": mappings,
            "programs": {"example": {**copy.deepcopy(self.programs["example"]), "compile_profile": "example", "link_profile": "example"}},
            "compile_profiles": {"example": [
                {"argument": option, "classification": kind}
                for option, kind in [
                    ("-DHAVE_CONFIG_H", "native_only"),
                    ("-I<swss>/app", "include"),
                    ("-I<swss>", "include"),
                    ("-I<swss>/app", "include"),
                    ("-I<system-headers>", "include"),
                    ("-I<sai-headers>", "include"),
                    ("-g", "literal"),
                    ("-O2", "toolchain"),
                    ("-fcf-protection", "target_toolchain"),
                    ("-std=c++17", "literal"),
                    ("-Wall", "literal"),
                    ("-Wformat=2", "literal"),
                    ("-Werror", "literal"),
                    ("-fPIC", "literal"),
                    ("-Wformat", "replacement"),
                    ("-Werror=format-security", "replacement"),
                    ("-D_FORTIFY_SOURCE=2", "replacement"),
                    ("-Wdate-time", "literal"),
                    ("-ffile-prefix-map=<swss>=.", "source_prefix_map"),
                    ("-g", "literal"),
                ]
            ]},
            "link_profiles": {"example": [
                {"argument": option, "classification": kind}
                for option, kind in [
                    ("-g", "native_link_driver"),
                    ("-fcf-protection", "target_native_link_driver"),
                    ("-Wl,-z,relro", "toolchain"),
                    ("-Wl,--no-undefined", "literal"),
                    ("-lsaimeta", "library"),
                    ("-lsaimetadata", "library"),
                    ("-lhiredis", "library"),
                    ("-lm", "library"),
                ]
            ]},
        }
        public_labels = compare.mapped_labels(mappings) + [compare.ARCHITECTURES["amd64"][2]]
        self.repository_mapping = {
            match[1]: match[1] + "+"
            for label in public_labels
            if (match := re.match(r"@([^/]+)//", label))
        }
        library = mappings["library_providers"]
        system = mappings["implicit_system_header_providers"]["all_programs"]
        self.evidence = {
            "schema_version": 1,
            "query": compare.cquery_expression(self.programs, mappings),
            "universe_scope": list(compare.program_labels(self.programs).values()),
            "root_repository_mapping": self.repository_mapping,
            "cquery": {
                "configurations": [{
                    "id": 1,
                    "checksum": "c" * 64,
                    "fragmentOptions": [{"options": [
                        {"name": "compilation_mode", "value": "opt"},
                        {"name": "platforms", "value": "[" + self.canonical(compare.ARCHITECTURES["amd64"][2]) + "]"},
                        *({"name": name, "value": "[]"} for name in ("copt", "cxxopt", "linkopt", "per_file_copt", "features")),
                    ]}],
                }],
                "results": [
                    self.rule("//app:example", "cc_binary", {
                        "srcs": ["//app:main.cpp"],
                        "cxxopts": [
                            "-std=c++17", "-Wall", "-Wformat=2", "-Werror", "-fPIC",
                            "-Wno-missing-include-dirs", "-g", "-Iapp", "-I.", "-Iapp",
                            "-Wdate-time", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=3",
                        ],
                        "linkopts": ["-Wl,--no-undefined"] + mappings.get("bazel_only_link_options", []),
                        "deps": [library["saimeta"], library["hiredis"], *system, "//lib:production_headers", "@rules_cc//:link_extra_lib"],
                    }),
                    self.rule("//lib:production_headers", "cc_library", {"hdrs": ["//lib:common.h"]}),
                    self.rule(library["saimeta"], "cc_library", {"deps": [library["saimetadata"]]}),
                    *(self.rule(label, "cc_library") for label in [library["saimetadata"], library["hiredis"], *system, "@rules_cc//:link_extra_lib"]),
                ],
            },
        }

    def canonical(self, label):
        return compare.canonical_label(label, self.repository_mapping)

    def rule(self, label, kind, attributes=None):
        attributes = attributes or {}
        return {
            "target": {"rule": {
                "name": self.canonical(label),
                "ruleClass": kind,
                "attribute": [
                    {
                        "name": name,
                        "type": "LABEL_LIST" if name in ("srcs", "hdrs", "deps") else "STRING_LIST",
                        "stringListValue": [self.canonical(value) for value in values] if name in ("srcs", "hdrs", "deps") else values,
                    }
                    for name, values in attributes.items()
                ],
            }},
            "configurationId": 1,
        }

    def attribute(self, label, name):
        rule = next(item["target"]["rule"] for item in self.evidence["cquery"]["results"] if item["target"]["rule"]["name"] == self.canonical(label))
        attribute = next((item for item in rule["attribute"] if item["name"] == name), None)
        if attribute is None:
            attribute = {"name": name, "type": "STRING_LIST", "stringListValue": []}
            rule["attribute"].append(attribute)
        return attribute["stringListValue"]

    def check(self):
        return compare.check_configured_rules(self.native, self.evidence, self.programs, "amd64")


class BuildContractTest(unittest.TestCase):
    def test_checks_target_settings_and_transitive_native_library(self):
        fixture = Fixture()
        result = fixture.check()
        self.assertEqual(result[0]["local_include_directories"], ["app", ".", "app"])
        self.assertIn("@sonic_sairedis//meta:saimetadata_shared", result[0]["required_dependency_labels"])

    def test_libteam_and_teamdctl_require_their_separate_source_providers(self):
        fixture = Fixture()
        for library in ("team", "teamdctl"):
            label = fixture.native["mappings"]["library_providers"][library]
            fixture.native["link_profiles"]["example"].append({"argument": "-l" + library, "classification": "library"})
            fixture.attribute("//app:example", "deps").append(fixture.canonical(label))
            fixture.evidence["cquery"]["results"].append(fixture.rule(label, "cc_library"))
        required = fixture.check()[0]["required_dependency_labels"]
        self.assertIn("@libteam//:libteam", required)
        self.assertIn("@libteam//:libteamdctl", required)
        fixture.attribute("//app:example", "deps").remove(fixture.canonical("@libteam//:libteamdctl"))
        with self.assertRaisesRegex(ValueError, "mapped dependencies are not reachable"):
            fixture.check()

    def test_detects_native_and_bazel_option_drift(self):
        for side, kind in (("native", "compile"), ("bazel", "compile"), ("native", "link"), ("bazel", "link")):
            with self.subTest(side=side, kind=kind):
                fixture = Fixture()
                option = "-DNEW_FEATURE=1" if kind == "compile" else "-Wl,--gc-sections"
                if side == "native":
                    fixture.native[kind + "_profiles"]["example"].append({"argument": option, "classification": "literal"})
                else:
                    fixture.attribute("//app:example", "cxxopts" if kind == "compile" else "linkopts").append(option)
                with self.assertRaisesRegex(ValueError, "resolved target .* differ from native mapping") as raised:
                    fixture.check()
                if kind == "compile":
                    self.assertIn('"expected":', str(raised.exception))
                    self.assertIn('"actual":', str(raised.exception))
                    self.assertIn(option, str(raised.exception))

    def test_requires_present_reachable_providers_and_classified_direct_labels(self):
        fixture = Fixture()
        metadata = fixture.canonical("@sonic_sairedis//meta:saimetadata_shared")
        fixture.evidence["cquery"]["results"] = [item for item in fixture.evidence["cquery"]["results"] if item["target"]["rule"]["name"] != metadata]
        with self.assertRaisesRegex(ValueError, "mapped dependencies are not reachable"):
            fixture.check()
        fixture = Fixture()
        fixture.attribute("//app:example", "deps").append("@@unmapped+//:extra")
        with self.assertRaisesRegex(ValueError, "unclassified direct production dependencies"):
            fixture.check()
        fixture = Fixture()
        fixture.native["link_profiles"]["example"].append({"argument": "-lnew", "classification": "library"})
        with self.assertRaisesRegex(ValueError, "unclassified native link option"):
            fixture.check()

    def test_checks_local_include_order_class_and_header_options(self):
        fixture = Fixture()
        options = fixture.attribute("//app:example", "cxxopts")
        options[options.index("-I.")] = "-Iapp"
        with self.assertRaisesRegex(ValueError, "resolved local include order differs"):
            fixture.check()
        fixture = Fixture()
        options = fixture.attribute("//app:example", "cxxopts")
        options[options.index("-I.")] = "-isystem."
        with self.assertRaisesRegex(ValueError, "unexpected target include class"):
            fixture.check()
        fixture = Fixture()
        fixture.attribute("//lib:production_headers", "includes").append("lib")
        with self.assertRaisesRegex(ValueError, "local production headers propagate includes"):
            fixture.check()

    def test_rejects_feature_selection_in_normal_release(self):
        fixture = Fixture()
        fixture.attribute("//app:example", "srcs").append(fixture.canonical("//lib:asan.cpp"))
        with self.assertRaisesRegex(ValueError, "feature support sources are selected"):
            fixture.check()
        fixture = Fixture()
        fixture.attribute("//app:example", "deps").append(fixture.canonical("//gcovpreload:gcovpreload_shared"))
        with self.assertRaisesRegex(ValueError, "feature support dependency is selected"):
            fixture.check()

    def test_requires_unique_release_configuration(self):
        fixture = Fixture()
        fixture.evidence["cquery"]["results"].append(copy.deepcopy(fixture.evidence["cquery"]["results"][0]))
        with self.assertRaisesRegex(ValueError, "multiple configured results"):
            fixture.check()
        fixture = Fixture()
        options = fixture.evidence["cquery"]["configurations"][0]["fragmentOptions"][0]["options"]
        next(item for item in options if item["name"] == "cxxopt")["value"] = "[-DGLOBAL]"
        with self.assertRaisesRegex(ValueError, "unclassified command-line options"):
            fixture.check()

    def test_collapses_only_aliases_with_the_same_resolved_edge(self):
        fixture = Fixture()
        hiredis = fixture.canonical(fixture.native["mappings"]["library_providers"]["hiredis"])
        provider = next(item for item in fixture.evidence["cquery"]["results"] if item["target"]["rule"]["name"] == hiredis)
        provider["target"]["rule"]["ruleClass"] = "alias"
        provider["target"]["rule"]["attribute"] = [{"name": "actual", "type": "LABEL", "stringValue": "@@provider+//:actual"}]
        duplicate = copy.deepcopy(provider)
        duplicate["configurationId"] = 2
        fixture.evidence["cquery"]["results"].append(duplicate)
        configuration = copy.deepcopy(fixture.evidence["cquery"]["configurations"][0])
        configuration.update({"id": 2, "checksum": "d" * 64})
        fixture.evidence["cquery"]["configurations"].append(configuration)
        fixture.check()
        duplicate["target"]["rule"]["attribute"][0]["stringValue"] = "@@provider+//:different"
        with self.assertRaisesRegex(ValueError, "multiple configured results"):
            fixture.check()

    def test_target_hardening_roles_do_not_cross_architecture(self):
        for cpu, option, other_cpu in (
            ("x86_64", "-fcf-protection", "aarch64"),
            ("aarch64", "-mbranch-protection=standard", "x86_64"),
        ):
            with self.subTest(cpu=cpu):
                fixture = Fixture()
                for profile in ("compile_profiles", "link_profiles"):
                    for record in fixture.native[profile]["example"]:
                        if record["classification"].startswith("target_"):
                            record["argument"] = option
                compare.native_options(fixture.native, "example", cpu)
                with self.assertRaisesRegex(ValueError, "unclassified native compile option"):
                    compare.native_options(fixture.native, "example", other_cpu)
                fixture.native["compile_profiles"]["example"] = [record for record in fixture.native["compile_profiles"]["example"] if record["classification"] != "target_toolchain"]
                with self.assertRaisesRegex(ValueError, "unclassified native link option"):
                    compare.native_options(fixture.native, "example", other_cpu)

    def test_reviewed_infra_matches_root_toolchain_selection(self):
        reviewed = compare.MODULE_BUILD_MAPPINGS["reviewed_infra"]
        module = (BAZEL_DIRECTORY.parent / "MODULE.bazel").read_text()
        pins = {}
        for node in ast.walk(ast.parse(module)):
            if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                continue
            if node.func.id not in ("bazel_dep", "single_version_override"):
                continue
            args = {kw.arg: kw.value.value for kw in node.keywords if isinstance(kw.value, ast.Constant)}
            if args.get("module_name", args.get("name")) == reviewed["module"]:
                pins[node.func.id] = args["version"]
        self.assertEqual(pins.get("single_version_override", pins.get("bazel_dep")), reviewed["version"])

    def test_checks_revision_build_definitions_architecture_and_reviewed_infra(self):
        fixture = Fixture()
        receipt = {
            "git_revision": "a" * 40,
            "build_definitions_sha256": "b" * 64,
            "architecture": "amd64",
            "target_platform": compare.ARCHITECTURES["amd64"][2],
        }
        reviewed = fixture.native["mappings"]["reviewed_infra"]
        module_graph = {"name": "sonic-swss", "dependencies": [{"name": reviewed["module"], "version": reviewed["version"]}]}
        compare.validate_provenance(fixture.native, receipt, module_graph, "amd64", "a" * 40, "b" * 64)
        for key, value, error in (
            ("git_revision", "d" * 40, "revisions must match"),
            ("architecture", {"execution_machine": "aarch64", "configured_target_cpus": ["aarch64"]}, "selected native architecture"),
        ):
            with self.subTest(key=key):
                changed = copy.deepcopy(fixture.native)
                changed[key] = value
                with self.assertRaisesRegex(ValueError, error):
                    compare.validate_provenance(changed, receipt, module_graph, "amd64", "a" * 40, "b" * 64)
        changed_receipt = {**receipt, "build_definitions_sha256": "e" * 64}
        with self.assertRaisesRegex(ValueError, "build definitions must match"):
            compare.validate_provenance(fixture.native, changed_receipt, module_graph, "amd64", "a" * 40, "b" * 64)
        module_graph["dependencies"][0]["version"] = "different"
        with self.assertRaisesRegex(ValueError, "require the reviewed infra version"):
            compare.validate_provenance(fixture.native, receipt, module_graph, "amd64", "a" * 40, "b" * 64)

    def test_receipt_binds_native_contract_path_and_bytes(self):
        with tempfile.TemporaryDirectory(prefix="swss-contract-receipt-") as temporary:
            root = Path(temporary)
            contract = root / "native-contract.json"
            contract.write_text(json.dumps(Fixture().native))
            receipt = {"native_contract": {"artifact": contract.name, "sha256": compare.sha256(contract)}}
            self.assertEqual(compare.receipt_artifact(root / "manifest.json", receipt, "native_contract", contract), contract)
            with self.assertRaisesRegex(ValueError, "input does not match the build receipt path"):
                compare.receipt_artifact(root / "manifest.json", receipt, "native_contract", root / "other.json")
            with tempfile.TemporaryDirectory(prefix="swss-external-contract-") as external:
                outside = Path(external) / contract.name
                contract.rename(outside)
                contract.symlink_to(outside)
                with self.assertRaisesRegex(ValueError, "leaves the build receipt directory"):
                    compare.receipt_artifact(root / "manifest.json", receipt, "native_contract", contract)
                contract.unlink()
                outside.rename(contract)
            contract.write_text("{}\n")
            with self.assertRaisesRegex(ValueError, "input does not match the build receipt hash"):
                compare.receipt_artifact(root / "manifest.json", receipt, "native_contract", contract)


if __name__ == "__main__":
    unittest.main()
