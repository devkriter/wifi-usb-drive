#!/usr/bin/env python3
"""Soak test for the WiFi drive: periodic WiFi upload/download/delete cycles, with an
occasional PC-side write/read on the mounted drive, while watching for reboots and errors.

Usage:
  python soak.py --url http://wifidrive.local --minutes 25 --interval 90 --size-mb 5 --pc-drive E --log soak.log
"""
import argparse
import hashlib
import http.client
import json
import os
import sys
import time
import urllib.parse
import urllib.request


def log(fh, msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    fh.write(line + "\n")
    fh.flush()


def status(url):
    with urllib.request.urlopen(url + "/api/status", timeout=5) as r:
        return json.loads(r.read().decode())


def upload(host, port, name, data):
    conn = http.client.HTTPConnection(host, port, timeout=300)
    conn.putrequest("POST", "/upload/" + urllib.parse.quote(name))
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(len(data)))
    conn.endheaders()
    t0 = time.perf_counter()
    try:
        for i in range(0, len(data), 65536):
            conn.send(data[i:i + 65536])
    except OSError:
        pass
    try:
        resp = conn.getresponse()
        st, body = resp.status, resp.read()[:80].decode("utf-8", "replace")
    except Exception as e:
        st, body = 0, str(e)
    conn.close()
    return st, body, time.perf_counter() - t0


def download_hash(host, port, name):
    conn = http.client.HTTPConnection(host, port, timeout=300)
    conn.request("GET", "/" + urllib.parse.quote(name))
    resp = conn.getresponse()
    h = hashlib.sha256()
    n = 0
    t0 = time.perf_counter()
    while True:
        b = resp.read(65536)
        if not b:
            break
        h.update(b)
        n += len(b)
    conn.close()
    return resp.status, n, h.hexdigest(), time.perf_counter() - t0


def delete(host, port, name):
    conn = http.client.HTTPConnection(host, port, timeout=60)
    conn.request("POST", "/delete/" + urllib.parse.quote(name), body=b"")
    r = conn.getresponse()
    r.read()
    conn.close()
    return r.status


def pc_roundtrip(drive, mb, fh):
    """Write a file on the mounted drive from the PC and read it back; returns (ok, note)."""
    path = "{}:\\soak_pc_{}.bin".format(drive, int(time.time()))
    data = os.urandom(mb * 1024 * 1024)
    want = hashlib.sha256(data).hexdigest()
    deadline = time.time() + 45
    while time.time() < deadline:
        try:
            with open(path, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            break
        except OSError as e:
            time.sleep(2)
    else:
        return False, "pc write never succeeded (drive not back?)", None
    with open(path, "rb") as f:
        got = hashlib.sha256(f.read()).hexdigest()
    return got == want, "pc write+read " + ("ok" if got == want else "HASH MISMATCH"), os.path.basename(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--minutes", type=float, default=25)
    ap.add_argument("--interval", type=float, default=90)
    ap.add_argument("--size-mb", type=int, default=5)
    ap.add_argument("--pc-drive", default="")
    ap.add_argument("--log", default="soak.log")
    args = ap.parse_args()
    u = urllib.parse.urlparse(args.url)
    host, port = u.hostname, u.port or 80
    fh = open(args.log, "a", encoding="utf-8")
    log(fh, "soak start: {} min, every {} s, {} MB uploads, pc drive {!r}".format(args.minutes, args.interval, args.size_mb, args.pc_drive))

    t_end = time.time() + args.minutes * 60
    it = 0
    fails = 0
    reboots = 0
    last_uptime = None
    heap_min = None
    heap_max = None
    rates = []
    while time.time() < t_end:
        it += 1
        try:
            s = status(args.url)
        except Exception as e:
            log(fh, "#{} status FAILED: {}".format(it, e))
            fails += 1
            time.sleep(args.interval)
            continue
        if last_uptime is not None and s["uptime_s"] < last_uptime:
            reboots += 1
            log(fh, "#{} REBOOT detected (uptime {} < {})".format(it, s["uptime_s"], last_uptime))
        last_uptime = s["uptime_s"]
        heap_min = s["free_heap"] if heap_min is None else min(heap_min, s["free_heap"])
        heap_max = s["free_heap"] if heap_max is None else max(heap_max, s["free_heap"])

        data = os.urandom(args.size_mb * 1024 * 1024)
        want = hashlib.sha256(data).hexdigest()
        name = "soak_{}_{}.bin".format(int(time.time()), it)
        st, body, dt = upload(host, port, name, data)
        note = "upload HTTP {} {:.2f} MB/s".format(st, args.size_mb / dt if dt else 0)
        ok = (st == 200)
        if ok:
            rates.append(args.size_mb / dt)
            st2, n, got, dt2 = download_hash(host, port, name)
            ok = (st2 == 200 and got == want)
            note += "; download HTTP {} {:.2f} MB/s {}".format(st2, n / 1048576 / dt2 if dt2 else 0, "hash ok" if got == want else "HASH MISMATCH")
            note += "; delete HTTP {}".format(delete(host, port, name))
        if not ok:
            fails += 1
        pcnote = ""
        if args.pc_drive and it % 3 == 0:
            time.sleep(12)   # let the hand-back and the Windows re-mount happen first
            pok, pcnote, pname = pc_roundtrip(args.pc_drive, 3, fh)
            if pname:
                pcnote += "; wifi delete HTTP {}".format(delete(host, port, pname))
            if not pok:
                fails += 1
        log(fh, "#{} uptime {} s heap {} owner {} | {} {}".format(it, s["uptime_s"], s["free_heap"], s["owner"], note, pcnote))
        time.sleep(max(0, args.interval - dt))

    try:
        s = status(args.url)
        last = "final uptime {} s heap {} borrows {} releases {} forced {}".format(s["uptime_s"], s["free_heap"], s["borrows"], s["releases"], s["forced"])
    except Exception as e:
        last = "final status failed: {}".format(e)
    log(fh, "soak done: {} iterations, {} failures, {} reboots, heap min {} max {}, upload avg {:.2f} MB/s; {}".format(
        it, fails, reboots, heap_min, heap_max, (sum(rates) / len(rates)) if rates else 0, last))
    fh.close()
    return 0 if fails == 0 and reboots == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
