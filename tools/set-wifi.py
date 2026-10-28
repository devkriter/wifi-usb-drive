#!/usr/bin/env python3
"""Give the WiFi drive its network credentials over HTTP (same as the Settings page).

The drive saves them and restarts; if it cannot join, it starts its setup network
WiFi-Drive-XXXX (see the screen) where this script can be used again at http://192.168.4.1.

Usage:
  python set-wifi.py --url http://192.168.4.1 --ssid "My Network" --password "secret"
  python set-wifi.py --url http://wifidrive.local --from-secrets ../secrets.yaml   # wifi_ssid/wifi_password keys
"""
import argparse
import json
import re
import time
import urllib.parse
import urllib.request


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--ssid")
    ap.add_argument("--password", default="")
    ap.add_argument("--from-secrets", help="ESPHome-style secrets.yaml with wifi_ssid / wifi_password")
    ap.add_argument("--mode", default="sta", choices=["sta", "ap", "apsta"])
    ap.add_argument("--ap-ssid", default="")
    ap.add_argument("--ap-password", default="")
    args = ap.parse_args()

    ssid, password = args.ssid, args.password
    if args.from_secrets:
        for line in open(args.from_secrets, encoding="utf-8"):
            m = re.match(r"^(wifi_ssid|wifi_password):\s*(.*?)\s*$", line)
            if m:
                v = m.group(2)
                if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                    v = v[1:-1]
                if m.group(1) == "wifi_ssid":
                    ssid = v
                else:
                    password = v
    if args.mode != "ap" and not ssid:
        ap.error("an SSID is required (--ssid or --from-secrets)")

    form = urllib.parse.urlencode({
        "wifimode": args.mode,
        "stassid": ssid or "",
        "stapassword": password or "",
        "apssid": args.ap_ssid,
        "appassword": args.ap_password,
    }).encode()
    req = urllib.request.Request(args.url + "/settings.html", data=form,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=20) as r:
        print("HTTP", r.status, "- saved; the drive is restarting")

    print("waiting for it to come back on the network...")
    t0 = time.time()
    while time.time() - t0 < 90:
        time.sleep(3)
        try:
            with urllib.request.urlopen(args.url + "/api/status", timeout=3) as r:
                s = json.loads(r.read().decode())
            if s.get("uptime_s", 999) < 80:
                print("back: ip {} sta_ssid {!r} rssi {} fallback_ap {}".format(
                    s.get("ip"), s.get("sta_ssid"), s.get("rssi"), s.get("fallback_ap")))
                return 0
        except Exception:
            pass
    print("not reachable at {} within 90 s; if it could not join, look for WiFi-Drive-XXXX on the screen".format(args.url))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
