#!/bin/bash

# Build list of test files dynamically and append additional test modules
all_tests=$(ls test_*.py 2>/dev/null | xargs)
all_tests="${all_tests} p4rt dash"

echo "🔍 Test modules to run:"
echo "$all_tests" | tr ' ' '\n'
echo

# Global start time
global_start_time=$(date '+%Y-%m-%d %H:%M:%S')
echo "🕓 Test run started at: $global_start_time"
echo

# Function to run a single test module (no internal timing)
run_test() {
    TEST_MODULE="$1"
    echo "🔧 Running: $TEST_MODULE"

    sudo pytest -v -s -x --tb=long "$TEST_MODULE"
    status=$?

    if [ $status -eq 0 ]; then
        echo "✅ [$TEST_MODULE] passed."
    else
        echo "❌ [$TEST_MODULE] failed with exit code $status"
    fi

    echo "---------------------------------------------"
}

# Export the function and env vars for xargs
export -f run_test
export PATH

# Run tests in parallel (up to 8 at a time)
echo "$all_tests" | tr ' ' '\n' | xargs -P 8 -I TEST_MODULE bash -c 'run_test TEST_MODULE'

# Global end time
global_end_time=$(date '+%Y-%m-%d %H:%M:%S')
echo
echo "🕓 Test run ended at: $global_end_time"

