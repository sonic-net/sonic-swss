#!/usr/bin/env python3
"""Focused tests for the configured Automake to Bazel boundary."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
import unittest.mock


BAZEL_DIRECTORY = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("swss_native_build_contract", BAZEL_DIRECTORY / "native_build_contract.py")
assert SPEC is not None and SPEC.loader is not None
native_build_contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(native_build_contract)


class Fixture:
    def __init__(self, root: Path):
        self.source = root / "source"
        self.configured = root / "configured"
        for directory in (self.source, self.configured, self.source / "bazel"):
            directory.mkdir(parents=True)
        files = {
            "app/main.cpp": '#include "config.h"\n#include "app/common.h"\nint main() { return answer(); }\n',
            "app/base.cpp": '#include "app/common.h"\nint answer() { return 0; }\n',
            "app/feature.cpp": "int feature() { return 1; }\n",
            "app/common.h": "int answer();\n",
            "app/data.lua": "return 1\n",
            "team/main.cpp": "int main() { return 0; }\n",
        }
        for name, contents in files.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        (self.configured / "config.h").write_text("#define FIXTURE 1\n")
        self.write_makefiles(team=True)

    def write_makefiles(self, team: bool) -> None:
        common = (
            f"abs_top_srcdir = {self.source}\nCXX = g++\nEXEEXT =\nhost_cpu = {platform.machine()}\n"
            + "INSTALL_DATA = /usr/bin/install -c -m 644\ninstall_sh_DATA = ./install-sh -c -m 644\n"
        )
        (self.configured / "Makefile").write_text(
            common + "SUBDIRS = app\n" + f"WITH_TEAM = {'yes' if team else 'no'}\n"
            + "ifeq ($(WITH_TEAM),yes)\nSUBDIRS += team\nendif\n"
        )
        (self.configured / "app").mkdir(exist_ok=True)
        (self.configured / "app/Makefile").write_text(
            common
            + f"top_srcdir = {self.source}\n"
            + "DEFAULT_INCLUDES = -I. -I$(top_srcdir)/app -I..\n"
            + "INCLUDES = -I $(top_srcdir)\nDEFS = -DHAVE_CONFIG_H\n"
            + f"CXXFLAGS = -g -O2 -ffile-prefix-map={self.source}=.\n"
            + "CPPFLAGS = -DGLOBAL\nAM_CPPFLAGS = -DFALLBACK\nAM_CXXFLAGS = -Wall\n"
            + "AM_LDFLAGS = -Wl,--as-needed\nLIBS = -lm\n"
            + "bindir = /usr/bin\nbin_PROGRAMS = example\n"
            + "example_SOURCES = main.cpp base.cpp common.h\nexample_OBJECTS = example-main.o example-base.o\n"
            + "FEATURE = yes\nifeq ($(FEATURE),yes)\nexample_SOURCES += feature.cpp\nexample_OBJECTS += example-feature.o\nendif\n"
            + "example_CPPFLAGS = -DEXAMPLE\nexample_LDADD = -lm\n"
            + "datadir = /usr/share\nexampledir = $(datadir)/swss\ndist_example_DATA = data.lua\n"
        )
        (self.configured / "team").mkdir(exist_ok=True)
        (self.configured / "team/Makefile").write_text(
            common + "bindir = /usr/bin\nbin_PROGRAMS = team\nteam_SOURCES = main.cpp\nteam_OBJECTS = team-main.o\n"
        )

    def generator(self):
        generator = native_build_contract.Generator(self.source, self.configured)
        generator.read_make_tree()
        return generator

    def build_contract(self) -> dict:
        generator = self.generator()
        return generator.build_contract(generator.production_inventory(), "a" * 40)


class GeneratorTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="swss-bazel-test-")
        self.fixture = Fixture(Path(self.temporary.name))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_configured_selection_flags_and_install_inventory(self) -> None:
        inventory = self.fixture.generator().production_inventory()
        self.assertEqual([item["name"] for item in inventory["programs"]], ["example", "team"])
        example = inventory["programs"][0]
        self.assertEqual(example["sources"], ["app/main.cpp", "app/base.cpp", "app/feature.cpp"])
        self.assertIn("-DEXAMPLE", example["compile_options"])
        self.assertNotIn("-DFALLBACK", example["compile_options"])
        self.assertIn("-DGLOBAL", example["compile_options"])
        self.assertIn("-ffile-prefix-map=<swss>=.", example["compile_options"])
        self.assertEqual(example["link_options"][-2:], ["-lm", "-lm"])
        self.assertEqual(inventory["automake_install"][0]["install_path"], "usr/share/swss/data.lua")
        self.assertNotIn(str(self.fixture.source), json.dumps(inventory))
        self.assertNotIn(str(self.fixture.configured), json.dumps(inventory))

    def test_contract_preserves_configured_sources_and_install_inventory(self) -> None:
        contract = self.fixture.build_contract()
        self.assertEqual(contract["programs"]["example"]["sources"], [
            "//app:main.cpp", "//app:base.cpp", "//app:feature.cpp",
        ])
        self.assertEqual(contract["programs"]["example"]["install_path"], "usr/bin/example")
        self.assertEqual(contract["programs"]["example"]["local_include_directories"], ["app", "app", ".", "."])
        self.assertEqual(contract["automake_install"], [
            {"source": "app/data.lua", "install_path": "usr/share/swss/data.lua", "mode": 0o644},
        ])

    def test_contract_detects_local_include_order_and_class_drift(self) -> None:
        baseline = self.fixture.build_contract()
        makefile = self.fixture.configured / "app/Makefile"
        original = makefile.read_text()
        makefile.write_text(original.replace("-I. -I$(top_srcdir)/app -I..", "-I.. -I$(top_srcdir)/app -I."))
        changed = self.fixture.build_contract()
        self.assertNotEqual(baseline["programs"]["example"]["local_include_directories"],
                            changed["programs"]["example"]["local_include_directories"])
        makefile.write_text(original.replace("-I. -I$(top_srcdir)/app -I..", "-isystem . -I$(top_srcdir)/app -I.."))
        with self.assertRaisesRegex(ValueError, "local module include class needs an explicit mapping"):
            self.fixture.build_contract()

    def test_build_contract_records_flags_and_normalizes_configured_roots(self) -> None:
        contract = self.fixture.build_contract()
        relocated = Fixture(Path(self.temporary.name) / "relocated")
        self.assertEqual(contract, relocated.build_contract())
        self.assertEqual(contract["architecture"], {
            "execution_machine": platform.machine(),
            "configured_target_cpus": [platform.machine()],
        })
        profile = contract["programs"]["example"]["compile_profile"]
        options = contract["compile_profiles"][profile]
        self.assertIn({"argument": "-I<swss>/app", "classification": "include"}, options)
        self.assertIn({"argument": "-ffile-prefix-map=<swss>=.", "classification": "source_prefix_map"}, options)
        self.assertIn({"argument": "-DEXAMPLE", "classification": "literal"}, options)
        self.assertIn({"argument": "-DHAVE_CONFIG_H", "classification": "native_only"}, options)
        self.assertNotIn(str(self.fixture.source), json.dumps(contract))
        self.assertNotIn(str(self.fixture.configured), json.dumps(contract))

    def test_build_contract_exposes_compile_and_link_drift(self) -> None:
        baseline = self.fixture.build_contract()
        makefile = self.fixture.configured / "app/Makefile"
        original = makefile.read_text()
        makefile.write_text(original.replace("CPPFLAGS = -DGLOBAL", "CPPFLAGS = -DGLOBAL -DNEW_FEATURE=1"))
        compile_change = self.fixture.build_contract()
        self.assertEqual(baseline["programs"], compile_change["programs"])
        self.assertNotEqual(baseline["compile_profiles"], compile_change["compile_profiles"])
        profile = compile_change["programs"]["example"]["compile_profile"]
        self.assertIn({"argument": "-DNEW_FEATURE=1", "classification": "literal"}, compile_change["compile_profiles"][profile])

        makefile.write_text(original.replace("example_LDADD = -lm", "example_LDADD = -lpthread"))
        link_change = self.fixture.build_contract()
        self.assertEqual(baseline["programs"], link_change["programs"])
        self.assertNotEqual(baseline["link_profiles"], link_change["link_profiles"])
        profile = link_change["programs"]["example"]["link_profile"]
        self.assertIn({"argument": "-lpthread", "classification": "library"}, link_change["link_profiles"][profile])

    def test_build_contract_requires_declared_path_and_library_mappings(self) -> None:
        makefile = self.fixture.configured / "app/Makefile"
        original = makefile.read_text()
        makefile.write_text(original + "CPPFLAGS += -I/opt/feature/include\n")
        with self.assertRaisesRegex(ValueError, "native path has no module build-contract mapping"):
            self.fixture.build_contract()
        makefile.write_text(original.replace("example_LDADD = -lm", "example_LDADD = -lfeature"))
        with self.assertRaisesRegex(ValueError, "native library has no module build-contract provider"):
            self.fixture.build_contract()

    def test_cli_records_contract_without_modifying_source_files(self) -> None:
        output = Path(self.temporary.name) / "artifacts/native-build-contract.json"
        command = [
            sys.executable, str(BAZEL_DIRECTORY / "native_build_contract.py"),
            "--source", str(self.fixture.source),
            "--configured-build", str(self.fixture.configured),
            "--output-build-contract", str(output),
            "--git-revision", "a" * 40,
        ]
        before = {p.relative_to(self.fixture.source): p.read_bytes() for p in self.fixture.source.rglob("*") if p.is_file()}
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        contract = json.loads(output.read_text())
        self.assertEqual(contract, self.fixture.build_contract())
        self.assertEqual(contract["schema_version"], 2)
        self.assertEqual(contract["git_revision"], "a" * 40)
        after = {p.relative_to(self.fixture.source): p.read_bytes() for p in self.fixture.source.rglob("*") if p.is_file()}
        self.assertEqual(after, before)

        makefile = self.fixture.configured / "app/Makefile"
        makefile.write_text(makefile.read_text().replace("main.cpp base.cpp common.h", "base.cpp main.cpp common.h"))
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        updated = json.loads(output.read_text())
        self.assertNotEqual(updated["programs"]["example"]["sources"], contract["programs"]["example"]["sources"])

        saved = output.read_bytes()
        makefile.write_text(makefile.read_text() + "CPPFLAGS += -I/opt/unmapped/include\n")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native path has no module build-contract mapping", result.stderr)
        self.assertEqual(output.read_bytes(), saved)

    def test_cli_requires_output_and_exact_revision(self) -> None:
        output = Path(self.temporary.name) / "native.json"
        command = [
            sys.executable, str(BAZEL_DIRECTORY / "native_build_contract.py"),
            "--source", str(self.fixture.source),
            "--configured-build", str(self.fixture.configured),
        ]
        for options in [
            ["--output-build-contract", str(output)],
            ["--git-revision", "a" * 40],
            ["--output-build-contract", str(output), "--git-revision", "main"],
        ]:
            with self.subTest(options=options):
                result = subprocess.run(command + options, capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse(output.exists())

    def test_missing_or_unsupported_inputs_fail_before_generation(self) -> None:
        makefile = self.fixture.configured / "app/Makefile"
        original = makefile.read_text()
        makefile.write_text(original + "BUILT_SOURCES = generated.cpp\n")
        with self.assertRaisesRegex(ValueError, "generated sources"):
            self.fixture.build_contract()
        makefile.write_text(original.replace("example-feature.o", ""))
        with self.assertRaisesRegex(ValueError, "object list"):
            self.fixture.build_contract()
        makefile.write_text(original + "INCLUDES += -I\n")
        with self.assertRaisesRegex(ValueError, "missing path"):
            self.fixture.build_contract()

    def test_path_options_reject_missing_paths_in_inventory_and_profiles(self) -> None:
        generator = self.fixture.generator()
        for flag in ("-I", "-isystem", "-iquote", "-idirafter", "-L"):
            for suffix in ([], ["-DOTHER"]):
                with self.subTest(flag=flag, suffix=suffix):
                    options = [flag, *suffix]
                    with self.assertRaisesRegex(ValueError, "missing path after"):
                        generator.options(options, "app")
                    inventory = generator.production_inventory()
                    inventory["programs"][0]["compile_options"] = options
                    with self.assertRaisesRegex(ValueError, "missing path after"):
                        generator.build_contract(inventory, "a" * 40)

    def test_fpmsyncd_query_defaults_only_unset_legacy_include_root(self) -> None:
        directory = self.fixture.configured / "fpmsyncd"
        directory.mkdir()
        makefile = directory / "Makefile"
        contents = f"abs_top_srcdir = {self.fixture.source}\nINCLUDES = -I $(FPM_PATH)\n"
        makefile.write_text(contents)
        with unittest.mock.patch.dict(os.environ):
            os.environ.pop("FPM_PATH", None)
            variables = native_build_contract.query_make(directory)
        self.assertEqual(native_build_contract.value(variables, "INCLUDES"), f"-I {self.fixture.source}/fpmsyncd")
        makefile.write_text(contents + "FPM_PATH = /usr/include/fpm\n")
        variables = native_build_contract.query_make(directory)
        self.assertEqual(native_build_contract.value(variables, "INCLUDES"), "-I /usr/include/fpm")
        self.assertEqual(makefile.read_text(), contents + "FPM_PATH = /usr/include/fpm\n")


if __name__ == "__main__":
    unittest.main()
