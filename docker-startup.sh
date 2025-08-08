#!/bin/bash

# Docker startup script for DeepStream + OpenVPN3 + MediaMTX (chỉ MediaMTX)

echo "Starting Docker container services..."

# Copy MediaMTX config
cp /app/mediamtx.yml /etc/mediamtx/mediamtx.yml

# Function to start OpenVPN3 if config file exists
start_openvpn() {
    if [ -f "/etc/openvpn3/client.ovpn" ]; then
        echo "Starting OpenVPN3..."
        openvpn3 config-import --config /etc/openvpn3/client.ovpn --name vpn-client
        openvpn3 session-start --config vpn-client &
        sleep 5
        echo "OpenVPN3 started"
    else
        echo "No OpenVPN3 config found at /etc/openvpn3/client.ovpn"
        echo "Please mount your .ovpn file to /etc/openvpn3/client.ovpn"
    fi
}

# Function to start MediaMTX (thay thế nginx)
start_mediamtx() {
    echo "Starting MediaMTX Media Server..."
    mediamtx /etc/mediamtx/mediamtx.yml &
    sleep 3
    echo "MediaMTX started:"
    echo "  - RTSP: port 8554"
    echo "  - RTMP: port 1935"
    echo "  - HLS: port 8888"
    echo "  - WebRTC: port 8889"
    echo "  - API: port 9997"
    echo "  - Metrics: port 9998"
}

# Function to start DeepStream
start_deepstream() {
    echo "Starting DeepStream application..."
    cd /app
    ./bin/deepstream-app -c samples/configs/deepstream_app.txt &
    echo "DeepStream started with RTSP output on localhost:8554/ds-test"
}

# Check command line arguments
case "$1" in
    "vpn")
        start_openvpn
        ;;
    "mediamtx")
        start_mediamtx
        ;;
    "deepstream")
        start_deepstream
        ;;
    "all")
        start_openvpn
        start_mediamtx
        start_deepstream
        ;;
    *)
        echo "Usage: $0 {vpn|mediamtx|deepstream|all}"
        echo "  vpn      - Start OpenVPN3 client"
        echo "  mediamtx - Start MediaMTX server (RTSP/RTMP/HLS/WebRTC)"
        echo "  deepstream - Start DeepStream application"
        echo "  all      - Start all services"
        exit 1
        ;;
esac

# Keep container running
echo "Services started. Press Ctrl+C to stop..."
tail -f /dev/null
