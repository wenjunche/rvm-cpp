#!/bin/bash
# Build script for rvm-cpp

echo "Installing dependencies (if needed)..."
# You may need to run: sudo apt-get install libcurl4-openssl-dev libzip-dev nlohmann-json3-dev

echo "Compiling rvm-cpp..."
g++ -std=c++17 -o rvm-cpp main.cpp \
    -lcurl \
    -lzip \
    -lpthread \
    -Wall \
    -O2

if [ $? -eq 0 ]; then
    echo "✓ Compilation successful!"
    echo ""
    echo "Run with:"
    echo "./rvm-cpp --config=<URL> --runtime-dir=<directory>"
    echo ""
    echo "Example:"
    echo "./rvm-cpp --config=https://cdn.openfin.co/release/apps/openfin/processmanager/app.json --runtime-dir=/home/wenjun/OpenFin/Runtime"
else
    echo "✗ Compilation failed"
    echo ""
    echo "Make sure you have the required dependencies installed:"
    echo "  sudo apt-get install libcurl4-openssl-dev libzip-dev nlohmann-json3-dev build-essential"
    exit 1
fi
