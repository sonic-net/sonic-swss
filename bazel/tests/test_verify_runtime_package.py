"""Keep runtime expectations independent of the handwritten package rules."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


BAZEL_DIRECTORY = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("verify_runtime_package", BAZEL_DIRECTORY / "verify_runtime_package.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)
REVISION = "a" * 40


class RuntimeInventoryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name)
        (self.source / "debian").mkdir()
        (self.source / "cfgmgr").mkdir()
        (self.source / "helpers").mkdir()
        (self.source / "debian/swss.install").write_text(
            "target/release/countersyncd usr/bin\nhelpers/check.py usr/bin\n")
        (self.source / "helpers/check.py").write_bytes(b"python helper\n")
        for stem in ("buffer_check_headroom", "buffer_headroom", "buffer_pool"):
            (self.source / ("cfgmgr/" + stem + "_mellanox.lua")).write_bytes((stem + " Mellanox implementation\n").encode())
            (self.source / ("cfgmgr/" + stem + "_vs.lua")).write_bytes((stem + " unused VS source\n").encode())
        self.contract = {
            "schema_version": 2,
            "git_revision": REVISION,
            "architecture": {"execution_machine": "x86_64", "configured_target_cpus": ["x86_64"]},
            "programs": {
                "orchagent": {
                    "directory": "orchagent",
                    "sources": ["//orchagent:main.cpp"],
                    "local_include_directories": ["orchagent", ".", "."],
                    "install_path": "usr/bin/orchagent",
                    "compile_profile": "orchagent",
                    "link_profile": "orchagent",
                },
            },
            "compile_profiles": {"orchagent": []},
            "link_profiles": {"orchagent": []},
            "automake_install": [
                {"install_path": "usr/share/swss/buffer_check_headroom_vs.lua", "mode": 0o644,
                 "source": "cfgmgr/buffer_check_headroom_mellanox.lua"},
                {"install_path": "usr/share/swss/buffer_headroom_vs.lua", "mode": 0o644,
                 "source": "cfgmgr/buffer_headroom_mellanox.lua"},
                {"install_path": "usr/share/swss/buffer_pool_vs.lua", "mode": 0o644,
                 "source": "cfgmgr/buffer_pool_mellanox.lua"},
            ],
        }
        self.contract_path = self.source / "native-build-contract.json"
        self.write_contract()
        revision = patch.object(PACKAGE, "command", return_value=REVISION + "\n")
        self.git = revision.start()
        self.addCleanup(revision.stop)

    def write_contract(self):
        self.contract_path.write_text(json.dumps(self.contract, indent=2) + "\n")

    def read(self, architecture="amd64"):
        return PACKAGE.read_contract(self.source, self.contract_path, architecture)

    def runtime(self):
        return {
            "usr/bin/orchagent": (0o755, b"ELF placeholder"),
            "usr/bin/countersyncd": (0o755, b"Rust ELF placeholder"),
            "usr/bin/check.py": (0o755, b"python helper\n"),
            "usr/share/swss/buffer_check_headroom_vs.lua": (0o644, b"buffer_check_headroom Mellanox implementation\n"),
            "usr/share/swss/buffer_headroom_vs.lua": (0o644, b"buffer_headroom Mellanox implementation\n"),
            "usr/share/swss/buffer_pool_vs.lua": (0o644, b"buffer_pool Mellanox implementation\n"),
        }

    def test_native_inventory_preserves_debian_extras_and_vs_alias(self):
        expected, provenance = self.read()
        self.assertEqual(set(expected), set(self.runtime()))
        for stem in ("buffer_check_headroom", "buffer_headroom", "buffer_pool"):
            self.assertEqual(expected["usr/share/swss/" + stem + "_vs.lua"],
                             (0o644, self.source / ("cfgmgr/" + stem + "_mellanox.lua")))
        self.assertIsNone(expected["usr/bin/countersyncd"][1])
        self.assertEqual(provenance["sha256"], hashlib.sha256(self.contract_path.read_bytes()).hexdigest())
        self.assertEqual(provenance["git_revision"], REVISION)
        self.git.assert_called_once_with("git", "-C", str(self.source), "rev-parse", "HEAD")
        PACKAGE.verify_inventory(self.runtime(), expected)

    def test_missing_or_extra_packaged_paths_fail_against_native_inventory(self):
        expected, _ = self.read()
        for omitted in ("usr/bin/orchagent", "usr/bin/countersyncd", "usr/share/swss/buffer_pool_vs.lua"):
            with self.subTest(omitted=omitted):
                runtime = self.runtime()
                del runtime[omitted]
                with self.assertRaisesRegex(ValueError, "runtime install mismatch: missing="):
                    PACKAGE.verify_inventory(runtime, expected)
        runtime = self.runtime()
        runtime["usr/bin/unreviewed"] = (0o755, b"extra")
        with self.assertRaisesRegex(ValueError, "extra=.*usr/bin/unreviewed"):
            PACKAGE.verify_inventory(runtime, expected)

    def test_installing_vs_source_instead_of_native_alias_is_rejected(self):
        expected, _ = self.read()
        for stem in ("buffer_check_headroom", "buffer_headroom", "buffer_pool"):
            with self.subTest(alias=stem):
                runtime = self.runtime()
                runtime["usr/share/swss/" + stem + "_vs.lua"] = (0o644, (stem + " unused VS source\n").encode())
                with self.assertRaisesRegex(ValueError, "installed data differs from source"):
                    PACKAGE.verify_inventory(runtime, expected)
        runtime = self.runtime()
        runtime["usr/bin/check.py"] = (0o644, b"python helper\n")
        with self.assertRaisesRegex(ValueError, "runtime permissions"):
            PACKAGE.verify_inventory(runtime, expected)

    def test_stale_revision_wrong_architecture_and_missing_inventory_fail(self):
        original = copy.deepcopy(self.contract)
        cases = [
            ("schema_version", 1, "schema must be 2"),
            ("git_revision", "b" * 40, "revision differs"),
            ("architecture", {"execution_machine": "aarch64", "configured_target_cpus": ["aarch64"]}, "architecture differs"),
            ("architecture", {"execution_machine": "x86_64", "configured_target_cpus": ["aarch64"]}, "architecture differs"),
            ("programs", {}, "no programs"),
            ("automake_install", None, "no Automake install inventory"),
        ]
        for key, value, error in cases:
            with self.subTest(key=key, value=value):
                self.contract = copy.deepcopy(original)
                self.contract[key] = value
                self.write_contract()
                with self.assertRaisesRegex(ValueError, error):
                    self.read()
        self.contract = original
        self.contract["architecture"] = {"execution_machine": "aarch64", "configured_target_cpus": ["aarch64"]}
        self.write_contract()
        self.read("arm64")

    def test_duplicate_install_paths_and_unsafe_source_paths_fail(self):
        self.contract["automake_install"][0]["install_path"] = "usr/bin/orchagent"
        self.write_contract()
        with self.assertRaisesRegex(ValueError, "duplicate install path"):
            self.read()
        self.contract["automake_install"][0]["install_path"] = "usr/share/swss/buffer_pool_vs.lua"
        self.contract["automake_install"][0]["source"] = "../outside.lua"
        self.write_contract()
        with self.assertRaisesRegex(ValueError, "unsafe contract path"):
            self.read()

    def test_install_sources_must_exist_and_stay_inside_checkout(self):
        origin = self.source / "cfgmgr/buffer_check_headroom_mellanox.lua"
        origin.unlink()
        with self.assertRaisesRegex(ValueError, "missing install source"):
            self.read()
        with tempfile.TemporaryDirectory() as other:
            outside = Path(other) / "outside.lua"
            outside.write_bytes(b"outside checkout")
            origin.symlink_to(outside)
            with self.assertRaisesRegex(ValueError, "escapes checkout"):
                self.read()


if __name__ == "__main__":
    unittest.main()
