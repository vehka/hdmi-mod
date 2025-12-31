#!/bin/bash

# Build script for hdmi-mod

set -e  # Exit on error

# Build

mkdir -p build-hdmi
cd build-hdmi
echo "Running cmake..."

cmake ..

echo "Building..."
make install-dust install-zip

echo ""
echo "Build complete! Output in build-hdmi/"
echo "To install on norns: cp -r build-hdmi/hdmi-mod /home/we/dust/code/"
echo "Or use the zip: build-hdmi/hdmi-mod.zip"
