#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

VALKEY_DIR="$REPO_ROOT/build/valkey"
LUAU_MODULE="$REPO_ROOT/build/libvalkeyluau.so"

if [ ! -f "$VALKEY_DIR/runtest" ]; then
    echo "Valkey test server not found at $VALKEY_DIR"
    echo "Run: ./build.sh --with-tests"
    exit 1
fi

if [ ! -f "$LUAU_MODULE" ]; then
    echo "Module not found at $LUAU_MODULE"
    echo "Run: ./build.sh"
    exit 1
fi

echo "Running tests"
echo "Valkey:  $VALKEY_DIR"
echo "Module:  $LUAU_MODULE"
echo ""

EXTRA_SKIP_ARGS=(
    "--skiptest" "/Test scripting debug"
    "--skiptest" "/Dynamic reset of lua engine with insecure API config change"
    "--skiptest" "Try trick readonly table on valkey table"
    "--skiptest" "Try trick readonly table on json table"
    "--skiptest" "Try trick readonly table on cmsgpack table"
    "--skiptest" "Try trick readonly table on bit table"
    "--skiptest" "Try trick readonly table on basic types metatable"
    "--skiptest" "Try trick global protection 1"
    "--skiptest" "Try trick global protection 2"
    "--skiptest" "LIBRARIES - malicious access test"
    "--skiptest" "Verify execution of prohibit dangerous Lua methods will fail"
    "--skiptest" "Binary code loading failed"
    "--skiptest" "LIBRARIES - register function inside a function"
    "--skiptest" "EVAL - Scripts support NULL byte"
    "--skiptest" "/Active Defrag eval scripts"
    "--skiptest" "CONFIG sanity"
)

cp "$SCRIPT_DIR"/*.tcl "$VALKEY_DIR/tests/unit/"

cd "$VALKEY_DIR"

./runtest --config loadmodule "$LUAU_MODULE" "${EXTRA_SKIP_ARGS[@]}" "$@"
