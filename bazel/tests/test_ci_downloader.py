"""Exercise bootstrap trust and failure boundaries without remote downloads."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
READER = b'''import json
import os
from pathlib import Path
import sys

Path("reader-called").write_text("called")
if os.environ.get("READER_REJECT_INPUTS"):
    print("github_sources: Input size or SHA256 does not match its lock", file=sys.stderr)
    raise SystemExit(1)
args = sys.argv[1:]
Path(args[args.index("--distdir") + 1]).mkdir(parents=True, exist_ok=True)
Path(args[args.index("--receipt") + 1]).write_text(json.dumps({"verified": True}))
'''
CURL = '''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
from urllib.parse import urlsplit

args = sys.argv[1:]
name = urlsplit(args[-1]).path.rsplit("/", 1)[-1]
root = Path(os.environ["BOOTSTRAP_FIXTURES"])
with (root / "requests").open("a") as log:
    log.write(name + "\\n")
response = json.loads((root / "responses.json").read_text()).get(name, {})
payload = (root / name).read_bytes()
if response.get("corrupt"):
    payload += b"unexpected bytes"
Path(args[args.index("--output") + 1]).write_bytes(payload)
print(response.get("status", "200"), end="")
raise SystemExit(response.get("exit", 0))
'''


class DownloaderBootstrapTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="swss bootstrap ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixtures = self.root / "fixtures"
        self.fixtures.mkdir()
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.user_config = self.workspace / ".bazelrc.user"
        self.user_config.write_text("build --jobs=2\n")
        self.generated_config = self.workspace / "artifacts/validation/swss-github-inputs.bazelrc"
        files = {"source_mirrors.cfg": b"# public mirror fixture\n",
                 "github_sources.py": READER,
                 "cosmos-4.0.2.json": b'{"schema_version": 1, "sources": []}\n'}
        for name, content in files.items():
            (self.fixtures / name).write_bytes(content)
        self.respond({})
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.bin / "curl").write_text(CURL)
        (self.bin / "curl").chmod(0o755)
        # Replace only the three trust roots in a temporary script copy. The
        # downloaded reader is a probe; the shared reader owns member validation.
        script = (ROOT / "bazel/ci_downloader.sh").read_text()
        values = {"config_digest": hashlib.sha256(files["source_mirrors.cfg"]).hexdigest(),
                  "reader_digest": hashlib.sha256(READER).hexdigest(),
                  "manifest_digest": hashlib.sha256(files["cosmos-4.0.2.json"]).hexdigest()}
        for key, value in values.items():
            script, count = re.subn(r"(?m)^" + key + r"=.*$", key + "=" + value, script)
            self.assertEqual(count, 1)
        self.script = self.workspace / "bootstrap.sh"
        self.script.write_text(script)
        self.env = {k: v for k, v in os.environ.items() if k not in ("RUNNER_TEMP", "READER_REJECT_INPUTS")}
        self.env.update(PATH=str(self.bin) + os.pathsep + self.env["PATH"],
                        BOOTSTRAP_FIXTURES=str(self.fixtures))
        self.bootstrap = self.workspace / "artifacts/swss-github-inputs/bootstrap"

    def respond(self, response):
        (self.fixtures / "responses.json").write_text(json.dumps(response))

    def run_bootstrap(self):
        return subprocess.run(["bash", str(self.script)], cwd=self.workspace,
                              env=self.env, text=True, capture_output=True, timeout=15)

    def requests(self):
        return (self.fixtures / "requests").read_text().splitlines()

    def assert_not_configured(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.user_config.read_text(), "build --jobs=2\n")
        self.assertFalse(self.generated_config.exists())
        self.assertFalse((self.workspace / "reader-called").exists())

    def test_verified_cache_reuse_configures_local_builds_without_network(self):
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.requests()), 3)
        expected = (f'common --downloader_config="{self.bootstrap / "source_mirrors.cfg"}"\n'
                    f'common --distdir="{self.bootstrap.parent / "distdir"}"\n')
        self.assertEqual(self.generated_config.read_text(), expected)
        self.assertEqual(self.user_config.read_text(), "build --jobs=2\n")
        for name in ("source-mirrors.sha256", "github-bootstrap.sha256", "github-sources.json",
                     "swss-github-inputs.bazelrc"):
            self.assertTrue((self.workspace / "artifacts/validation" / name).is_file())
        self.respond({name: {"status": "503", "exit": 22} for name in self.requests()})
        repeated = self.run_bootstrap()
        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertEqual(len(self.requests()), 3)
        self.assertEqual(self.generated_config.read_text(), expected)
        self.assertEqual(self.user_config.read_text(), "build --jobs=2\n")

    def test_private_downloader_configuration_keeps_precedence(self):
        private = 'common --downloader_config="/approved private mirror.cfg"\nbuild --jobs=2\n'
        self.user_config.write_text(private)
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.user_config.read_text(), private)
        self.assertNotIn("approved private mirror", self.generated_config.read_text())
        configuration = (ROOT / ".bazelrc").read_text().splitlines()
        defaults = configuration.index("try-import %workspace%/artifacts/validation/swss-github-inputs.bazelrc")
        for override in ("try-import %workspace%/.bazelrc.user",
                         "try-import %workspace%/../../tools/bazel/submodule-config.bazelrc"):
            self.assertLess(defaults, configuration.index(override))

    def test_ci_uses_runner_temp_but_keeps_evidence_in_workspace(self):
        self.env["RUNNER_TEMP"] = str(self.root / "runner temp")
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(self.root / "runner temp/swss-github-inputs/distdir"),
                      self.generated_config.read_text())
        self.assertEqual(self.user_config.read_text(), "build --jobs=2\n")
        self.assertTrue((self.workspace / "artifacts/validation/github-sources.json").is_file())

    def test_corrupt_cached_reader_is_repaired_before_execution(self):
        self.assertEqual(self.run_bootstrap().returncode, 0)
        (self.bootstrap / "github_sources.py").write_bytes(b"unverified cache data")
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests()[3:], ["github_sources.py"])
        self.assertEqual((self.bootstrap / "github_sources.py").read_bytes(), READER)

    def test_wrong_download_hash_never_executes_reader_or_changes_config(self):
        self.respond({"github_sources.py": {"corrupt": True}})
        result = self.run_bootstrap()
        self.assert_not_configured(result)
        self.assertIn("SHA256 verification failed for ci/github_sources.py", result.stderr)

    def test_redirect_with_valid_bytes_is_rejected_before_reader_execution(self):
        self.respond({"github_sources.py": {"status": "302"}})
        result = self.run_bootstrap()
        self.assert_not_configured(result)
        self.assertIn("HTTP 302", result.stderr)

    def test_http_failure_has_input_context_without_changing_config(self):
        self.respond({"cosmos-4.0.2.json": {"status": "404", "exit": 22}})
        result = self.run_bootstrap()
        self.assert_not_configured(result)
        self.assertIn("ci/github_sources/cosmos-4.0.2.json (HTTP 404)", result.stderr)

    def test_failed_member_preflight_does_not_configure_bazel(self):
        self.env["READER_REJECT_INPUTS"] = "1"
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.workspace / "reader-called").exists())
        self.assertEqual(self.user_config.read_text(), "build --jobs=2\n")
        self.assertFalse(self.generated_config.exists())
        self.assertIn("Bazel configuration was not changed", result.stderr)

    def test_failed_repreparation_preserves_existing_configuration(self):
        self.assertEqual(self.run_bootstrap().returncode, 0)
        previous = self.generated_config.read_bytes()
        self.env["READER_REJECT_INPUTS"] = "1"
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.generated_config.read_bytes(), previous)
        self.assertEqual(self.user_config.read_text(), "build --jobs=2\n")

    def test_directory_cannot_be_executed_as_the_cached_reader(self):
        reader_directory = self.bootstrap / "github_sources.py"
        reader_directory.mkdir(parents=True)
        (reader_directory / "__main__.py").write_bytes(READER)
        self.assert_not_configured(self.run_bootstrap())


if __name__ == "__main__":
    unittest.main()
