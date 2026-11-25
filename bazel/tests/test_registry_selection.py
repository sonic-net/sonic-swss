"""Verify that SWSS CI records the maintained registry without changing inputs."""

import json
import os
from pathlib import Path
import tempfile
import textwrap
import unittest
import unittest.mock


ROOT = Path(__file__).resolve().parents[2]
MAIN = "https://raw.githubusercontent.com/sonic-net/sonic-bazel-registry/main"
OBSERVED_MAIN = "a" * 40
WORKFLOWS = ("bazel.yml", "codeql-analysis.yml")


def selection_script(workflow):
    text = (ROOT / ".github/workflows" / workflow).read_text()
    start = "          python3 - <<'PYTHON'\n"
    assert text.count(start) == 1
    return textwrap.dedent(text.split(start, 1)[1].split("          PYTHON\n", 1)[0])


class RegistrySelectionTest(unittest.TestCase):
    def run_selection(self, workflow, declared, working):
        def git_output(command, **kwargs):
            if command == ["git", "show", "HEAD:.bazelrc"]:
                return declared
            if command == ["git", "ls-remote",
                           "https://github.com/sonic-net/sonic-bazel-registry.git",
                           "refs/heads/main"]:
                return OBSERVED_MAIN + "\trefs/heads/main\n"
            raise AssertionError("Unexpected subprocess: " + repr(command))

        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            (workspace / ".bazelrc").write_text(working)
            previous = Path.cwd()
            try:
                os.chdir(workspace)
                with unittest.mock.patch("subprocess.check_output", side_effect=git_output):
                    exec(compile(selection_script(workflow), workflow, "exec"), {})
                evidence = workspace / "artifacts/validation"
                return {
                    "declared": (evidence / "declared.bazelrc").read_text(),
                    "effective": (evidence / "effective.bazelrc").read_text(),
                    "working": (workspace / ".bazelrc").read_text(),
                    "receipt": json.loads((evidence / "registry-inputs.json").read_text()),
                }
            finally:
                os.chdir(previous)

    def test_both_workflows_preserve_the_declared_endpoint(self):
        declared = (ROOT / ".bazelrc").read_text()
        self.assertEqual(declared.count(MAIN), 1)
        for workflow in WORKFLOWS:
            with self.subTest(workflow=workflow):
                result = self.run_selection(workflow, declared, declared)
                receipt = result["receipt"]
                self.assertEqual(receipt, {"registry": MAIN,
                                           "observed_revision": OBSERVED_MAIN})
                self.assertEqual(result["declared"], declared)
                self.assertEqual(result["effective"], declared)
                self.assertEqual(result["working"], declared)

    def test_unrelated_working_configuration_changes_are_rejected(self):
        declared = (ROOT / ".bazelrc").read_text()
        for workflow in WORKFLOWS:
            with self.subTest(workflow=workflow), self.assertRaises(AssertionError):
                self.run_selection(workflow, declared, declared + "\nbuild --jobs=99\n")

    def test_missing_or_repeated_endpoint_is_rejected(self):
        declared = (ROOT / ".bazelrc").read_text()
        for invalid in (declared.replace(MAIN, "https://invalid.example/registry"),
                        declared + "\ncommon --registry=" + MAIN + "\n"):
            for workflow in WORKFLOWS:
                with self.subTest(workflow=workflow, invalid=invalid), self.assertRaises(AssertionError):
                    self.run_selection(workflow, invalid, invalid)


if __name__ == "__main__":
    unittest.main()
