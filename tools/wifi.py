#!/usr/bin/env python3
"""Manage Pico Wi-Fi via /api/wifi. Stdlib only.

USB NCM is http://192.168.7.1  SoftAP is http://192.168.4.1
"""
from __future__ import annotations

import argparse
import getpass
import json
import sys
import time
import urllib.error
import urllib.request

DEFAULT_BASE = "http://192.168.7.1"


def request(base: str, path: str, method: str = "GET", body=None, timeout: float = 15):
    url = base.rstrip("/") + path
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            return resp.status, json.loads(raw.decode()) if raw else {}
    except urllib.error.HTTPError as e:
        raw = e.read() if e.fp else b""
        try:
            parsed = json.loads(raw.decode()) if raw else {}
        except json.JSONDecodeError:
            parsed = {"ok": False, "error": raw.decode("utf-8", "replace")}
        raise SystemExit(f"{method} {url} -> {e.code} {parsed}") from e
    except urllib.error.URLError as e:
        raise SystemExit(f"{method} {url} failed: {e.reason}") from e


def print_json(obj) -> None:
    print(json.dumps(obj, indent=2))


def cmd_status(base: str) -> int:
    _, data = request(base, "/api/wifi")
    print_json(data)
    return 0


def cmd_scan(base: str, wait: float) -> int:
    request(base, "/api/wifi/scan", method="POST")
    t0 = time.time()
    seen_active = False
    data: dict = {}
    while time.time() < t0 + wait:
        _, data = request(base, "/api/wifi/scan")
        if data.get("scanning"):
            seen_active = True
        elif seen_active or time.time() - t0 > 1.5:
            break
        time.sleep(0.4)
    aps = data.get("aps") or []
    if not aps:
        print("no APs (scan still running)" if data.get("scanning") else "no APs found")
        return 0
    print(f"{'SSID':32} {'RSSI':>5} {'CH':>3}  AUTH   KNOWN")
    for ap in sorted(aps, key=lambda a: a.get("rssi") or -999, reverse=True):
        print(
            f"{ap.get('ssid', ''):32} {ap.get('rssi', 0):5} {ap.get('chan', 0):3}  "
            f"{ap.get('auth', ''):6} {'yes' if ap.get('known') else ''}"
        )
    return 0


def cmd_known(base: str) -> int:
    _, data = request(base, "/api/wifi/networks")
    nets = data.get("networks") or []
    print(f"scan policy: {data.get('scan', '?')}")
    if not nets:
        print("no known networks")
        return 0
    for n in nets:
        print(f"  {n.get('ssid', '')}")
    return 0


def cmd_delete(base: str, ssid: str) -> int:
    _, data = request(base, "/api/wifi/networks", method="DELETE", body={"ssid": ssid})
    print_json(data)
    return 0


def cmd_connect(base: str, ssid: str, password: str | None) -> int:
    if password is None:
        password = getpass.getpass("password (empty if open): ")
    _, data = request(
        base,
        "/api/wifi/connect",
        method="POST",
        body={"ssid": ssid, "password": password},
    )
    print_json(data)
    print("joining in the background; SoftAP drops if this succeeds.")
    print("USB NCM http://192.168.7.1/api/wifi  still works while it tries.")
    return 0


def cmd_scan_mode(base: str, mode: str) -> int:
    _, data = request(base, "/api/wifi", method="PUT", body={"scan": mode})
    print_json(data)
    return 0


def cmd_ap(base: str) -> int:
    _, data = request(base, "/api/wifi/ap", method="POST")
    print_json(data)
    return 0


def cmd_mdns(base: str, name: str) -> int:
    _, data = request(base, "/api/wifi", method="PUT", body={"mdns": name})
    print_json(data)
    return 0


def cmd_set_ap(base: str, ssid: str | None, password: str | None) -> int:
    body = {}
    if ssid is not None:
        body["ap_ssid"] = ssid
    if password is not None:
        body["ap_password"] = password
    if not body:
        raise SystemExit("pass --ssid and/or --password")
    _, data = request(base, "/api/wifi", method="PUT", body=body)
    print_json(data)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="Manage pm220-pico2w Wi-Fi")
    p.add_argument(
        "--base",
        default=DEFAULT_BASE,
        help=f"Pico HTTP base (default {DEFAULT_BASE}; AP is http://192.168.4.1)",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="AP/STA status")
    sc = sub.add_parser("scan", help="scan nearby APs")
    sc.add_argument("--wait", type=float, default=8, help="seconds to wait for results")
    sub.add_parser("known", help="list saved networks")
    d = sub.add_parser("delete", help="forget a saved SSID")
    d.add_argument("ssid")
    c = sub.add_parser("connect", help="save SSID and join it")
    c.add_argument("ssid", nargs="+", help="SSID (quote or use multiple words)")
    c.add_argument("--password", "-p", help="PSK; prompt if omitted")
    m = sub.add_parser("scan-mode", help="idle | always | never")
    m.add_argument("mode", choices=["idle", "always", "never"])
    sub.add_parser("ap", help="leave STA and start SoftAP")
    md = sub.add_parser("mdns", help="set mDNS hostname (name.local)")
    md.add_argument("name")
    apc = sub.add_parser("set-ap", help="change SoftAP SSID/password")
    apc.add_argument("--ssid")
    apc.add_argument("--password")

    args = p.parse_args()
    base = args.base
    if args.cmd == "status":
        return cmd_status(base)
    if args.cmd == "scan":
        return cmd_scan(base, args.wait)
    if args.cmd == "known":
        return cmd_known(base)
    if args.cmd == "delete":
        return cmd_delete(base, args.ssid)
    if args.cmd == "connect":
        return cmd_connect(base, " ".join(args.ssid), args.password)
    if args.cmd == "scan-mode":
        return cmd_scan_mode(base, args.mode)
    if args.cmd == "ap":
        return cmd_ap(base)
    if args.cmd == "mdns":
        return cmd_mdns(base, args.name)
    if args.cmd == "set-ap":
        return cmd_set_ap(base, args.ssid, args.password)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
