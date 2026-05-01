#!/bin/sh

PORT=18088

echo "Building chatd and test client..."
make all
make test_client

echo "Starting server on port $PORT..."
./chatd "$PORT" > test/server_output.txt 2>&1 &
SERVER_PID=$!

sleep 1

echo "Running Bob client..."
{
    echo "WHO #all"
    echo "SET Smiling politely"
    echo "WHO Bob"
    echo "QUIT"
} | ./test/test_client 127.0.0.1 "$PORT" Bob > test/bob_output.txt 2>&1

echo "Bob output:"
cat test/bob_output.txt

echo "Stopping server..."
kill "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null

echo "Done."