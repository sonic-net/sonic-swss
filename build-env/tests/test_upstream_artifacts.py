import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from buildenv_setup.cascade import collect_bundles
from buildenv_setup.model import Context
from buildenv_setup.schema import load_upstream_file

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from stage_abi_dependencies import stage_abi_dependencies


MANIFEST = Path(__file__).resolve().parents[1] / "upstream-artifacts.yaml"


class ArtifactClient:
    def __init__(self, context):
        self.context = context
        self.requests = []

    def fetch_artifact(self, ref, work_dir, subpaths=None):
        self.requests.append(ref)
        bundle = Path(work_dir) / ref.artifact_name
        bundle.mkdir(parents=True, exist_ok=True)
        if ref.pipeline == "Azure.sonic-sairedis":
            # Real sairedis bundles cascade to an unpinned SWSS-common entry.
            # Exercise that traversal and the resolver's first-occurrence rule.
            suffix = "" if self.context.arch == "amd64" else "." + self.context.arch
            nested = bundle / "build-env"
            nested.mkdir(exist_ok=True)
            (nested / "upstream-artifacts.yaml").write_text(
                "upstream:\n  - name: sonic-swss-common\n"
                "    pipeline: Azure.sonic-swss-common\n"
                "    cascade_optional: true\n"
                "    artifact_name: sonic-swss-common-"
                + self.context.debian_version + suffix + "\n"
            )
        return str(bundle)


class UpstreamArtifactsTest(unittest.TestCase):
    def collect(self, arch, debian, scope="build", staged=False):
        context = Context(arch, debian, "ubuntu22_04", scope, "master")
        client = ArtifactClient(context)
        with tempfile.TemporaryDirectory() as directory:
            staged_dir = Path(directory) / "staged"
            if staged:
                for name in ("sonic-swss-common", "sonic-sairedis"):
                    (staged_dir / name).mkdir(parents=True)
            stage_abi_dependencies(arch, debian, staged_dir, client)
            # Only artifact I/O is mocked; use the real schema, predicates,
            # recursive collection, deduplication and staged-override handling.
            with patch("buildenv_setup.cascade._glob_one",
                       side_effect=lambda bundle, pattern, *_: [str(Path(bundle) / pattern)]):
                collect_bundles(load_upstream_file(str(MANIFEST)), context,
                                client=client, work_dir=directory,
                                staged_dir=str(staged_dir))
        return client.requests

    def test_build_matrix_uses_matching_pair_only_for_arm64_trixie(self):
        for arch in ("amd64", "arm64", "armhf"):
            for debian in ("bookworm", "trixie"):
                with self.subTest(arch=arch, debian=debian):
                    requests = self.collect(arch, debian)
                    for pipeline, pinned in (("Azure.sonic-sairedis", 1285301),
                                             ("Azure.sonic-swss-common", 1284926)):
                        selected = [ref for ref in requests if ref.pipeline == pipeline]
                        self.assertEqual(len(selected), 1)
                        expected = pinned if (arch, debian) == ("arm64", "trixie") else None
                        self.assertEqual(selected[0].run_id, expected)

    def test_test_host_dependencies_are_not_pinned(self):
        self.assertTrue(all(ref.run_id is None
                            for ref in self.collect("amd64", "bookworm", scope="test")))

    def test_current_run_staged_pair_overrides_download_pins(self):
        requests = self.collect("arm64", "trixie", staged=True)
        self.assertFalse(any(ref.pipeline in ("Azure.sonic-sairedis", "Azure.sonic-swss-common")
                             for ref in requests))

    def test_failed_second_download_does_not_publish_partial_pair(self):
        client = ArtifactClient(Context("arm64", "trixie", "ubuntu22_04", "build", "master"))
        fetch = client.fetch_artifact

        def fail_second(ref, directory):
            if ref.pipeline == "Azure.sonic-sairedis":
                raise RuntimeError("artifact unavailable")
            return fetch(ref, directory)

        with tempfile.TemporaryDirectory() as directory:
            staged_dir = Path(directory) / "staged"
            with patch.object(client, "fetch_artifact", side_effect=fail_second):
                with self.assertRaisesRegex(RuntimeError, "artifact unavailable"):
                    stage_abi_dependencies("arm64", "trixie", staged_dir, client)
            self.assertFalse(staged_dir.exists())

    def test_explicit_partial_staging_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            staged_dir = Path(directory)
            marker = staged_dir / "sonic-sairedis" / "marker"
            marker.parent.mkdir()
            marker.write_text("current run")
            client = ArtifactClient(None)
            self.assertFalse(stage_abi_dependencies("arm64", "trixie", staged_dir, client))
            self.assertEqual(marker.read_text(), "current run")
            self.assertEqual(client.requests, [])


if __name__ == "__main__":
    unittest.main()
