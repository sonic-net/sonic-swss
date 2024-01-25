FROM docker-sonic-vs

ARG docker_container_name
ARG need_dbg
ARG build_dir
ENV BUILD_DIR=$build_dir

COPY ["debs", "/debs"]

# Remove existing packages first before installing the new/current packages. This is to overcome limitations with
# Docker's diff detection mechanism, where only the file size and the modification timestamp (which will remain the
# same, even though contents have changed) are checked between the previous and current layer.
RUN dpkg --purge libswsscommon python3-swsscommon sonic-db-cli libsaimetadata libsairedis libsaivs syncd-vs swss sonic-eventd libdashapi

RUN dpkg -i /debs/libdashapi_1.0.0_amd64.deb \
            /debs/libswsscommon_1.0.0_amd64.deb \
            /debs/python3-swsscommon_1.0.0_amd64.deb \
            /debs/sonic-db-cli_1.0.0_amd64.deb \
            /debs/libsaimetadata_1.0.0_amd64.deb \
            /debs/libsairedis_1.0.0_amd64.deb \
            /debs/libsaivs_1.0.0_amd64.deb \
            /debs/syncd-vs_1.0.0_amd64.deb \
            /debs/swss_1.0.0_amd64.deb

RUN if [ "$need_dbg" = "y" ] ; then dpkg -i /debs/swss-dbg_1.0.0_amd64.deb ; fi

RUN apt-get update

RUN apt-get -y install software-properties-common libdatetime-perl libcapture-tiny-perl build-essential libcpanel-json-xs-perl git

RUN git clone -b v2.0 --single-branch --depth 1 https://github.com/linux-test-project/lcov && cd lcov && make install

RUN lcov --version

RUN pip3 install lcov_cobertura

RUN if [ -n "$BUILD_DIR" ]; then mkdir -p $BUILD_DIR && tar -xf /tmp/gcov/gcov-source.tar -C $BUILD_DIR; fi
