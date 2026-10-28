#!/usr/bin/env python3
"""WiFi throughput test against the usb_msc_wireless_disk HTTP server.

Uploads random data with POST /upload/<name> (raw body), optionally downloads it back
with GET /<name> and verifies the SHA-256, then deletes it with POST /delete/<name>.

Usage:
  python wifi-upload-test.py --url http://wifidrive.local --size-mb 50 --count 2 --download
"""
import argparse
import hashlib
import http.client
import os
import time
import urllib.parse


def connect(url):
    u = urllib.parse.urlparse(url)
    return u.hostname, u.port or 80


def upload(host, port, name, data, chunk):
    conn = http.client.HTTPConnection(host, port, timeout=600)
    conn.putrequest("POST", "/upload/" + urllib.parse.quote(name))
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(len(data)))
    conn.endheaders()
    t0 = time.perf_counter()
    try:
        for i in range(0, len(data), chunk):
            conn.send(data[i:i + chunk])
    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
        pass  # the server may answer (e.g. 503 busy) and close before the body is through
    try:
        resp = conn.getresponse()
        status, body = resp.status, resp.read()
    except Exception as e:
        status, body = 0, str(e).encode()
    t1 = time.perf_counter()
    conn.close()
    return status, body[:120].decode("utf-8", "replace"), t1 - t0


def download(host, port, name):
    conn = http.client.HTTPConnection(host, port, timeout=600)
    conn.request("GET", "/" + urllib.parse.quote(name))
    resp = conn.getresponse()
    h = hashlib.sha256()
    n = 0
    t0 = time.perf_counter()
    while True:
        block = resp.read(65536)
        if not block:
            break
        h.update(block)
        n += len(block)
    t1 = time.perf_counter()
    conn.close()
    return resp.status, n, h.hexdigest(), t1 - t0


def delete(host, port, name):
    conn = http.client.HTTPConnection(host, port, timeout=60)
    conn.request("POST", "/delete/" + urllib.parse.quote(name), body=b"")
    resp = conn.getresponse()
    resp.read()
    conn.close()
    return resp.status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True, help="e.g. http://wifidrive.local")
    ap.add_argument("--size-mb", type=int, default=50)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--chunk-kb", type=int, default=64)
    ap.add_argument("--download", action="store_true", help="read the file back and verify its hash")
    ap.add_argument("--keep", action="store_true", help="leave the uploaded files on the card")
    args = ap.parse_args()

    host, port = connect(args.url)
    data = os.urandom(args.size_mb * 1024 * 1024)
    want = hashlib.sha256(data).hexdigest()
    mb = len(data) / (1024 * 1024)
    print("sha256 of upload data: " + want)
    ups, downs = [], []
    for i in range(args.count):
        name = "wifitest_{}_{}.bin".format(int(time.time()), i)
        status, body, dt = upload(host, port, name, data, args.chunk_kb * 1024)
        rate = mb / dt
        ups.append(rate)
        print("upload   {:>3} {:6.1f} MB in {:6.2f} s = {:5.2f} MB/s  HTTP {} {}".format(i, mb, dt, rate, status, body.strip()))
        if args.download:
            # the server finishes writing in the background; give it a moment and retry
            for attempt in range(10):
                st, n, got, dt2 = download(host, port, name)
                if st == 200 and n == len(data):
                    break
                time.sleep(1.0)
            rate2 = (n / (1024 * 1024)) / dt2 if dt2 else 0
            downs.append(rate2)
            print("download {:>3} {:6.1f} MB in {:6.2f} s = {:5.2f} MB/s  HTTP {}  hash {}".format(
                i, n / (1024 * 1024), dt2, rate2, st, "OK" if got == want else "MISMATCH"))
        if not args.keep:
            print("delete   {:>3} HTTP {}".format(i, delete(host, port, name)))
    if ups:
        print("upload   avg {:5.2f} MB/s  max {:5.2f} MB/s".format(sum(ups) / len(ups), max(ups)))
    if downs:
        print("download avg {:5.2f} MB/s  max {:5.2f} MB/s".format(sum(downs) / len(downs), max(downs)))


if __name__ == "__main__":
    main()
