FROM docker-sonic-vs

ARG docker_container_name
ARG need_dbg=n
ARG debug_pkg=swss-dbg
ARG build_dir
ARG include_gcov=y
ARG include_vpp=y
ARG include_nexthopgroup=n
ENV BUILD_DIR=$build_dir

COPY ["debs", "/debs"]

# Remove the existing packages before installing current artifacts. This avoids
# Docker layer diff issues when a rebuilt DEB retains its size and timestamp.
RUN dpkg --remove --force-all libswsscommon
RUN apt --fix-broken install -y
RUN dpkg --purge python3-swsscommon sonic-db-cli libsaimetadata libsairedis \
        libsaivs syncd-vs swss sonic-eventd libdashapi framework
RUN if [ "$include_nexthopgroup" = "y" ]; then \
        dpkg --purge libnexthopgroup || true; \
    fi

RUN apt-get update

# VPP maintainer scripts invoke sysctl operations that docker build cannot
# perform. Consumers that do not include VPP skip this block entirely.
RUN if [ "$include_vpp" = "y" ]; then \
        cp /usr/sbin/sysctl /usr/sbin/sysctl.org; \
        cp /usr/bin/true /usr/sbin/sysctl; \
        apt install -y $(ls /debs/*.deb | grep vpp); \
        mv /usr/sbin/sysctl.org /usr/sbin/sysctl; \
    fi

RUN apt install -y /debs/libdashapi_1.0.0_amd64.deb \
            /debs/libswsscommon_1.0.0_amd64.deb \
            /debs/python3-swsscommon_1.0.0_amd64.deb \
            /debs/sonic-db-cli_1.0.0_amd64.deb \
            /debs/libsaimetadata_1.0.0_amd64.deb \
            /debs/libsairedis_1.0.0_amd64.deb \
            /debs/libsaivs_1.0.0_amd64.deb \
            /debs/syncd-vs_1.0.0_amd64.deb \
            /debs/swss_1.0.0_amd64.deb

RUN if [ "$include_nexthopgroup" = "y" ]; then \
        apt install -y /debs/libnexthopgroup_1.0.0_amd64.deb; \
    fi

RUN if [ "$need_dbg" = "y" ]; then \
        dpkg -i "/debs/${debug_pkg}_1.0.0_amd64.deb"; \
    fi

COPY ["start.sh", "/usr/bin/"]

RUN pip3 install scapy==2.5.0

RUN apt-get -y install software-properties-common libdatetime-perl \
        libcapture-tiny-perl build-essential libcpanel-json-xs-perl git \
        python3-protobuf

RUN if [ "$include_gcov" = "y" ]; then \
        git clone -b v2.0 --single-branch --depth 1 https://github.com/linux-test-project/lcov; \
        cd lcov; \
        make install; \
        lcov --version; \
        pip3 install lcov_cobertura; \
    fi

RUN if [ "$include_gcov" = "y" ] && [ -n "$BUILD_DIR" ]; then \
        mkdir -p "$BUILD_DIR"; \
        tar -xf /tmp/gcov/gcov-source.tar -C "$BUILD_DIR"; \
    fi
