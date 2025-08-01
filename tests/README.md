# Docker Virtual Switch (DVS) Tests

## Introduction
The Docker Virtual Switch tests allow developers to validate the control plane behavior of new SWSS features without needing an actual network device or switching ASIC.

The DVS tests work by publishing configuration updates to redis (typically Config DB or App DB) and checking that the state of the system is correctly updated by SWSS (typically by checking ASIC DB).

SWSS, Redis, and all the other required components run inside a virtual switch Docker container, meaning that these test cases can be run on any Linux machine - no special hardware required!

## Contents
- [Setting up your test environment](#setting-up-your-test-environment)
- [Running the tests](#running-the-tests)
- [Setting up a persistent testbed](#setting-up-a-persistent-testbed)
- [Other useful test parameters](#other-useful-test-parameters)
- [Known Issues/FAQs](#known-issues)

## Setting up your test environment
### System Requirements

  To set up your test environment, you will need:

  - A machine running **Ubuntu 22.04**
  - **Python 3**

  You can check these dependencies with the following commands:

  ```bash
  cat /etc/os-release | grep ".*22.04*"
  uname -r | grep generic
  python3 --version
  ```
### Team Kernel Module

  **Note:**  
  Check if the `team` kernel module is already installed by running:

  ```bash
  lsmod | grep team
  ```

  If the output is non-empty, you're good to proceed to the next step.  
  If the output is empty, run the following script to install the necessary modules:

```bash
sudo .azure-pipelines/build_and_install_module.sh
```

Once the script completes successfully, verify again:

```bash
lsmod | grep team
```

### Install Docker CE

  Install [Docker CE](https://docs.docker.com/engine/install/ubuntu/) from the official documentation.

  **Important:** Follow the [post-install instructions](https://docs.docker.com/engine/install/linux-postinstall/) to avoid needing `sudo` for Docker commands.
  

### Install External Dependencies

  Install packages required for running the VS tests:

```bash
sudo apt-get install -y net-tools bridge-utils vlan libzmq3-dev libzmq5 \
  libboost-serialization1.74.0 libboost1.74-dev libboost-dev \
  libhiredis0.14 libyang-dev

sudo apt install -y python3-pip net-tools bridge-utils ethtool vlan \
  libnl-nf-3-200 libnl-cli-3-200

sudo pip3 install docker pytest flaky redis distro dataclasses fstring \
  exabgp docker lcov_cobertura
```

### Install DASH Dependencies (Ubuntu 22.04)

[Download latest artifacts](https://dev.azure.com/mssonic/build/_build?definitionId=1055&_a=summary&repositoryFilter=158&branchFilter=11237%2C11237%2C11237%2C11237%2C11237)

```bash
sudo dpkg -i libdashapi.deb libprotobuf32.deb
```

### Install swsscommon

[Download latest artifacts](https://sonic-build.azurewebsites.net/api/sonic/artifacts?branchName=master&definitionId=9&artifactName=sonic-swss-common.amd64.ubuntu22_04)

```bash
sudo dpkg -i libswsscommon.deb
sudo dpkg -i python_swsscommon.deb
```

### Download the latest `docker-sonic-vs.gz` file from Azure build artifacts.

```bash
wget -O docker-sonic-vs.gz "https://sonic-build.azurewebsites.net/api/sonic/artifacts?branchName=master&platform=vs&target=target/docker-sonic-vs.gz"
```
  Load the image into Docker:

```bash
docker load < docker-sonic-vs.gz
```

## Running the tests
```
cd sonic-swss/tests
sudo pytest
```

## Setting up a persistent testbed
For those developing new features for SWSS or the DVS framework, you might find it helpful to setup a persistent DVS container that you can inspect and make modifications to (e.g. using `dpkg -i` to install a new version of SWSS to test a new feature).

1. [Download `create_vnet.sh`](https://github.com/sonic-net/sonic-buildimage/blob/master/platform/vs/create_vnet.sh).

2. Setup a virtual server and network. **Note**: It is _highly_ recommended you include the `-n 32` option or you may run into problems running the tests later.

    ```
    docker run --privileged -id --name sw debian bash
    sudo ./create_vnet.sh -n 32 sw
    ```

3. Start the DVS container.

    ```
    docker run --privileged -v /var/run/redis-vs/sw:/var/run/redis --network container:sw -d --name vs docker-sonic-vs
    ```

4. You can specify your persistent DVS container when running the tests as follows.
    
    ```
    sudo pytest --dvsname=vs
    ```

    By default if the DVS has <32 ports (needed by testbed), then test will be aborted. To overcome that the --forcedvs option can be used. This automatically adds the proper number of ports for the tests to run, and cleans up the extra ports after the test is complete.

    ```
    sudo pytest --dvsname=vs --forcedvs
    ```

5. Additionally, if you need to simulate a specific hardware platform (e.g. Broadcom or Mellanox), you can add this environment variable for hardware SKU when starting the DVS container. Note that this is not a precise 1-to-1 model, and dataplane behavior is not simulated by the DVS.

    ```
    -e "HWSKU=Mellanox-SN2700"
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
- The test run might abort before any cases are run:
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
    dpkg -i libswsscommon_1.0.0_amd64.deb python3-swsscommon_1.0.0_amd64.deb
    ```

- The tests might report a syntax error:

    ```
    ImportError while loading conftest '/sonic/src/sonic-swss/tests/conftest.py'.
    File "/sonic/src/sonic-swss/tests/conftest.py", line 56
        def __init__(self, db_id: str, connector: str):
    ```

    This indicates that `pytest` is using `python2` rather than `python3`. You need to install and use `pytest` for `python3`.

- You may encounter the following error message:
    ```
    ERROR: Error response from daemon: client is newer than server (client API version: x.xx, server API version: x.xx)
    ```

    You can mitigate this by upgrading to a newer version of Docker CE or editing the `DEFAULT_DOCKER_API_VERSION` in `/usr/local/lib/python3/dist-packages/docker/constants.py`, or by upgrading to a newer version of Docker CE. See [relevant GitHub discussion](https://github.com/drone/drone/issues/2048).

- Currently when pytest is run using `--force-flaky` and the last test case fails, then pytest tears down the module before retrying the failed test case and invokes the module setup again to run the failed test case. This is a known issue with [flaky](https://github.com/box/flaky/issues/128) and [pytest](https://github.com/pytest-dev/pytest-rerunfailures/issues/51).
    
    Because of this issue, all the logs are lost except for the last test case as modules are torn down and set up again. The workaround for this is to include a dummy test case that always passes at the end of all test files/modules.

- Too many open files

    If some tests end with the error "Too many open files", you should check the maximum number of open files that are permitted on your system:
    ```
    ulimit -a | grep "open files"
    ```
    You can increase it by executing this command: `ulimit -n 8192`. Feel free to change `8192`. This value worked fine for me.

    **Note:** This change is only valid for the current terminal session. If you want a persistent change, append `ulimit -n 8192` to `~/.bashrc`.
