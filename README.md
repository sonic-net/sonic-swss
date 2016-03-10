[![Build Status](https://sonic-jenkins.westus.cloudapp.azure.com/buildStatus/icon?job=swss-build)](https://sonic-jenkins.westus.cloudapp.azure.com/job/swss-build)
# Switch State Service - swss

### This software repository must be treated as private and is only shared under NDA terms with Microsoft until publicly released.

# Description
The Switch State Service (SwSS) is a collection of software that provides a database interface for communication with and state representation of network applications and network switch hardware.

# Getting Started

## Install from Debian Repo

For your convenience, you can install prepared packages on Debian Jessie:

    echo 'deb http://apt-mo.trafficmanager.net/repos/sonic/ trusty main' >> /etc/apt/sources.list.d/sonic.list
    sudo apt-get update
    sudo apt-get install sonic-swss

## Install from Source

You can compile and install from source using:

    git clone https://github.com/Azure/swss.git
    #TODO: ./getdeps.sh
    make && sudo make install

You can also build a debian package using:

    fakeroot debian/rules binary

## Need Help?

For general questions, setup help, or troubleshooting:
- [sonicproject on Google Groups](https://groups.google.com/d/forum/sonicproject)

For bug reports or feature requests, please open an Issue.

# Contribution guide

All contributors must sign a contribution license agreement before contributions can be accepted.  Contact kasubra@microsoft.com or daloher@microsoft.com.  Later this will be automated.

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

