#!/usr/bin/env python3
"""Pack web/ into pre-gzipped files for LittleFS, and optionally upload them.

Stdlib only. Gzip is used instead of a JS minifier so there is no npm step.
Text assets are stored as <name>.gz and served by the Pico with Content-Encoding: gzip.
"""
from __future__ import annotations

import argparse
import gzip
import json
import shutil
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
DIST = ROOT / "build" / "web_dist"

GZIP_EXTS = {".html", ".css", ".js", ".mjs", ".json", ".svg", ".txt", ".map"}
MAX_FILE = 64 * 1024
NAME_MAX = 64
DEFAULT_BASE = "http://192.168.7.1"


def valid_name(name: str) -> bool:
    if not name or name.startswith(".") or len(name) > NAME_MAX:
        return False
    return all(ch.isalnum() or ch in "._-" for ch in name)


def pack(src: Path, dest: Path) -> list[Path]:
    if not src.is_dir():
        raise SystemExit(f"web directory missing: {src}")
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    written: list[Path] = []
    total_in = 0
    total_out = 0
    print(f"packing {src} -> {dest}")
    for path in sorted(p for p in src.iterdir() if p.is_file()):
        data = path.read_bytes()
        total_in += len(data)
        if path.suffix.lower() in GZIP_EXTS:
            packed = gzip.compress(data, compresslevel=9, mtime=0)
            out_name = path.name + ".gz"
            out = dest / out_name
            out.write_bytes(packed)
            print(f"  {path.name}: {len(data)} -> {len(packed)} gzip")
        else:
            out_name = path.name
            out = dest / out_name
            out.write_bytes(data)
            print(f"  {path.name}: {len(data)} raw")
        if not valid_name(out_name):
            raise SystemExit(f"invalid LittleFS name: {out_name}")
        size = out.stat().st_size
        if size > MAX_FILE:
            raise SystemExit(f"{out_name} is {size} bytes; max {MAX_FILE}")
        total_out += size
        written.append(out)
    print(f"  total {total_in} -> {total_out} ({len(written)} files)")
    return written


def request(url: str, method: str = "GET", data: bytes | None = None, timeout: float = 30):
    headers = {}
    if data is not None:
        headers["Content-Type"] = "application/octet-stream"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        body = e.read() if e.fp else b""
        raise SystemExit(f"{method} {url} -> {e.code} {body.decode('utf-8', 'replace')}") from e
    except urllib.error.URLError as e:
        raise SystemExit(f"{method} {url} failed: {e.reason}") from e


def upload(base: str, dest: Path, sync: bool) -> None:
    if not dest.is_dir():
        raise SystemExit(f"packed directory missing: {dest} (run without --upload first?)")
    base = base.rstrip("/")
    files = {p.name: p for p in dest.iterdir() if p.is_file()}
    if not files:
        raise SystemExit(f"no files in {dest}")
    print(f"uploading {len(files)} files to {base}")
    for name, path in sorted(files.items()):
        data = path.read_bytes()
        status, body = request(f"{base}/api/fs/{name}", method="PUT", data=data)
        print(f"  PUT {name} {len(data)}B -> {status} {body.decode()}")
    if sync:
        _, listing = request(f"{base}/api/fs")
        remote = json.loads(listing.decode())
        for info in remote.get("files", []):
            name = info.get("name")
            if name and name not in files:
                status, body = request(f"{base}/api/fs/{name}", method="DELETE")
                print(f"  DELETE {name} -> {status} {body.decode()}")


def list_remote(base: str) -> None:
    base = base.rstrip("/")
    _, body = request(f"{base}/api/fs")
    listing = json.loads(body.decode())
    files = listing.get("files", [])
    total = 0
    for info in files:
        size = int(info.get("size", 0))
        total += size
        print(f"  {info.get('name')} {size}")
    print(f"  {len(files)} files, {total} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack pm220-pico2w/web for LittleFS")
    parser.add_argument("--src", type=Path, default=WEB)
    parser.add_argument("--dest", type=Path, default=DIST)
    parser.add_argument(
        "--base",
        default=DEFAULT_BASE,
        help=f"Pico HTTP base URL (default {DEFAULT_BASE})",
    )
    parser.add_argument(
        "--upload",
        action="store_true",
        help="PUT packed files to --base",
    )
    parser.add_argument(
        "--sync",
        action="store_true",
        help="with --upload, DELETE device files that are not in the pack",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list files already on the Pico and exit",
    )
    args = parser.parse_args()
    if args.list:
        list_remote(args.base)
        return 0
    pack(args.src, args.dest)
    if args.upload:
        upload(args.base, args.dest, args.sync)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
