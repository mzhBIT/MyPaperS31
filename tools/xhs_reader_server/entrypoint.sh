#!/bin/sh
set -eu

if [ "${XHS_HEADLESS:-0}" != 1 ]; then
    : "${XHS_VNC_PASSWORD:?Set XHS_VNC_PASSWORD for the local browser console}"
    export DISPLAY=:99
    Xvfb "$DISPLAY" -screen 0 1280x900x24 -nolisten tcp &
    attempt=0
    until xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; do
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 50 ]; then
            echo 'Xvfb did not start' >&2
            exit 1
        fi
        sleep 0.1
    done
    password_file=$(mktemp)
    x11vnc -storepasswd "$XHS_VNC_PASSWORD" "$password_file" >/dev/null
    x11vnc -display "$DISPLAY" -localhost -forever -shared -rfbauth "$password_file" -rfbport 5900 &
    websockify --web /usr/share/novnc 0.0.0.0:6080 127.0.0.1:5900 &
fi

exec /opt/reader-venv/bin/python -m tools.xhs_reader_server
