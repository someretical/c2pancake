#!/bin/bash
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJ_DIR/build"
C2PANCAKE="$BUILD_DIR/c2pancake"
CAKE="$PROJ_DIR/cake-arm8-64/cake"
CAKE_FLAGS="--target=arm8 --pancake --main_return=true"
TESTS_DIR="$PROJ_DIR/tests"
OUT_DIR="$PROJ_DIR/test_output"

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

SKIP_BUILD=false
for arg in "$@"; do
    [ "$arg" = "--no-build" ] && SKIP_BUILD=true
done

if [ "$SKIP_BUILD" = false ]; then
    echo "stage 1: build"
    (
        cd "$BUILD_DIR"
        cmake -DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm .. > /dev/null 2>&1 || true
        make 2>&1
    ) > "$OUT_DIR/build.log" 2>&1
    if [ $? -eq 0 ]; then
        pass "c2pancake builds"
    else
        fail "c2pancake build"
        cat "$OUT_DIR/build.log"
        exit 1
    fi
fi

echo ""
echo "stage 2: transpile + compile"

TRANSPILE_FILES=(loop.c test.c struct.c hello.c)

for cfile in "${TRANSPILE_FILES[@]}"; do
    filepath="$TESTS_DIR/$cfile"
    if [ ! -f "$filepath" ]; then
        fail "$cfile not found"
        continue
    fi

    base="${cfile%.c}"
    pancake_out="$OUT_DIR/${base}.pancake"
    asm_out="$OUT_DIR/${base}.S"

    echo ""
    echo "  $cfile"

    if "$C2PANCAKE" "$filepath" > "$pancake_out" 2>"$OUT_DIR/${base}.transpile.err"; then
        pass "transpile $cfile"
    else
        fail "transpile $cfile"
        cat "$OUT_DIR/${base}.transpile.err"
        continue
    fi

    cake_stderr="$OUT_DIR/${base}.cake.err"
    if cat "$pancake_out" | cpp -P 2>/dev/null | "$CAKE" $CAKE_FLAGS > "$asm_out" 2>"$cake_stderr"; then
        warnings=$(grep "WARNING" "$cake_stderr" 2>/dev/null || true)
        if [ -n "$warnings" ]; then
            pass "cake compiles $base.pancake (with warnings)"
        else
            pass "cake compiles $base.pancake"
        fi
    else
        fail "cake compile $base.pancake"
        cat "$cake_stderr" 2>/dev/null | head -10
    fi
done

echo ""
echo "stage 3: full pipeline (verify.c)"

VERIFY_DIR="$OUT_DIR/verify_build"
if python3 "$PROJ_DIR/c2pancake.py" "$TESTS_DIR/verify.c" \
    --ffi "$TESTS_DIR/verify_ffi.c" \
    -o "$VERIFY_DIR" \
    --compile --run > "$OUT_DIR/verify.log" 2>&1; then
    if grep -q "PASS" "$OUT_DIR/verify.log"; then
        pass "verify.c full pipeline (all feature tests)"
    else
        fail "verify.c (unexpected output)"
        cat "$OUT_DIR/verify.log"
    fi
else
    fail "verify.c full pipeline"
    cat "$OUT_DIR/verify.log"
fi

echo ""
echo "stage 4: FFI hello world (hello.c)"

HELLO_DIR="$OUT_DIR/hello_build"
if python3 "$PROJ_DIR/c2pancake.py" "$TESTS_DIR/hello.c" \
    --ffi "$TESTS_DIR/hello_ffi.c" \
    -o "$HELLO_DIR" \
    --compile --run > "$OUT_DIR/hello.log" 2>&1; then
    if grep -q "Hello from Pancake" "$OUT_DIR/hello.log"; then
        pass "hello.c FFI pipeline"
    else
        fail "hello.c (no FFI output)"
        cat "$OUT_DIR/hello.log"
    fi
else
    fail "hello.c FFI pipeline"
    cat "$OUT_DIR/hello.log"
fi

echo ""
echo "results: $PASS passed, $FAIL failed"

[ $FAIL -eq 0 ]
