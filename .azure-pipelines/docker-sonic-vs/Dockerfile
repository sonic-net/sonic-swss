FROM docker-sonic-vs

ARG docker_container_name

ADD ["debs", "/debs"]

RUN dpkg --purge python-swsscommon
RUN dpkg --purge python3-swsscommon
RUN dpkg --purge swss
RUN dpkg --purge sonic-db-cli
RUN dpkg --purge libsairedis
RUN dpkg --purge libswsscommon
RUN dpkg --purge libsaimetadata
RUN dpkg --purge libsaivs
RUN dpkg --purge syncd-vs

RUN dpkg -i /debs/libswsscommon_1.0.0_amd64.deb
RUN dpkg -i /debs/python-swsscommon_1.0.0_amd64.deb
RUN dpkg -i /debs/python3-swsscommon_1.0.0_amd64.deb

RUN dpkg -i /debs/libsaimetadata_1.0.0_amd64.deb
RUN dpkg -i /debs/libsairedis_1.0.0_amd64.deb
RUN dpkg -i /debs/libsaivs_1.0.0_amd64.deb
RUN dpkg -i /debs/syncd-vs_1.0.0_amd64.deb
RUN dpkg -i /debs/sonic-db-cli_1.0.0_amd64.deb

RUN dpkg --purge swss
RUN dpkg -i /debs/swss_1.0.0_amd64.deb

RUN apt-get update

RUN apt-get -y install lcov
