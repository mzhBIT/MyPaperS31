#!/usr/bin/env python3
"""Send to the temporary S31 raw TCP sink (not an iperf protocol client)."""
import argparse
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="Device IP; hotspot default: 192.168.4.1")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--seconds", type=float, default=30)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    payload = bytes(range(256)) * 256
    total = previous_bytes = 0
    stage = "connect"
    started = time.monotonic()
    print(f"Connecting to {args.host}:{args.port} (timeout=5s)...", flush=True)
    try:
        with socket.create_connection((args.host, args.port), timeout=5) as connection:
            stage = "send"
            connection.settimeout(5)
            begin = previous = time.monotonic()
            print(f"Connected to {args.host}:{args.port}; sending for {args.seconds:g}s", flush=True)
            while time.monotonic() - begin < args.seconds:
                # Count actual sends; partial sends are normal on a slow link.
                sent = connection.send(payload)
                if sent == 0:
                    raise OSError("Device closed connection")
                total += sent
                now = time.monotonic()
                if now - previous >= 2:
                    print(f"sent={total} avg={total / (now-begin) / 1024:.1f} KiB/s "
                          f"inst={(total-previous_bytes) / (now-previous) / 1024:.1f} KiB/s", flush=True)
                    previous, previous_bytes = now, total
            # Wait for device EOF so queued PC data is drained before timing ends.
            stage = "drain"
            print(f"Draining queued data: sent={total} (timeout=15s)...", flush=True)
            connection.shutdown(socket.SHUT_WR)
            connection.settimeout(15)
            while connection.recv(1024):
                pass
            elapsed = time.monotonic() - begin
            print(f"Complete: sent={total} elapsed={elapsed:.2f}s avg={total/elapsed/1024:.1f} KiB/s")
            print("Compare with the device TCP_RX_TEST complete log (received bytes and throughput).")
    except OSError as error:
        hint = {
            "connect": "Check the device IP, PC network, and device log: TCP_RX_TEST listening port=5001.",
            "send": "Connected, but sending stalled. Check device TCP_RX_TEST progress/error logs.",
            "drain": "Sending finished, but queued data did not drain or the device did not close. Check the device received-byte count.",
        }[stage]
        parser.exit(1, f"TCP test failed: stage={stage} sent={total} "
                      f"elapsed={time.monotonic()-started:.2f}s error={error}\n{hint}\n")


if __name__ == "__main__":
    main()
