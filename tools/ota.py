#!/usr/bin/env python3
"""Over-the-air update of the WiFi drive: streams an app .bin to POST /ota and waits for the reboot.

Usage:
  python ota.py --url http://wifidrive.local --bin ../c-wifi-drive/build/wifi_drive.bin
"""
import argparse
import http.client
import json
import time
import urllib.parse
import urllib.request


def status(url):
    with urllib.request.urlopen(url + "/api/status", timeout=3) as r:
        return json.loads(r.read().decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--chunk-kb", type=int, default=64)
    args = ap.parse_args()

    u = urllib.parse.urlparse(args.url)
    host, port = u.hostname, u.port or 80
    try:
        before = status(args.url)
        print("before: partition {} elf {} uptime {} s".format(before.get("partition"), before.get("elf"), before.get("uptime_s")))
    except Exception as e:
        before = None
        print("before: status unavailable ({})".format(e))

    data = open(args.bin, "rb").read()
    print("sending {} bytes".format(len(data)))
    conn = http.client.HTTPConnection(host, port, timeout=300)
    conn.putrequest("POST", "/ota")
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(len(data)))
    conn.endheaders()
    t0 = time.perf_counter()
    chunk = args.chunk_kb * 1024
    for i in range(0, len(data), chunk):
        conn.send(data[i:i + chunk])
    resp = conn.getresponse()
    body = resp.read().decode("utf-8", "replace")
    dt = time.perf_counter() - t0
    conn.close()
    print("HTTP {} {} in {:.1f} s ({:.2f} MB/s)".format(resp.status, body.strip(), dt, len(data) / 1048576 / dt))
    if resp.status != 200:
        return 1

    print("waiting for the board to come back...")
    t1 = time.perf_counter()
    while time.perf_counter() - t1 < 90:
        time.sleep(2)
        try:
            after = status(args.url)
            if after.get("uptime_s", 999) < 60:
                print("back after {:.0f} s: partition {} elf {} uptime {} s".format(
                    time.perf_counter() - t1, after.get("partition"), after.get("elf"), after.get("uptime_s")))
                if before and after.get("elf") == before.get("elf"):
                    print("WARNING: same ELF hash as before; the old image is still running")
                    return 2
                return 0
        except Exception:
            pass
    print("board did not come back within 90 s")
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
