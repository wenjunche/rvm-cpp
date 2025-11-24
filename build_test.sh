#!/bin/bash

echo "Compiling test_socket..."
g++ -o test_socket test_socket.cpp -std=c++17 -I/usr/include

if [ $? -eq 0 ]; then
    echo "✓ Compilation successful!"
    echo ""
    echo "Run with:"
    echo "./test_socket"
    echo ""
    echo "Note: Make sure rvm-cpp is running first, then send test data with:"
    echo "echo 'test' | nc -U /tmp/test_socket"
else
    echo "✗ Compilation failed"
    exit 1
fi
