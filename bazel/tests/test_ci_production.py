#!/usr/bin/env python3
"""Regressions for configured graph discovery and retained CI evidence."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock


BAZEL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BAZEL_DIRECTORY))
import ci_production as ci  # noqa: E402
import production_graph as graph  # noqa: E402


REVISION = "a" * 40
BUILD_DEFINITIONS_SHA256 = "b" * 64


class GraphFixture:
    def __init__(self):
        self.programs = {
            "example": {"directory": "app", "sources": ["//app:main.cpp", "//lib:shared.cpp"]},
            "helper": {"directory": "app", "sources": ["//app:helper.cpp"]},
        }
        self.sources = ["app/helper.cpp", "app/main.cpp", "lib/shared.cpp"]
        platform = graph.ARCHITECTURES["amd64"][2].replace("@sonic_build_infra//", "@@sonic_build_infra+//")
        self.evidence = {
            "schema_version": 1,
            "query": graph.PRODUCTION_QUERY,
            "root_repository_mapping": {"sonic_build_infra": "sonic_build_infra+"},
            "cquery": {
                "configurations": [{
                    "id": 1,
                    "checksum": "c" * 64,
                    "fragmentOptions": [{"options": [
                        {"name": "compilation_mode", "value": "opt"},
                        {"name": "platforms", "value": "[" + platform + "]"},
                        *({"name": key, "value": "[]"} for key in ("copt", "cxxopt", "linkopt", "per_file_copt", "features")),
                    ]}],
                }],
                "results": [
                    self.rule(graph.AGGREGATE, "filegroup", ["//app:example", "//app:helper"]),
                    *(self.rule("//app:" + name, "cc_binary", program["sources"]) for name, program in self.programs.items()),
                ],
            },
            "architecture": "amd64",
            "git_revision": REVISION,
            "build_definitions_sha256": BUILD_DEFINITIONS_SHA256,
            "programs": copy.deepcopy(self.programs),
            "programs_sha256": ci.inventory_sha256(self.programs),
        }

    @staticmethod
    def rule(label, kind, sources):
        return {
            "configurationId": 1,
            "target": {"rule": {
                "name": "@@" + label,
                "ruleClass": kind,
                "attribute": [{"name": "srcs", "type": "LABEL_LIST", "stringListValue": ["@@" + source for source in sources]}],
            }},
        }

    def get_rule(self, label):
        return next(item["target"]["rule"] for item in self.evidence["cquery"]["results"] if item["target"]["rule"]["name"] == "@@" + label)

    def sources_for(self, label):
        return self.get_rule(label)["attribute"][0]["stringListValue"]


class ProductionGraphTest(unittest.TestCase):
    def test_discovers_programs_from_the_configured_aggregate(self):
        fixture = GraphFixture()
        self.assertEqual(graph.parse_program_graph(fixture.evidence, "amd64"), fixture.programs)

    def test_rejects_malformed_aggregate_and_non_binary_members(self):
        for problem in ("missing", "wrong_kind", "empty", "duplicate", "non_binary", "missing_member"):
            with self.subTest(problem=problem):
                fixture = GraphFixture()
                results = fixture.evidence["cquery"]["results"]
                if problem == "missing":
                    del results[0]
                elif problem == "wrong_kind":
                    fixture.get_rule(graph.AGGREGATE)["ruleClass"] = "cc_library"
                elif problem == "empty":
                    fixture.sources_for(graph.AGGREGATE).clear()
                elif problem == "duplicate":
                    fixture.sources_for(graph.AGGREGATE).append("@@//app:example")
                elif problem == "non_binary":
                    fixture.get_rule("//app:example")["ruleClass"] = "cc_library"
                else:
                    del results[1]
                with self.assertRaises(ValueError):
                    graph.parse_program_graph(fixture.evidence, "amd64")

    def test_rejects_unselected_programs_and_duplicate_configurations(self):
        fixture = GraphFixture()
        fixture.evidence["cquery"]["results"].append(fixture.rule("//app:extra", "cc_binary", ["//app:extra.cpp"]))
        with self.assertRaises(ValueError):
            graph.parse_program_graph(fixture.evidence, "amd64")
        fixture = GraphFixture()
        fixture.evidence["cquery"]["results"].append(copy.deepcopy(fixture.evidence["cquery"]["results"][1]))
        with self.assertRaises(ValueError):
            graph.parse_program_graph(fixture.evidence, "amd64")

    def test_rejects_nonlocal_unsafe_empty_and_duplicate_sources(self):
        for sources in ([], ["@@external+//:main.cpp"], ["@@//app:../main.cpp"], ["@@//app:main.h"], ["@@//app:main.cpp"] * 2):
            with self.subTest(sources=sources):
                fixture = GraphFixture()
                fixture.sources_for("//app:example")[:] = sources
                with self.assertRaises(ValueError):
                    graph.parse_program_graph(fixture.evidence, "amd64")

    def test_requires_release_configuration_and_selected_architecture(self):
        fixture = GraphFixture()
        options = fixture.evidence["cquery"]["configurations"][0]["fragmentOptions"][0]["options"]
        options[0]["value"] = "dbg"
        with self.assertRaises(ValueError):
            graph.parse_program_graph(fixture.evidence, "amd64")
        fixture = GraphFixture()
        with self.assertRaises(ValueError):
            graph.parse_program_graph(fixture.evidence, "arm64")

    def test_compares_programs_and_sources_against_independent_native_inventory(self):
        fixture = GraphFixture()
        expected = copy.deepcopy(fixture.programs)
        expected["example"].update({"install": "usr/bin/example", "local_include_directories": ["app", ".", "app"]})
        graph.validate_program_inventory(expected, fixture.programs)
        for problem in ("omitted_program", "extra_program", "different_label", "omitted_source", "extra_source"):
            with self.subTest(problem=problem):
                observed = copy.deepcopy(fixture.programs)
                if problem == "omitted_program":
                    del observed["helper"]
                elif problem == "extra_program":
                    observed["extra"] = {"directory": "app", "sources": ["//app:extra.cpp"]}
                elif problem == "different_label":
                    observed["helper"]["directory"] = "different"
                elif problem == "omitted_source":
                    observed["example"]["sources"].pop()
                else:
                    observed["example"]["sources"].append("//app:extra.cpp")
                with self.assertRaises(ValueError):
                    graph.validate_program_inventory(expected, observed)


class BuildDefinitionsTest(unittest.TestCase):
    def test_hash_tracks_build_definitions_without_tracking_source_contents(self):
        with tempfile.TemporaryDirectory(prefix="swss-build-definition-hash-") as temporary:
            root = Path(temporary)
            contents = {
                "app/BUILD.bazel": 'filegroup(name = "files")\n',
                "tools/cc.bzl": "CXXOPTS = []\n",
                "app/main.cpp": "int main() { return 0; }\n",
            }
            for name, content in contents.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            tracked = ("\0".join(contents) + "\0").encode()
            with unittest.mock.patch.object(ci, "ROOT", root), unittest.mock.patch.object(ci.subprocess, "check_output", return_value=tracked):
                original = ci.build_definitions_sha256()
                for name in ("app/BUILD.bazel", "tools/cc.bzl"):
                    with self.subTest(definition=name):
                        path = root / name
                        path.write_text(contents[name] + "# Changed build selection.\n")
                        self.assertNotEqual(ci.build_definitions_sha256(), original)
                        path.write_text(contents[name])
                        self.assertEqual(ci.build_definitions_sha256(), original)
                (root / "app/main.cpp").write_text("int main() { return 1; }\n")
                self.assertEqual(ci.build_definitions_sha256(), original)


class RetainedGraphTest(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory(prefix="swss-retained-graph-")))
        self.fixture = GraphFixture()
        self.graph_path = self.root / "production-graph.json"
        self.receipt_path = self.root / "manifest.json"
        self.receipt = {
            "schema_version": 3,
            "artifact_type": "cpp_executables",
            "build_mode": "codeql",
            "cache_policy": {"fresh_output_base": True, "action_result_reuse": "disabled", "spawn_strategy": "local"},
            "git_revision": REVISION,
            "build_definitions_sha256": BUILD_DEFINITIONS_SHA256,
            "architecture": "amd64",
            "target_platform": graph.ARCHITECTURES["amd64"][2],
            "additional_compile_targets": ci.CODEQL_COMPILE_TARGETS,
            "source_files": self.fixture.sources,
            "programs": [{"name": name, "label": "//app:" + name} for name in self.fixture.programs],
        }
        self.write_graph()
        self.stack.enter_context(unittest.mock.patch.object(ci, "ROOT", self.root))
        self.stack.enter_context(unittest.mock.patch.object(ci, "checkout_revision", return_value=REVISION))
        self.stack.enter_context(unittest.mock.patch.object(ci, "build_definitions_sha256", return_value=BUILD_DEFINITIONS_SHA256))
        self.native_sources = (
            set(self.fixture.sources) | ci.CODEQL_TEST_SOURCES | ci.CODEQL_FEATURE_SOURCES
            | {"tests/legacy_test.cpp", "lib/asan.cpp", "other/unused.cpp"}
        )
        for source in self.native_sources:
            path = self.root / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// Tracked fixture source.\n")
        self.stack.enter_context(unittest.mock.patch.object(ci.subprocess, "check_output", return_value=("\0".join(sorted(self.native_sources)) + "\0").encode()))

    def write_receipt(self):
        self.receipt_path.write_text(json.dumps(self.receipt) + "\n")

    def write_graph(self):
        self.graph_path.write_text(json.dumps(self.fixture.evidence) + "\n")
        self.receipt["production_graph"] = {"artifact": self.graph_path.name, "sha256": ci.sha256(self.graph_path)}
        self.write_receipt()

    def test_replays_the_retained_graph(self):
        evidence, programs = ci.load_retained_graph(self.receipt_path)
        self.assertEqual(evidence, self.fixture.evidence)
        self.assertEqual(programs, self.fixture.programs)

    def test_rejects_graph_bytes_that_do_not_match_receipt(self):
        self.graph_path.write_text(self.graph_path.read_text() + " ")
        with self.assertRaisesRegex(ValueError, "hash|sha256"):
            ci.load_retained_graph(self.receipt_path)

    def test_rejects_artifact_paths_outside_receipt_directory(self):
        self.receipt["production_graph"]["artifact"] = "../production-graph.json"
        self.write_receipt()
        with self.assertRaises(ValueError):
            ci.load_retained_graph(self.receipt_path)

    def test_rejects_artifact_symlinks_outside_receipt_directory(self):
        with tempfile.TemporaryDirectory(prefix="swss-external-artifact-") as temporary:
            external = Path(temporary) / "production-graph.json"
            external.write_bytes(self.graph_path.read_bytes())
            self.graph_path.unlink()
            self.graph_path.symlink_to(external)
            with self.assertRaisesRegex(ValueError, "leaves the build receipt directory"):
                ci.load_retained_graph(self.receipt_path)

    def test_binds_receipt_and_graph_to_checkout_revision_and_build_definitions(self):
        for location in ("receipt", "graph"):
            for key, value in (("git_revision", "d" * 40), ("build_definitions_sha256", "e" * 64)):
                with self.subTest(location=location, key=key):
                    target = self.receipt if location == "receipt" else self.fixture.evidence
                    original = target[key]
                    target[key] = value
                    self.write_graph()
                    with self.assertRaises(ValueError):
                        ci.load_retained_graph(self.receipt_path)
                    target[key] = original
                    self.write_graph()

    def test_rejects_disagreement_between_graph_metadata_and_parsed_rules(self):
        self.fixture.evidence["programs"]["example"]["sources"] = ["//app:other.cpp"]
        self.write_graph()
        with self.assertRaises(ValueError):
            ci.load_retained_graph(self.receipt_path)

    def test_requires_the_selected_sources_to_exist_in_the_checkout(self):
        (self.root / "app/main.cpp").unlink()
        with self.assertRaisesRegex(ValueError, "source is missing"):
            ci.load_retained_graph(self.receipt_path)

    def test_rejects_receipt_inventory_and_platform_drift(self):
        for key, value in (
            ("programs", [{"name": "example", "label": "//app:example"}]),
            ("source_files", ["app/main.cpp"]),
            ("target_platform", graph.ARCHITECTURES["arm64"][2]),
            ("schema_version", 2),
        ):
            with self.subTest(key=key):
                original = self.receipt[key]
                self.receipt[key] = value
                self.write_receipt()
                with self.assertRaises(ValueError):
                    ci.load_retained_graph(self.receipt_path)
                self.receipt[key] = original
                self.write_receipt()

    def codeql_args(self, missing=None):
        selected = set(self.fixture.sources) | ci.CODEQL_TEST_SOURCES | ci.CODEQL_FEATURE_SOURCES
        csv_path = self.root / "source-files.csv"
        observed = selected | {"generated/proto.cpp"}
        if missing:
            observed.remove(missing)
        csv_path.write_text("path\n" + "\n".join(sorted(observed)) + "\n")
        return argparse.Namespace(build_receipt=self.receipt_path, csv=csv_path, output=self.root / "coverage.json")

    def test_codeql_replays_production_sources_and_adds_selected_tests_and_features(self):
        args = self.codeql_args()
        ci.check_codeql(args)
        coverage = json.loads(args.output.read_text())
        self.assertEqual(coverage["selected_source_files"], {
            "production": self.fixture.sources,
            "tests": sorted(ci.CODEQL_TEST_SOURCES),
            "features": sorted(ci.CODEQL_FEATURE_SOURCES),
        })
        self.assertEqual(coverage["missing_source_files"], [])
        self.assertEqual(coverage["observed_sources_not_in_tracked_native_inventory"], ["generated/proto.cpp"])
        self.assertEqual(coverage["excluded_source_files"], {
            "tests": ["tests/legacy_test.cpp"], "features": ["lib/asan.cpp"], "other_native": ["other/unused.cpp"],
        })
        self.assertEqual(coverage["production_graph"], self.receipt["production_graph"])
        self.assertEqual(coverage["build_receipt"]["sha256"], ci.sha256(self.receipt_path))

    def test_codeql_requires_its_own_uncached_build_and_selected_compile_targets(self):
        for key, value in (
            ("build_mode", "normal"),
            ("cache_policy", {"fresh_output_base": False, "action_result_reuse": "allowed", "spawn_strategy": "local"}),
            ("additional_compile_targets", []),
        ):
            with self.subTest(key=key):
                original = self.receipt[key]
                self.receipt[key] = value
                self.write_receipt()
                with self.assertRaisesRegex(ValueError, "fresh, uncached production build receipt"):
                    ci.check_codeql(self.codeql_args())
                self.receipt[key] = original
                self.write_receipt()

    def test_codeql_reports_a_missing_selected_translation_unit(self):
        args = self.codeql_args(missing="app/main.cpp")
        with self.assertRaisesRegex(ValueError, "CodeQL did not extract"):
            ci.check_codeql(args)
        self.assertEqual(json.loads(args.output.read_text())["missing_source_files"], ["app/main.cpp"])


if __name__ == "__main__":
    unittest.main()
