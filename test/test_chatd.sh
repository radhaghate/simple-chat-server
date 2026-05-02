#!/bin/sh

PORT=18088
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

echo "Building chatd and test client..."
make all
make test_client

mkdir -p test
rm -f test/server_output.txt test/test_results.txt

echo "Starting server on port $PORT..."
./chatd "$PORT" > test/server_output.txt 2>&1 &
SERVER_PID=$!

sleep 1

echo "Running protocol tests..."
python3 - "$PORT" > test/test_results.txt <<'PY'
import socket
import sys
import time

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
TIMEOUT = 2.0

passed = 0
failed = 0


def report(ok, name, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"PASS: {name}")
    else:
        failed += 1
        print(f"FAIL: {name}")
        if detail:
            print(f"      {detail}")


def connect_client():
    s = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    s.settimeout(TIMEOUT)
    return s


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed while reading")
        data += chunk
    return data


def recv_frame(sock):
    header = b""
    pipes = 0
    while pipes < 3:
        ch = recv_exact(sock, 1)
        header += ch
        if ch == b"|":
            pipes += 1
        if len(header) > 64:
            raise ValueError(f"header too long: {header!r}")

    parts = header.decode("ascii").split("|")
    version, code, length_text = parts[0], parts[1], parts[2]
    length = int(length_text)
    body = recv_exact(sock, length).decode("ascii")
    return version, code, length, body


def send_frame(sock, code, fields):
    body = "|".join(fields) + "|"
    frame = f"1|{code}|{len(body)}|{body}"
    sock.sendall(frame.encode("ascii"))


def send_raw(sock, raw):
    sock.sendall(raw.encode("ascii"))


def expect_msg(sock, from_field=None, to_field=None, text_contains=None):
    version, code, length, body = recv_frame(sock)
    fields = body[:-1].split("|", 2) if body.endswith("|") else body.split("|", 2)
    ok = version == "1" and code == "MSG" and len(fields) == 3
    if from_field is not None:
        ok = ok and fields[0] == from_field
    if to_field is not None:
        ok = ok and fields[1] == to_field
    if text_contains is not None:
        ok = ok and text_contains in fields[2]
    return ok, (version, code, body)


def expect_err(sock, err_code):
    version, code, length, body = recv_frame(sock)
    fields = body[:-1].split("|", 1) if body.endswith("|") else body.split("|", 1)
    ok = version == "1" and code == "ERR" and len(fields) >= 1 and fields[0] == str(err_code)
    return ok, (version, code, body)


def login(name):
    s = connect_client()
    send_frame(s, "NAM", [name])
    ok, detail = expect_msg(s, "#all", name, "Welcome")
    report(ok, f"NAM accepts valid name {name}", str(detail))
    return s


try:
    bob = login("Bob")

    send_frame(bob, "WHO", ["#all"])
    ok, detail = expect_msg(bob, "#all", "Bob", "Bob")
    report(ok, "WHO #all includes connected user", str(detail))

    send_frame(bob, "SET", ["Smiling politely"])
    ok, detail = expect_msg(bob, "#all", "#all", "Bob is now \"Smiling politely\"")
    report(ok, "SET broadcasts non-empty status", str(detail))

    send_frame(bob, "WHO", ["Bob"])
    ok, detail = expect_msg(bob, "#all", "Bob", "Bob: Smiling politely")
    report(ok, "WHO specific user reports status", str(detail))

    alice = login("Alice")

    send_frame(bob, "MSG", ["", "#all", "Hello everyone"])
    ok_bob, detail_bob = expect_msg(bob, "Bob", "#all", "Hello everyone")
    ok_alice, detail_alice = expect_msg(alice, "Bob", "#all", "Hello everyone")
    report(ok_bob and ok_alice, "MSG #all broadcasts to multiple clients", f"Bob saw {detail_bob}; Alice saw {detail_alice}")

    send_frame(bob, "MSG", ["", "Alice", "Private hello"])
    ok_alice, detail_alice = expect_msg(alice, "Bob", "Alice", "Private hello")
    bob.settimeout(0.4)
    try:
        unexpected = recv_frame(bob)
        ok_bob = False
        detail_bob = f"Bob unexpectedly received {unexpected}"
    except socket.timeout:
        ok_bob = True
        detail_bob = "Bob received no private echo"
    finally:
        bob.settimeout(TIMEOUT)
    report(ok_alice and ok_bob, "Private MSG goes only to recipient", f"Alice saw {detail_alice}; {detail_bob}")

    dup = connect_client()
    send_frame(dup, "NAM", ["Bob"])
    ok, detail = expect_err(dup, 1)
    report(ok, "Duplicate name returns ERR 1", str(detail))
    dup.close()

    send_frame(bob, "MSG", ["", "Nobody", "Are you there?"])
    ok, detail = expect_err(bob, 2)
    report(ok, "Unknown recipient returns ERR 2", str(detail))

    bad_name = connect_client()
    send_frame(bad_name, "NAM", ["Bad!"])
    ok, detail = expect_err(bad_name, 3)
    report(ok, "Illegal name character returns ERR 3", str(detail))
    bad_name.close()

    send_frame(bob, "MSG", ["", "#all", "x" * 81])
    ok, detail = expect_err(bob, 4)
    report(ok, "Too-long message returns ERR 4", str(detail))

    malformed = connect_client()
    send_raw(malformed, "2|NAM|4|Eve|")
    ok, detail = expect_err(malformed, 0)
    report(ok, "Bad protocol version returns fatal ERR 0", str(detail))
    try:
        data = malformed.recv(1)
        report(data == b"", "Fatal ERR 0 closes connection", f"Received after ERR 0: {data!r}")
    except ConnectionResetError:
        report(True, "Fatal ERR 0 closes connection", "Connection reset by server")
    except socket.timeout:
        report(False, "Fatal ERR 0 closes connection", "Socket stayed open")
    malformed.close()

    alice.close()
    bob.close()

except Exception as exc:
    report(False, "test harness exception", repr(exc))

print(f"\nSummary: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
PY
TEST_STATUS=$?

cat test/test_results.txt

echo "Stopping server..."
cleanup
SERVER_PID=""

if [ "$TEST_STATUS" -ne 0 ]; then
    echo "Some tests failed. See test/test_results.txt and test/server_output.txt."
    exit "$TEST_STATUS"
fi

echo "All tests passed."
