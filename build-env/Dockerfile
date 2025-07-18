ARG DEBIAN="bookworm"
FROM sonicdev-microsoft.azurecr.io:443/sonic-slave-${DEBIAN}:latest
ARG USER
ARG UID
ARG GID
ARG HOME

ARG BRANCH="master"

COPY container-setup.py custom-setup.sh /tmp/

RUN python3 /tmp/container-setup.py --branch ${BRANCH}; bash /tmp/custom-setup.sh

RUN groupadd -o -g ${GID} ${USER}; useradd -o -l -g ${GID} -u ${UID} -m -d ${HOME} -s /bin/bash ${USER}; usermod -a -G sudo ${USER}
RUN echo "${USER} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USER}; chmod 0440 /etc/sudoers.d/${USER}; sed -i -E 's/^#?PermitRootLogin.*$/PermitRootLogin yes/g' /etc/ssh/sshd_config
RUN echo "* soft nofile 2048" >> /etc/security/limits.conf
RUN mkdir -p /__w; chown -R ${USER}:${USER} /__w

USER ${USER}
CMD ["sudo", "service", "ssh", "start", "-D"]
