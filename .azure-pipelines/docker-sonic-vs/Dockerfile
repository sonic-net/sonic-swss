FROM docker-sonic-vs

ARG docker_container_name
ARG need_dbg

ADD ["debs", "/debs"]

RUN dpkg --purge python-swsscommon python3-swsscommon swss libsairedis sonic-db-cli libswsscommon libsaimetadata libsaivs syncd-vs

RUN dpkg -i /debs/libswsscommon_1.0.0_amd64.deb
RUN dpkg -i /debs/python-swsscommon_1.0.0_amd64.deb
RUN dpkg -i /debs/python3-swsscommon_1.0.0_amd64.deb

RUN dpkg -i /debs/libsaimetadata_1.0.0_amd64.deb
RUN dpkg -i /debs/libsairedis_1.0.0_amd64.deb
RUN dpkg -i /debs/libsaivs_1.0.0_amd64.deb
RUN dpkg -i /debs/syncd-vs_1.0.0_amd64.deb

RUN dpkg --purge swss
RUN dpkg -i /debs/swss_1.0.0_amd64.deb
RUN if [ "$need_dbg" = "y" ] ; then dpkg -i /debs/swss-dbg_1.0.0_amd64.deb ; fi

RUN apt-get update

RUN apt-get -y install lcov
