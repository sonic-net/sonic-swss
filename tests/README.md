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
    sudo apt install python3-pip net-tools ethtool vlan libnl-nf-3-200 libnl-cli-3-200
    sudo pip3 install docker pytest flaky redis distro
    ```

    If you are running **Ubuntu 18.04** you will need to install this package:
    ```
    sudo apt install libhiredis0.13
    ```

    If you are running **Ubuntu 20.04** you will need to install this package:
    ```
    sudo apt install libhiredis0.14
    ```
3. Install `python3-swsscommon_1.0.0_amd64.deb`. You will need to install all the dependencies as well in the following order:

    ```
    sudo dpkg -i libswsscommon_1.0.0_amd64.deb
    sudo dpkg -i python3-swsscommon_1.0.0_amd64.deb
    ```

    You can get these two packages by:
    - [Building it from scratch](https://github.com/Azure/sonic-swss-common)
    - Downloading the latest build from Jenkins:
      - [Ubuntu 18.04](https://sonic-jenkins.westus2.cloudapp.azure.com/job/common/job/sonic-swss-common-build-ubuntu/lastSuccessfulBuild/artifact/target/)
      - [Ubuntu 20.04](https://sonic-jenkins.westus2.cloudapp.azure.com/job/common/job/sonic-swss-common-build-ubuntu-20_04/lastSuccessfulBuild/artifact/target/)
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
    sudo ./create_vnet.sh -n 32 sw
    ```
3. Start the DVS container:

    ```
    docker run --privileged -v /var/run/redis-vs/sw:/var/run/redis --network container:sw -d --name vs docker-sonic-vs
    ```

4. You can specify your persistent DVS container when running the tests as follows:
    
    ```
    sudo pytest --dvsname=vs
    ```
    By default if number of ports in persistent DVS < 32 (needed by testbed) then test will be aborted. To overcome that --forcedvs option can be used.

    ```
    sudo pytest --dvsname=vs --forcedvs
    ```


5. Additionally, if you need to simulate a specific hardware platform (e.g. Broadcom or Mellanox), you can add this environment variable when starting the DVS container:

    ```
    -e "fake_platform=mellanox"
    ```

## Other useful test parameters
- You can specify a maximum amount of cores for the DVS to use (we recommend 2):
    ```
    sudo pytest --max_cpu 2
    ```

    For a persistent DVS:
    ```
    docker run --privileged -v /var/run/redis-vs/sw:/var/run/redis --network container:sw -d --name vs --cpus 2 docker-sonic-vs
    ```

    For specific details about the performance impact of this, see [the Docker docs.](https://docs.docker.com/config/containers/resource_constraints/#configure-the-default-cfs-scheduler)

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

- You can also preserve a non-persistent DVS container for debugging purposes:

    ```
    sudo pytest --keeptb
    ```

    Which should give you something like this in `docker ps`:

    ```
    CONTAINER ID        IMAGE                                                 COMMAND                  CREATED             STATUS              PORTS               NAMES
    10bb406e7475        docker-sonic-vs:sonic-swss-build.1529                 "/usr/bin/supervisord"   3 hours ago         Up 3 hours                              ecstatic_swartz
    edb35e9aa10b        debian:jessie                                         "bash"                   3 hours ago         Up 3 hours                              elegant_edison
    ```

- You can automatically retry failed test cases **once**:

    ```
    sudo pytest --force-flaky
    ```

## Known Issues
-   You may encounter the test run being aborted before any cases are run:
    ```
    daall@baker:~/sonic-swss/tests$ sudo pytest test_acl.py 
    ============================= test session starts ==============================
    platform linux -- Python 3.6.9, pytest-4.6.9, py-1.9.0, pluggy-0.13.1
    rootdir: /home/daall/sonic-swss/tests
    plugins: flaky-3.7.0
    collected 25 items                                                             

    test_acl.py Aborted
    ```
    
    When run with the `-sv` flags we get some more information:
    ```
    daall@baker:~/sonic-swss/tests$ sudo pytest -sv test_acl.py 
    ============================= test session starts ==============================
    platform linux -- Python 3.6.9, pytest-4.6.9, py-1.9.0, pluggy-0.13.1 -- /usr/bin/python3
    cachedir: .pytest_cache
    rootdir: /home/daall/sonic-swss/tests
    plugins: flaky-3.7.0
    collected 25 items                                                             

    test_acl.py::TestAcl::test_AclTableCreation terminate called after throwing an instance of 'std::runtime_error'
    what():  Sonic database config file doesn't exist at /var/run/redis/sonic-db/database_config.json
    Aborted
    ```

    This indicates that something went wrong with the `libswsscommon` installation. The following should mitigate the issue:
    ```
    dpkg -r libswsscommon python3-swsscommon
    dpkg --purge libswsscommon python3-swsscommon
    rm -rf /usr/lib/python3/dist-packages/swsscommon/
    dpkg -i libswsscommon.deb python3-swsscommon.deb
    ```

-   You may encounter the following error message:
    ```
    ERROR: Error response from daemon: client is newer than server (client API version: x.xx, server API version: x.xx)
    ```

    You can mitigate this by upgrading to a newer version of Docker CE or editing the `DEFAULT_DOCKER_API_VERSION` in `/usr/local/lib/python3/dist-packages/docker/constants.py`, or by upgrading to a newer version of Docker CE. See [relevant GitHub discussion](https://github.com/drone/drone/issues/2048).

-   Currently when pytest are run using --force-flaky and if the last test case fails pytest tear-down the module before retrying the failed test case and invoke module
    setup again to run fail test case. This is know issue of pytest w.r.t flaky as tracked here (https://github.com/box/flaky/issues/128) and 
    (https://github.com/pytest-dev/pytest-rerunfailures/issues/51). Because of this issue all the logs are lost till last test case run as modules is teardown and setup again.
    To avoid this as workaround a dummy always-pass test case is added in all modules/test files. 
