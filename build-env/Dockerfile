# Local development image for sonic-swss.
#
# CI uses the same sonic-slave base image and buildenv_setup configuration, but
# does not use this Dockerfile. Source stays mounted from the host so ordinary
# edits do not invalidate the dependency layer.

ARG DEBIAN=bookworm
ARG BRANCH=master
FROM sonicdev-microsoft.azurecr.io:443/sonic-slave-${DEBIAN}:${BRANCH}

ARG USER
ARG UID
ARG GID
ARG HOME
ARG DEBIAN
ARG BRANCH
ARG SWSS_COMMON_REF=master

RUN groupadd --gid "${GID}" "${USER}" \
 && useradd --gid "${GID}" --uid "${UID}" --create-home --home-dir "${HOME}" --shell /bin/bash "${USER}" \
 && usermod --append --groups sudo "${USER}"
RUN echo "${USER} ALL=(ALL) NOPASSWD:ALL" > "/etc/sudoers.d/${USER}" \
 && chmod 0440 "/etc/sudoers.d/${USER}" \
 && sed --in-place -E 's/^#?PermitRootLogin.*$/PermitRootLogin prohibit-password/g' /etc/ssh/sshd_config
RUN echo "* soft nofile 2048" >> /etc/security/limits.conf \
 && mkdir --parents /__w \
 && chown --recursive "${USER}:${USER}" /__w

# Obtain the shared tool, then copy only dependency declarations so the heavy
# setup layer is cached independently of source edits.
RUN git clone --depth 1 --branch "${SWSS_COMMON_REF}" \
      https://github.com/sonic-net/sonic-swss-common /tmp/sw-common
COPY packages/ /workspace/build-env/packages/
COPY upstream-artifacts.yaml /workspace/build-env/upstream-artifacts.yaml
COPY custom-setup.sh /tmp/custom-setup.sh

# Inline scripts inherited through the cascade may invoke sudo even though this
# layer runs as root, so keep sudo available while using --no-sudo for the
# installer's own apt/pip/dpkg commands.
RUN apt-get update \
 && apt-get install -y python3-yaml python3-requests sudo \
 && PYTHONPATH=/tmp/sw-common/ci python3 -m buildenv_setup \
      --repo-dir /workspace \
      --scope build \
      --arch amd64 \
      --debian-version "${DEBIAN}" \
      --branch "${BRANCH}" \
      --org-url https://dev.azure.com/mssonic \
      --no-sudo \
 && bash /tmp/custom-setup.sh

USER ${USER}
CMD ["sudo", "service", "ssh", "start", "-D"]
