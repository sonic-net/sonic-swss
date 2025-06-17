ARG DEBIAN="bookworm"
FROM sonicdev-microsoft.azurecr.io:443/sonic-slave-${DEBIAN}:latest
ARG USER
ARG UID
ARG GID
ARG HOME

ARG BRANCH="master"

COPY container-setup.py /tmp

RUN python3 /tmp/container-setup.py --branch ${BRANCH}

RUN groupadd -o -g ${GID} ${USER}
RUN useradd -o -l -g ${GID} -u ${UID} -m -d ${HOME} -s /bin/bash ${USER};
RUN usermod -a -G sudo ${USER}
RUN echo "${USER} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USER}
RUN chmod 0440 /etc/sudoers.d/${USER}
RUN sed -i -E 's/^#?PermitRootLogin.*$/PermitRootLogin yes/g' /etc/ssh/sshd_config
RUN echo "* soft nofile 2048" >> /etc/security/limits.conf

CMD ["service", "ssh", "start", "-D"]
