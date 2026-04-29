"""Third party dependencies.

Please read carefully before adding new dependencies:
- Any dependency can break all of swss. Please be mindful of that before
  adding new dependencies. Try to stick to stable versions of widely used libraries.
  Do not depend on private repositories and forks.
- Fix dependencies to a specific version or commit, so upstream changes cannot break
  swss. Prefer releases over arbitrary commits when both are available.
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def swss_deps():
    """Sets up 3rd party workspaces needed to build swss."""
    if not native.existing_rule("com_google_googletest"):
        http_archive(
            name = "com_google_googletest",
            sha256 = "78c676fc63881529bf97bf9d45948d905a66833fbfa5318ea2cd7478cb98f399",
            strip_prefix = "googletest-1.16.0",
            urls = ["https://github.com/google/googletest/releases/download/v1.16.0/googletest-1.16.0.tar.gz"],
        )

    if not native.existing_rule("com_github_grpc_grpc"):
        http_archive(
            name = "com_github_grpc_grpc",
            url = "https://github.com/grpc/grpc/archive/v1.58.0.zip",
            strip_prefix = "grpc-1.58.0",
            sha256 = "aa329c7de707a03511c88206ef4483e9346ab6336b6be4378d294060aa7400b3",
            patch_args = ["-p1"],
        )

    if not native.existing_rule("com_google_protobuf"):
        http_archive(
            name = "com_google_protobuf",
            url = "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v23.1.zip",
            strip_prefix = "protobuf-23.1",
            sha256 = "c0ea9f4d75b37ea8e9d78ce4c670d066bcb7cebdba190fa5fc8c57b1f00c0c2c",
        )

    if not native.existing_rule("rules_pkg"):
        http_archive(
            name = "rules_pkg",
            urls = [
                "https://mirror.bazel.build/github.com/bazelbuild/rules_pkg/releases/download/1.0.1/rules_pkg-1.0.1.tar.gz",
                "https://github.com/bazelbuild/rules_pkg/releases/download/1.0.1/rules_pkg-1.0.1.tar.gz",
            ],
            sha256 = "d20c951960ed77cb7b341c2a59488534e494d5ad1d30c4818c736d57772a9fef",
        )

    if not native.existing_rule("rules_foreign_cc"):
        http_archive(
            name = "rules_foreign_cc",
            sha256 = "4b33d62cf109bcccf286b30ed7121129cc34cf4f4ed9d8a11f38d9108f40ba74",
            strip_prefix = "rules_foreign_cc-0.11.1",
            url = "https://github.com/bazelbuild/rules_foreign_cc/releases/download/0.11.1/rules_foreign_cc-0.11.1.tar.gz",
        )
