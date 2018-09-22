SWSS Integration Tests

# Introduction

SWSS Integration tests runs on docker-sonic-vs which runs on top of SAI virtual switch. The tests can be run on any Linux machine without real switch ASIC. It is used to test SwSS (Switch State Service) by setting AppDB or ConfigDB and checking corresponding AsicDB entries.

# How to run the tests

- Install docker and pytest on your dev machine
    ```
    sudo pip install --system docker==3.5.0
    sudo pip install --system pytest==3.3.0 docker redis
    ```
- Compile and install swss common library. Follow instructions [here](https://github.com/Azure/sonic-swss-common/blob/master/README.md) to first install prerequisites to build swss common library. 
    ```
    cd sonic-swss-common
    ./autogen.sh
    dpkg-buildpackage -us -uc -b
    dpkg -i ../libswsscommon_1.0.0_amd64.deb
    dpkg -i ../python-swsscommon_1.0.0_amd64.deb
    ```
- Build and load docker-sonic-vs

    ```
    cd sonic-buildimage
    make configure PLATFORM=vs
    make all
    docker load < target/docker-sonic-vs.gz
    ```

- Run tests
    
    ```
    cd sonic-swss/tests
    sudo pytest -v
    ```

\* If you meet the error: client is newer than server, please edit the file `/usr/local/lib/python2.7/dist-packages/docker/constants.py` to update the `DEFAULT_DOCKER_API_VERSION` to mitigate this issue.

# How to setup test development env

To develop new swss features or swss integration tests, you need to setup a virtual switch docker container which 
persists.

- Create virtual switch container (name:vs). ```create_vnet.sh``` can be found at [here](https://github.com/Azure/sonic-buildimage/blob/master/platform/vs/create_vnet.sh).

    ```
    docker run --privileged -id --name sw debian bash
    sudo ./create_vnet.sh sw
    docker run --privileged -v /var/run/redis-vs:/var/run/redis --network container:sw -d --name vs docker-sonic-vs
    ```

- Run test using the existing vs container

    ```
    sudo pytest -v --dvsname=vs
    ```
