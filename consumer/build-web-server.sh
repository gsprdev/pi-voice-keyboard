#!/usr/bin/env bash
# Build the web server for Raspberry Pi

set -e

echo "Building web server..."

mkdir -p build
rm -f build/web-server

# Build for linux/arm64 for use on Raspberry Pi Zero 2 W
GOOS=linux GOARCH=arm64 go build -o build/web-server web-server.go

echo "Build complete: ./web-server"
echo ""
echo "To generate a new API key:"
echo "  ./web-server -generate-key"
echo ""
echo "To run the server:"
echo "  ./web-server"
