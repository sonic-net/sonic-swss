# Docker Virtual Switch (DVS) Tests

## Introduction
The Docker Virtual Switch tests allow developers to validate the control plane behavior of new SWSS features without needing an actual network device or switching ASIC.

The DVS tests work by publishing configuration updates to redis (typically Config DB or App DB) and checking that the state of the system is correctly updated by SWSS (typically by checking ASIC DB).

SWSS, Redis, and all the other required components run inside a virtual switch Docker container, meaning that these test cases can be run on any Linux machine - no special hardware required!

## Setting up your test environment
1. [Install Docker CE](https://docs.docker.com/install/linux/docker-ce/ubuntu/). Be sure to follow the [post-install instructions](https://docs.docker.com/install/linux/linux-postinstall/) so that you don't need sudo privileges to run docker commands.
2. Install the external dependencies needed to run the tests:

    ```
    sudo modprobe team
    sudo apt install net-tools ethtool
    sudo pip install docker pytest flaky redis
    ```
3. Install `python-swsscommon_1.0.0_amd64.deb`. You will need to install all the dependencies as well in the following order:

    ```
    sudo dpkg -i libnl-3-200_3.5.0-1_amd64.deb
    sudo dpkg -i libnl-genl-3-200_3.5.0-1_amd64.deb
    sudo dpkg -i libnl-route-3-200_3.5.0-1_amd64.deb
    sudo dpkg -i libnl-nf-3-200_3.5.0-1_amd64.deb
    sudo dpkg -i libhiredis0.14_0.14.0-3~bpo9+1_amd64.deb
    sudo dpkg -i libswsscommon_1.0.0_amd64.deb
    sudo dpkg -i python-swsscommon_1.0.0_amd64.deb
    ```

    You can find the dependencies [here](https://sonic-jenkins.westus2.cloudapp.azure.com/job/vs/job/buildimage-vs-all/lastSuccessfulBuild/artifact/target/debs/stretch/), and get this package by:
    - [Building it from scratch](https://github.com/Azure/sonic-swss-common)
    - [Downloading the latest build from Jenkins](https://sonic-jenkins.westus2.cloudapp.azure.com/job/common/job/sonic-swss-common-build/lastSuccessfulBuild/artifact/target/)
4. Load the `docker-sonic-vs.gz` file into docker. You can get the image by:
    - [Building it from scratch](https://github.com/Azure/sonic-buildimage)
    - [Downloading the latest build from Jenkins](https://sonic-jenkins.westus2.cloudapp.azure.com/job/vs/job/buildimage-vs-all/lastSuccessfulBuild/artifact/target/)
    
    Once you have the file, you can load it into docker by running `docker load < docker-sonic-vs.gz`.

## Running the tests
```
cd sonic-swss/tests
sudo pytest
```

## Setting up a persistent test environment
For those developing new features for SWSS or the DVS framework, you might find it helpful to setup a persistent DVS container that you can inspect and make modifications to (e.g. using `dpkg -i` to install a new version of SWSS to test a new feature).

1. [Download `create_vnet.sh`](https://github.com/Azure/sonic-buildimage/blob/master/platform/vs/create_vnet.sh).
2. Setup a virtual server and network:

    ```
    docker run --privileged -id --name sw debian bash
    sudo ./create_vnet.sh sw
    ```
3. Start the DVS container:

    ```
    docker run --privileged -v /var/run/redis-vs/sw:/var/run/redis --network container:sw -d --name vs docker-sonic-vs
    ```

4. You can specify your persistent DVS container when running the tests as follows:
    
    ```
    sudo pytest --dvsname=vs
    ```

5. Additionally, if you need to simulate a specific hardware platform (e.g. Broadcom or Mellanox), you can add this environment variable when starting the DVS container:

    ```
    -e "fake_platform=mellanox"
    ```

## Other useful test parameters
- You can see the output of all test cases that have been run by adding the verbose flag:

    ```
    sudo pytest -v
    ```

    This works for persistent DVS containers as well.

- You can specify a specific test file, class, or even individual test case when you run the tests:

    ```
    sudo pytest <test_file_name>::<test_class_name>::<test_name>
    ```

    This works for persistent DVS containers as well.

- You can specify a specific image:tag to use when running the tests *without a persistent DVS container*:

    ```
    sudo pytest --imgname=docker-sonic-vs:my-changes.333
    ```

- You can automatically retry failed test cases **once**:

    ```
    sudo pytest --force-flaky
    ```

## Known Issues
-   ```
    ERROR: Error response from daemon: client is newer than server (client API version: x.xx, server API version: x.xx)
    ```

    You can mitigate this by editing the `DEFAULT_DOCKER_API_VERSION` in `/usr/local/lib/python2.7/dist-packages/docker/constants.py`, or by upgrading to a newer version of Docker CE. See [relevant GitHub discussion](https://github.com/drone/drone/issues/2048).
    