ARG DEBIAN="bookworm"
ARG BRANCH="master"
FROM sonicdev-microsoft.azurecr.io:443/sonic-slave-${DEBIAN}:${BRANCH}
ARG USER
ARG UID
ARG GID
ARG HOME
ARG DEBIAN
ARG BRANCH


COPY container-setup.py custom-setup.sh /tmp/

RUN groupadd --gid ${GID} ${USER}; useradd --gid ${GID} --uid ${UID} --create-home --home-dir ${HOME} --shell /bin/bash ${USER}; usermod --append --groups sudo ${USER}
RUN echo "${USER} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USER}; chmod 0440 /etc/sudoers.d/${USER}; sed --in-place -E 's/^#?PermitRootLogin.*$/PermitRootLogin prohibit-password/g' /etc/ssh/sshd_config
RUN echo "* soft nofile 2048" >> /etc/security/limits.conf
RUN mkdir --parents /__w; chown --recursive ${USER}:${USER} /__w

RUN apt-get update; python3 /tmp/container-setup.py --branch ${BRANCH} --debian-version ${DEBIAN}; bash /tmp/custom-setup.sh

USER ${USER}
CMD ["sudo", "service", "ssh", "start", "-D"]
