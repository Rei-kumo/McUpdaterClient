#!/bin/bash

set -e

echo "Installing dependencies for Minecraft Updater..."

if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    # Linux
    echo "Installing on Linux..."
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        cmake \
        pkg-config \
        libcurl4-openssl-dev \
        libssl-dev \
        libjsoncpp-dev \
        libzip-dev

elif [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    echo "Installing on macOS..."
    if ! command -v brew &> /dev/null; then
        echo "Homebrew not found. Please install Homebrew first."
        exit 1
    fi
    brew update
    brew install cmake pkg-config curl openssl jsoncpp libzip

elif [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
    # Windows
    echo "Please install dependencies using vcpkg on Windows:"
    echo "vcpkg install curl openssl jsoncpp libzip"
else
    echo "Unsupported OS: $OSTYPE"
    exit 1
fi

echo "Dependencies installed successfully!"