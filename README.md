*static analysis:*

[![Total alerts](https://img.shields.io/lgtm/alerts/g/sonic-net/sonic-swss.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/sonic-net/sonic-swss/alerts/)
[![Language grade: Python](https://img.shields.io/lgtm/grade/python/g/sonic-net/sonic-swss.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/sonic-net/sonic-swss/context:python)
[![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/sonic-net/sonic-swss.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/sonic-net/sonic-swss/context:cpp)

*sonic-swss builds:*

[![master build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss?branchName=master&label=master)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=15&branchName=master)
[![202205 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss?branchName=202205&label=202205)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=15&branchName=202205)
[![202111 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss?branchName=202111&label=202111)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=15&branchName=202111)
[![202106 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss?branchName=202106&label=202106)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=15&branchName=202106)
[![202012 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss?branchName=202012&label=202012)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=15&branchName=202012)
[![201911 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss?branchName=201911&label=201911)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=15&branchName=201911)


# SONiC - SWitch State Service - SWSS

## Description
The SWitch State Service (SWSS) is a collection of software that provides a database interface for communication with and state representation of network applications and network switch hardware.

## Getting Started

### Prerequisites

Install the following dependencies:
```
sudo apt install redis-server
sudo apt install libhiredis0.14
sudo apt install libzmq5 libzmq3-dev
sudo apt install libboost-serialization1.74.0
sudo apt install libboost1.71-dev
sudo apt install libasan6
```
**Note:** If your are using Ubuntu 18.04, install `libhiredis0.13` instead.

Visit the [official sonic-buildimage Azure pipeline for the VS platform](https://dev.azure.com/mssonic/build/_build?definitionId=142&view=branches) and choose the branch that matches the sonic-swss branch you are trying to build or install. Then select the latest successful build.
From the Summary tab, access build artifacts.
<img width="1048" alt="image" src="https://github.com/user-attachments/assets/faa6f08d-788b-4801-8439-3f31a52efaa1">
Download the folder `sonic-buildimage.vs/target/debs/{your host machine's Debian code name}`. You can check the Debian code name of your machine by running `cat /etc/debian_version`.
<img width="1022" alt="image" src="https://github.com/user-attachments/assets/1ad750eb-252c-4913-b14f-91b5533a1295">
Extract the downloaded zip file using `unzip sonic-buildimage.vs.zip`. Then navigate to `sonic-buildimage.vs/target/debs/{Debian code name}/` and install the following Debian packages:
```
sudo dpkg -i libdashapi_1.0.0_amd64.deb libnl-3-200_3.5.0-1_amd64.deb libnl-3-dev_3.5.0-1_amd64.deb libnl-cli-3-200_3.5.0-1_amd64.deb libnl-cli-3-dev_3.5.0-1_amd64.deb libnl-genl-3-200_3.5.0-1_amd64.deb libnl-genl-3-dev_3.5.0-1_amd64.deb libnl-nf-3-200_3.5.0-1_amd64.deb libnl-nf-3-dev_3.5.0-1_amd64.deb libnl-route-3-200_3.5.0-1_amd64.deb libnl-route-3-dev_3.5.0-1_amd64.deb libprotobuf32_3.21.12-3_amd64.deb libsaimetadata_1.0.0_amd64.deb  libsaimetadata-dev_1.0.0_amd64.deb libsairedis_1.0.0_amd64.deb libsairedis-dev_1.0.0_amd64.deb libsaivs_1.0.0_amd64.deb libsaivs-dev_1.0.0_amd64.deb  libswsscommon_1.0.0_amd64.deb libswsscommon-dev_1.0.0_amd64.deb libteam5_1.31-1_amd64.deb libteamdctl0_1.31-1_amd64.deb libyang_1.0.73_amd64.deb libyang-dev_1.0.73_amd64.deb python3-swsscommon_1.0.0_amd64.deb
```
**Note:** You can also [build these packages yourself (for the VS platform)](https://github.com/sonic-net/sonic-buildimage/blob/master/README.md).

Now, you can either directly install the SONiC SWSS package or you can build it from source and then install it. To install the SONiC SWSS package that is already in `sonic-buildimage.vs/target/debs/{Debian code name}/`, simply run the following command:
```
sudo dpkg -i swss_1.0.0_amd64.deb
```

#### Install from Source

Install build dependencies:
```
sudo apt install libtool
sudo apt install autoconf automake
sudo apt install dh-exec
sudo apt install nlohmann-json3-dev
sudo apt install libgmock-dev
```

Clone the `sonic-swss` repository on your host machine: `git clone https://github.com/sonic-net/sonic-swss.git`.

Make sure that SAI header files exist in `/usr/include/sai`. Since you have already installed `libsairedis-dev`, `libsaimetadata-dev`, and `libsaivs-dev`, this should already be the case. If you have compiled `libsairedis` yourself, make sure that the SAI header files are copied to `/usr/include/sai`.

You can compile and install from source using:
```
./autogen.sh
./configure
make && sudo make install
```
**Note:** This will NOT run the mock tests located under `tests/mock_tests`.

You can also build a debian package using:
```
./autogen.sh
fakeroot debian/rules binary
```
## Common issues

#### Cannot find `libboost-serialization1.74.0`

Unfortunately, `libboost-serialization1.74.0` is not officially supported on Ubuntu 20.04 (focal) even though it is supported on Debian 11 (bullseye). Therefore, you must build this package from source. You can use a script similar to [this one](https://github.com/ulikoehler/deb-buildscripts/blob/master/deb-boost.sh), but you only need to create a package for the Boost serialization library. You should also make sure that the generated package is named `libboost-serialization1.74.0`. After the package is created, you can install it by running `sudo dpkg -i libboost-serialization1.74.0_1.74.0_amd64.deb`.

#### Dependency issue when installing `libzmq3-dev`

If you cannot install `libzmq3-dev` because of dependency issues, please check the version of `libkrb5` packages installed on your host machine:
```
    sudo dpkg -l | grep "libkrb5"
```
If the version is not `1.17-6ubuntu4.7`, then you need to install the correct version:

    sudo apt install libkrb5support0=1.17-6ubuntu4.7
    sudo apt install libzmq3-dev

**Warning:** This may remove many packages that are already installed on your system. Please take note of what is being removed.

**Note:** Do NOT install `*krb5*` packages that are located in the `sonic-buildimage.vs` folder that you downloaded. These packages have a higher version and will cause dependency issues.

#### Dependency issues when installing some package

If you run into dependency issues during the installation of a package, you can run `sudo apt -f install` to fix the issue. But note that if `apt` is unable to fix the dependency problem, it will attempt to remove the broken package(s).

#### Too many open files

If you get a C++ exception with the description "Too many open files" during the mock tests, you should check the maximum number of open files that are permitted on your system:
```
ulimit -a | grep "open files"
```
You can increase it by executing this command: `ulimit -n 8192`. Feel free to change `8192`. This value worked fine for me.

**Note:** This change is only valid for the current terminal session. If you want a persistent change, append `ulimit -n 8192` to `~/.bashrc`.

## Need Help?

For general questions, setup help, or troubleshooting:
- [sonicproject on Google Groups](https://groups.google.com/g/sonicproject)

For bug reports or feature requests, please open an Issue.

## Contribution guide

See the [contributors guide](https://github.com/sonic-net/SONiC/wiki/Becoming-a-contributor) for information about how to contribute.

### GitHub Workflow

We're following basic GitHub Flow. If you have no idea what we're talking about, check out [GitHub's official guide](https://guides.github.com/introduction/flow/). Note that merge is only performed by the repository maintainer.

Guide for performing commits:

* Isolate each commit to one component/bugfix/issue/feature
* Use a standard commit message format:

>     [component/folder touched]: Description intent of your changes
>
>     [List of changes]
>
> 	  Signed-off-by: Your Name your@email.com

For example:

>     swss-common: Stabilize the ConsumerTable
>
>     * Fixing autoreconf
>     * Fixing unit-tests by adding checkers and initialize the DB before start
>     * Adding the ability to select from multiple channels
>     * Health-Monitor - The idea of the patch is that if something went wrong with the notification channel,
>       we will have the option to know about it (Query the LLEN table length).
>
>       Signed-off-by: user@dev.null


* Each developer should fork this repository and [add the team as a Contributor](https://help.github.com/articles/adding-collaborators-to-a-personal-repository)
* Push your changes to your private fork and do "pull-request" to this repository
* Use a pull request to do code review
* Use issues to keep track of what is going on

hello
new run
