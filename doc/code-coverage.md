# Code Coverage Implementation
Code coverage is a useful metric to determine how much of your code base is run during testing. The more source code is executed during testing, the less likely it is that there are undetected bugs in the code.

The code coverage for sonic-swss requires a bespoke implementation due to the testing framework used. The document aims to give an overview of this implementation so that other developers can understand/improve upon it when necessary. This document is written within the context of running tests and code coverage in Azure Pipelines (AZP). The code coverage implementation for the sonic-swss repository is broken down into four stages:

1. Instrumentation
2. Unit testing
3. Virtual switch testing
4. Coverage analysis

## 1. Instrumentation
This step is common to all code coverage implementations using `gcov`. The `swss` binary is compiled using the flags `-fprofile-arcs` and `-ftest-coverage`. The Makefile for sonic-swss will add these flags  based on the environment variable `ENABLE_GCOV` (implemented in `debian/rules::configure_opts` and `configure.ac::107`). These flags have two effects:

1. Generate a `*.gcno` file for each source file. This file contains information about which lines of code in the source file are reachable.
2. After running the compiled executable, create a `*.gcda` file for each source file that was run. This file contains information about which lines of code in the source file were actually executed at runtime.

## 2. Unit Testing
The sonic-swss repository includes several C++ unit tests, located in `tests/mock_tests`. These tests are automatically run during compilation. After execution, `*.gcda` files are generated for each source file used during the tests, located in the same directory as the original source file. Since there are still additional tests to be run, these files need to be saved so that they can be included in the coverage analysis after all tests have run. 

In `.azure_pipelines/build_template.yml`, after compiling the swss binary the `.gcno` and `.gcda` files that were generated are saved in the artifact staging directory of the AZP agent running the compilation under `$(Build.ArtifactStagingDirectory)/gcda_archives/mock_tests`.

## 3. Virtual Switch Testing
The majority of tests in sonic-swss run in a Docker Virtual Switch (DVS), essentially a Docker container which can mimic parts of an actual SONiC switch. These tests are run in batches, with 20 modules per batch. The logical flow for collecting coverage data for each batch is defined in `.azure_pipelines/test-docker-sonic-vs-template.yml`:

1. Create a new DVS if necessary. (This is only needed for the first module in each batch, and for some specific modules which require it)
    - Implemented in `tests/conftest.py::manage_dvs`
2. Run the next test module in the batch
3. Send a `SIGKILL` to all processes inside the DVS to generated `.gcda` files for the module. If `.gcda` files already exist, new coverage information will be appended to the existing files.
    - Implemented in `tests/conftest.py::dvs`
5. Cleanup/restart the DVS to prepare for the next module.
    - Implemented in `tests/conftest.py::manage_dvs`
6. Repeat steps 1-5 until all modules in the batch are finished.
7. For each DVS created during this batch, copy all `.gcda` archives in `/tmp/gcov/` to `$(Build.ArtifactStagingDirectory)/gcda_archives/`, creating a separate directory for each container.
    - Implemented in `tests/gcov_support.sh::collect_container_gcda`
8. Delete all DVS containers and begin the next batch of tests.

After all test modules have run, `$(Build.ArtifactStagingDirectory)/gcda_archives/` is saved as an artifact so it can be used in the next step.

## 4. Coverage Analysis
Once all `.gcda` archives are collected, `tests/gcov_support.sh` is used to generate coverage information in two steps:

1. For each container under `$(Build.ArtifactStagingDirectory)/gcda_archives/`, generate one tracefile. This tracefile (`*.info`) is an intermediary format which contains coverage information for one test module. (In this step, we include the mock test coverage information by treating `$(Build.ArtifactStagingDirectory)/gcda_archives/mock_tests/` as another container-specific directory)
    - Implemented in `tests/gcov_support.sh::generate_tracefiles`
2. Combine all tracefiles into a single comprehensive tracefile containing coverage information for the entire sonic-swss repository. Then transform this comprehensive tracefile into a Cobertura-style `coverage.xml` file which can be ingested by the AZP code coverage tool.