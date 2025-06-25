# SWSS Docker-Based Build Environment

This directory contains scripts to create a docker image which serves as a build environment for SWSS. The image is designed to closely mimic the build environment used for SWSS in Azure Pipelines. The primary motivation behind this container is to provide an easy way to run the C++ unit tests (`tests/mock_tests`) locally.

## Getting Started

Note: Your home directory on the host machine will be mounted to the home directory inside the container. This means that any settings in your home directory (e.g. authorized SSH keys or `.bashrc` settings) will also apply in the container and files inside your host home directory will be accessible inside the container.

From this directory (`sonic-swss/build-env`):
1. Before building the image for the first time, run the `env_init.sh` script.
2. Build the image and start the container with `docker compose up -d swss-bookworm`.
3. Enter the container either with `docker exec -it -u ${USER} swss-bookworm-master bash` or via SSH `ssh ${USER}@172.19.0.10`.
4. Once inside the container, navigate to the unit test directory `sonic-swss/tests/mock_test` 
5. From here, you can build and run all unit tests using `make check`. You can also build a run a specific test binary, e.g. `make tests` and then `./tests`.
    ```
    lawlee@e77fd85f06b6:~/repos/sonic-swss/tests/mock_tests$ make check
    make  check-TESTS
    make[1]: Entering directory '/home/lawlee/repos/sonic-swss/tests/mock_tests'
    make[2]: Entering directory '/home/lawlee/repos/sonic-swss/tests/mock_tests'
    PASS: tests
    PASS: tests_intfmgrd
    PASS: tests_teammgrd
    PASS: tests_portsyncd
    PASS: tests_fpmsyncd
    PASS: tests_response_publisher
    ============================================================================
    Testsuite summary for sonic-swss 1.0
    ============================================================================
    # TOTAL: 6
    # PASS:  6
    # SKIP:  0
    # XFAIL: 0
    # FAIL:  0
    # XPASS: 0
    # ERROR: 0
    ============================================================================
    make[2]: Leaving directory '/home/lawlee/repos/sonic-swss/tests/mock_tests'
    make[1]: Leaving directory '/home/lawlee/repos/sonic-swss/tests/mock_tests'
    lawlee@e77fd85f06b6:~/repos/sonic-swss/tests/mock_tests$
    ```
6. To shut down and remove the container, run `docker compose down swss-bookworm` on the host.
7. To rebuild the container image (e.g. in case of dependency changes), run `docker compose build --pull`

## Debugging Failing Tests

If tests are failing unexpectedly, build artifacts may be out-of-date but not being rebuilt due to switching branches. From `tests/mock_tests`, run `make clean` and then rebuild and re-run the tests.

To run individual test classes/cases, use the `--gtest_filter` flag with the test binary. E.g. to run only tests in the `DashOrchTest` class from the main set of orchagent unit tests:
1. `cd tests/mock_tests` and `make tests` to build the test binary.
2. `./tests --gtest_filter="DashOrchTest*"` to run this test class only. `./tests --gtest_filter="DashOrchTest.SetEniMode"` to run this test case only.
3. `./tests --gtest_list_tests` will print all available test cases.

The tests are built with debug symbols. A simple way to run a test in GDB: `gdb --args ./tests --gtest_filter="DashOrchTest*"`.