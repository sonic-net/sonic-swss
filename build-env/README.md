# SWSS Docker-Based Build Environment

This directory is the single source of truth for sonic-swss build and VS-test
environment setup. CI and local development both consume the declarative YAML
through the shared
[`buildenv_setup`](https://github.com/sonic-net/sonic-swss-common/tree/master/ci)
tool hosted in sonic-swss-common.

## Contents

| Path | Purpose | Cascades downstream? |
|------|---------|----------------------|
| `packages/base.yaml` | Packages needed to build/link sonic-swss | **Yes** |
| `packages/tooling.yaml` | Build-only tools | No |
| `packages/test.yaml` | Shared VS test-host dependencies | N/A |
| `upstream-artifacts.yaml` | Build and test upstream bundles | **Yes** |
| `build.sh` | Canonical package build + Rust tests | - |
| `Dockerfile`, `compose.yaml` | Local development image/workflow | - |

For Build scope, sonic-swss declares only immediate artifact dependencies:
sonic-sairedis and sonic-dash-api. The sairedis artifact carries its own
`build-env/`, so buildenv_setup recursively inherits sonic-swss-common,
common-libs, VPP, packages, and the canonical Redis test configuration.

For Test scope, `packages/test.yaml` and the test-scoped artifact entries set up
the Ubuntu VS-test host. The swss-owned
`test-docker-sonic-vs-template.yml` consumes the same configuration for all
three dataplane repositories.

## CI

The Build template clones sonic-swss-common to obtain the shared tool, then runs:

```bash
PYTHONPATH=/tmp/sw-common/ci python3 -m buildenv_setup \
    --repo-dir "$(Build.SourcesDirectory)" \
    --scope build \
    --arch <arch> \
    --debian-version <version> \
    --branch "$(BUILD_BRANCH)"

GCOV=<true|false> ASAN=<true|false> ./build-env/build.sh
```

The published artifact retains the existing top-level DEBs and coverage files
and adds `build-env/` alongside them for future downstream cascade consumers.

## Local development

The existing non-root, home-mounted workflow is preserved:

1. From `build-env/`, run `./env_init.sh`. This records your UID/GID and creates
   ignored `custom-setup.sh` if needed.
2. Build and start the container:

   ```bash
   docker compose up -d swss-bookworm
   ```

3. Enter it with either:

   ```bash
   docker exec -it -u "${USER}" swss-bookworm-master bash
   # or
   ssh "${USER}"@172.19.0.10
   ```

4. Build the repository from its mounted host path:

   ```bash
   ./build-env/build.sh
   ```

   For alternate modes:

   ```bash
   GCOV=true ./build-env/build.sh
   ASAN=true ./build-env/build.sh
   ```

5. Stop the environment with `docker compose down`.

`SWSS_COMMON_REF` selects the sonic-swss-common branch containing
buildenv_setup; `BRANCH` selects the sonic-slave image and upstream artifact
branch. Both default to `master`.

## Custom setup

`env_init.sh` creates ignored `custom-setup.sh`. Commands placed there run as
root after the canonical dependency setup, allowing personal shell/tooling
customization without changing the shared environment definition.
