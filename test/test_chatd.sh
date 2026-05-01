#!/usr/bin/env bash
# test_chatd.sh - Automated test suite for chatd
# CS 214 Spring 2026 - Project IV
#
# Starts the server, runs several scripted test scenarios, and reports results.
#
# Usage: ./test_chatd.sh

PORT=9876
SERVER_PID=""
PASS=0
FAIL=0

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

start_server() {
    ./chatd $PORT &
    SERVER_PID=$!
    sleep 0.3   # give it time to bind
    echo "[TEST] Server started (PID $SERVER_PID)"
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
    fi
}

# send_cmd <name> <commands...>  and capture output
# Returns output in $CMD_OUT
run_client() {
    local name="$1"
    shift
    CMD_OUT=$(echo "$*" | ./test_client localhost $PORT "$name" 2>&1)
}

check() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "        Expected to find: '$expected'"
        echo "        Got: $actual"
        FAIL=$((FAIL + 1))
    fi
}

# ============================================================
echo ""
echo "=== Build ==="
make chatd test_client
if [ $? -ne 0 ]; then
    echo "FATAL: Build failed"
    exit 1
fi
echo "Build OK"

# ============================================================
echo ""
echo "=== Test 1: Basic NAM / Welcome ==="
start_server
OUT=$(echo "" | ./test_client localhost $PORT Alice 2>&1)
check "Alice gets welcome" "Welcome to the chat" "$OUT"
stop_server

# ============================================================
echo ""
echo "=== Test 2: Duplicate name rejected ==="
start_server
# Start Alice in background (stays connected)
(echo "" ; sleep 2) | ./test_client localhost $PORT Alice > /dev/null 2>&1 &
CLIENT1=$!
sleep 0.2
# Bob tries to use Alice's name
OUT=$(echo "" | ./test_client localhost $PORT Alice 2>&1)
check "Duplicate name gets ERR 1" "ERR" "$OUT"
kill $CLIENT1 2>/dev/null
stop_server

# ============================================================
echo ""
echo "=== Test 3: SET status ==="
start_server
OUT=$(printf "SET Happy today\nQUIT\n" | ./test_client localhost $PORT Bob 2>&1)
check "SET sends status MSG" "Bob is now" "$OUT"
stop_server

# ============================================================
echo ""
echo "=== Test 4: WHO single user ==="
start_server
# Start Alice in background
(printf "SET I was here first\n"; sleep 2) | ./test_client localhost $PORT Alice > /dev/null 2>&1 &
CLIENT1=$!
sleep 0.3
OUT=$(printf "WHO Alice\nQUIT\n" | ./test_client localhost $PORT Bob 2>&1)
check "WHO Alice returns status" "I was here first" "$OUT"
kill $CLIENT1 2>/dev/null
stop_server

# ============================================================
echo ""
echo "=== Test 5: WHO unknown user ==="
start_server
OUT=$(printf "WHO nobody\nQUIT\n" | ./test_client localhost $PORT Carol 2>&1)
check "WHO unknown user gets ERR 2" "ERR" "$OUT"
stop_server

# ============================================================
echo ""
echo "=== Test 6: WHO #all ==="
start_server
(printf "SET Smiling politely\n"; sleep 2) | ./test_client localhost $PORT Alice > /dev/null 2>&1 &
CLIENT1=$!
sleep 0.3
OUT=$(printf "WHO #all\nQUIT\n" | ./test_client localhost $PORT Bob 2>&1)
check "WHO #all lists Alice" "Alice" "$OUT"
check "WHO #all lists Bob"   "Bob"   "$OUT"
kill $CLIENT1 2>/dev/null
stop_server

# ============================================================
echo ""
echo "=== Test 7: Name with illegal chars rejected ==="
start_server
OUT=$(echo "" | ./test_client localhost $PORT "bad name!" 2>&1)
check "Illegal name chars get ERR 3" "ERR" "$OUT"
stop_server

# ============================================================
echo ""
echo "=== Test 8: Name too long rejected ==="
start_server
LONGNAME=$(python3 -c "print('a'*33)")
OUT=$(echo "" | ./test_client localhost $PORT "$LONGNAME" 2>&1)
check "Name too long gets ERR 4" "ERR" "$OUT"
stop_server

# ============================================================
echo ""
echo "============================================"
echo "Results: $PASS passed, $FAIL failed"
echo "============================================"
[ $FAIL -eq 0 ] && exit 0 || exit 1
