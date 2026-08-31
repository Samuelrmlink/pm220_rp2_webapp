#!/usr/bin/env python3
"""Serve the label editor from web/ during PC-side development.

Stdlib only. The Pico API is reached from the browser over CORS (?api=).
"""
from __future__ import annotations

import argparse
import http.server
import os
import socketserver
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "web"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def log_message(self, fmt: str, *args) -> None:
        print("[%s] %s" % (self.log_date_time_string(), fmt % args))


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve pm220-pico2w/web on the PC")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    if not ROOT.is_dir():
        raise SystemExit(f"web directory missing: {ROOT}")
    os.chdir(ROOT)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer((args.bind, args.port), Handler) as httpd:
        base = f"http://{args.bind}:{args.port}/"
        print(f"Serving {ROOT}")
        print(f"  {base}")
        print(f"  {base}?api=http://192.168.7.1   USB NCM")
        print(f"  {base}?api=http://192.168.4.1   Wi-Fi AP")
        print(f"  {base}?api=http://pm220.local   mDNS")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
