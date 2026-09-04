#!/bin/bash

# Build and run tests 50 times to check for intermittent failures

set -e

cd "$(dirname "$0")"

echo "=== Building project ==="
cmake --build build_test --target vulkan_renderer_tester --config Debug
./tools/pack_vkt_textures.sh

echo ""
echo "=== Running tests 50 times ==="

passed=0
failed=0
tmpfile=$(mktemp)
trap 'rm -f "$tmpfile"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export VKR_TEXTURE_VKT_STRICT=0
export VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK=1
export VKR_TEXTURE_VKT_ALLOW_LEGACY=1

for i in {1..50}; do
    if ./build_test/tests/vulkan_renderer_tester > "$tmpfile" 2>&1; then
        passed=$((passed + 1))
        echo "Run $i: PASSED"
    else
        exit_code=$?
        failed=$((failed + 1))
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

