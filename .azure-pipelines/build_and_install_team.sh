#!/bin/bash
#
# build and install team driver
#

set -e

source /etc/os-release

function build_and_install_team()
{
    TEAM_DIR=$(echo /lib/modules/$(uname -r)/kernel/net/team)
    if sudo modprobe team 2>/dev/null || [ -e "$TEAM_DIR/team.ko" ]; then
        echo "The module team or $TEAM_DIR/team.ko exists."
        return
    fi

    [ -z "$WORKDIR" ] && WORKDIR=$(mktemp -d)
    cd $WORKDIR

    KERNEL_RELEASE=$(uname -r)
    KERNEL_MAINVERSION=$(echo $KERNEL_RELEASE | cut -d- -f1)
    EXTRAVERSION=$(echo $KERNEL_RELEASE | cut -d- -f2)
    LOCALVERSION=$(echo $KERNEL_RELEASE | cut -d- -f3)
    VERSION=$(echo $KERNEL_MAINVERSION | cut -d. -f1)
    PATCHLEVEL=$(echo $KERNEL_MAINVERSION | cut -d. -f2)
    SUBLEVEL=$(echo $KERNEL_MAINVERSION | cut -d. -f3)

    # Install the required debian packages to build the kernel modules
    apt-get install -y build-essential linux-headers-${KERNEL_RELEASE} autoconf pkg-config fakeroot
    apt-get install -y flex bison libssl-dev libelf-dev
    apt-get install -y libnl-route-3-200 libnl-route-3-dev libnl-cli-3-200 libnl-cli-3-dev libnl-3-dev

    # Add the apt source mirrors and download the linux image source code
    cp /etc/apt/sources.list /etc/apt/sources.list.bk
    sed -i "s/^# deb-src/deb-src/g" /etc/apt/sources.list
    apt-get update
    apt-get source linux-image-unsigned-$(uname -r) > source.log

    # Recover the original apt sources list
    cp /etc/apt/sources.list.bk /etc/apt/sources.list
    apt-get update

    # Build the Linux kernel module drivers/net/team
    cd $(find . -maxdepth 1 -type d | grep -v "^.$")
    make  allmodconfig
    mv .config .config.bk
    cp /boot/config-$(uname -r) .config
    grep NET_TEAM .config.bk >> .config
    make VERSION=$VERSION PATCHLEVEL=$PATCHLEVEL SUBLEVEL=$SUBLEVEL EXTRAVERSION=-${EXTRAVERSION} LOCALVERSION=-${LOCALVERSION} modules_prepare
    make M=drivers/net/team

    # Install the module
    mkdir -p $TEAM_DIR
    cp drivers/net/team/*.ko $TEAM_DIR/
    modinfo $TEAM_DIR/team.ko
    depmod
    modprobe team
    cd /tmp
    rm -rf $WORKDIR
}


build_and_install_team
