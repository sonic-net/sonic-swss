Broadcom[![Broadcom](https://sonic-jenkins.westus2.cloudapp.azure.com/job/broadcom/job/sonic-swss-build/badge/icon)](https://sonic-jenkins.westus.cloudapp.azure.com/job/broadcom/job/sonic-swss-build/)
Cavium[![Cavium](https://sonic-jenkins.westus2.cloudapp.azure.com/job/cavium/job/sonic-swss-build/badge/icon)](https://sonic-jenkins.westus.cloudapp.azure.com/job/cavium/job/sonic-swss-build/)
Mellanox[![Mellanox](https://sonic-jenkins.westus2.cloudapp.azure.com/job/mellanox/job/sonic-swss-build/badge/icon)](https://sonic-jenkins.westus.cloudapp.azure.com/job/mellanox/job/sonic-swss-build/)
P4[![P4](https://sonic-jenkins.westus2.cloudapp.azure.com/job/p4/job/sonic-swss-build/badge/icon)](https://sonic-jenkins.westus.cloudapp.azure.com/job/p4/job/sonic-swss-build/)

# SONiC - SWitch State Service - SWSS

## Description
The SWitch State Service (SWSS) is a collection of software that provides a database interface for communication with and state representation of network applications and network switch hardware.

## Getting Started

### Install

Before installing, add key and package sources:

    sudo apt-key adv --keyserver apt-mo.trafficmanager.net --recv-keys 417A0893
    echo 'deb http://apt-mo.trafficmanager.net/repos/sonic/ trusty main' | sudo tee -a /etc/apt/sources.list.d/sonic.list
    sudo apt-get update

Install dependencies:

    sudo apt-get install redis-server -t trusty
    sudo apt-get install libhiredis0.13 -t trusty
    
Install building dependencies:

    sudo apt-get install libtool
    sudo apt-get install autoconf automake
    sudo apt-get install dh-exec

There are a few different ways you can install SONiC-SWSS.

#### Install from Debian Repo

For your convenience, you can install prepared packages on Debian Jessie:

    sudo apt-get install swss

#### Install from Source

Checkout the source: `git clone https://github.com/Azure/sonic-swss.git` and install it yourself.

Get SAI header files into /usr/include/sai. Put the SAI header files that you use to compile
libsairedis into /usr/include/sai

Install prerequisite packages:

    sudo apt-get install libswsscommon libswsscommon-dev libsairedis libsairedis-dev

You can compile and install from source using:

    ./autogen.sh
    ./configure
    make && sudo make install

You can also build a debian package using:

    ./autogen.sh
    fakeroot debian/rules binary

## Need Help?

For general questions, setup help, or troubleshooting:
- [sonicproject on Google Groups](https://groups.google.com/d/forum/sonicproject)

For bug reports or feature requests, please open an Issue.

## Contribution guide

See the [contributors guide](https://github.com/Azure/SONiC/blob/gh-pages/CONTRIBUTING.md) for information about how to contribute.

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

