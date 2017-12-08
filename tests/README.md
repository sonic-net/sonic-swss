SWSS Integration Tests

# Introduction

SWSS Integration tests runs on docker-sonic-vs which runs on top of SAI virtual switch. The tests can be run on any Linux machine without real switch ASIC. It is used to test SwSS (Switch State Service) by setting AppDB or ConfigDB and checking corresponding AsicDB entries.

# How to run the tests

- Install docker and pytest on your dev machine. In case "--system" option is not available, you can ignore the option and try.
    ```
    sudo pip install --system docker==2.6.1
    sudo pip install --system pytest==3.3.0
    ```
- Compile and install swss common library. Install libhiredis prior to installing libswss.
    ````
    cd sonic-swss-common
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

# How to setup test development env

To develop new swss features or swss integration tests, you need to setup a virtual switch docker container which 
persists.

- Create virtual switch container (name:vs). ```create_vnet.sh``` can be found at [here](https://github.com/Azure/sonic-buildimage/blob/master/platform/vs/create_vnet.sh).

    ```
    docker run -id --name sw debian bash
    sudo ./create_vnet.sh sw
    docker run --privileged -v /var/run/redis-vs:/var/run/redis --network container:sw -d --name vs docker-sonic-vs
    ```

- Run test using the existing vs container

    ```
    sudo pytest -v --dvsname=vs
    ```
