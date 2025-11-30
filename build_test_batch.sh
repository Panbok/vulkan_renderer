#!/bin/bash

# Build and run tests 50 times to check for intermittent failures

set -e

echo "=== Building project ==="
cmake --build build

echo ""
echo "=== Running tests 50 times ==="

passed=0
failed=0
tmpfile=$(mktemp)

for i in {1..50}; do
    if ./build/tests/vulkan_renderer_tester > "$tmpfile" 2>&1; then
        ((passed++))
        echo "Run $i: PASSED"
    else
        exit_code=$?
        ((failed++))
        echo "Run $i: FAILED (exit code: $exit_code)"
        echo "--- Output ---"
        tail -30 "$tmpfile"
        echo "--------------"
        echo ""
    fi
done

rm -f "$tmpfile"

echo ""
echo "=== Summary ==="
echo "Passed: $passed/50"
echo "Failed: $failed/50"

if [ $failed -gt 0 ]; then
    exit 1
fi

