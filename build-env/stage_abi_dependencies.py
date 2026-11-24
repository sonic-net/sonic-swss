#!/usr/bin/env python3
"""Stage a verified ABI-coherent pair for the ARM64/Trixie CI build."""

import argparse
import shutil
import tempfile
from pathlib import Path

from buildenv_setup.azp_client import ArtifactRef, AzpClient


# sairedis 1284046 allocated the old NotificationConsumer layout, while newer
# swss-common constructors wrote past it. 1285301 was built against 1284926.
# Remove this temporary pair once upstream artifacts preserve ABI provenance.
PAIR = (
    ("sonic-swss-common", "Azure.sonic-swss-common", 1284926),
    ("sonic-sairedis", "Azure.sonic-sairedis", 1285301),
)


def stage_abi_dependencies(arch, debian_version, staged_dir, client=None):
    if (arch, debian_version) != ("arm64", "trixie"):
        return False
    staged_dir = Path(staged_dir)
    # Explicit current-run staging retains precedence over download defaults,
    # including callers staging just one upstream and managing its dependencies.
    if any((staged_dir / name).exists() for name, _, _ in PAIR):
        return False
    client = client if client is not None else AzpClient()
    with tempfile.TemporaryDirectory(prefix="swss-abi-pair-") as directory:
        bundles = []
        for name, pipeline, run_id in PAIR:
            ref = ArtifactRef(project="build", pipeline=pipeline,
                              artifact_name=name + "-trixie.arm64", branch="master",
                              run_id=run_id)
            bundles.append((name, run_id, client.fetch_artifact(ref, directory)))
        # Fetch both successfully before publishing either staged dependency.
        staged_dir.mkdir(parents=True, exist_ok=True)
        for name, run_id, bundle in bundles:
            shutil.copytree(bundle, staged_dir / name)
            print(f"Staged ABI-compatible {name} from Azure run {run_id}", flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--debian-version", required=True)
    parser.add_argument("--staged-dir", required=True)
    args = parser.parse_args()
    stage_abi_dependencies(args.arch, args.debian_version, args.staged_dir)


if __name__ == "__main__":
    main()
