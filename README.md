# pm220-pico2w

Nelko PM220 dongle firmware. Bluetooth Classic SPP to the printer, HTTP API + tiny web UI, no PC app.

**Development board:** Pimoroni Pico Plus 2 W (`pimoroni_pico_plus2_w_rp2350`, RP2350B, 16 MB flash, 8 MB PSRAM, CYW43439).

**Production size target:** Raspberry Pi Pico 2 W (`pico2_w`, 4 MB flash). The JavaScript UI packed into LittleFS must still fit leftover flash on that 4 MB part. Extra flash on the Plus 2 W is bring-up headroom, not a larger web app. Do not depend on PSRAM in v1.

Tooling comes from `~/.pico-sdk`:

| Piece | Version |
| --- | --- |
| Pico SDK | 2.3.0 |
| ARM GCC | 15_2_Rel1 |
| picotool | 2.2.0-a4 in `~/.pico-sdk`; CMake fetches **2.3.0** to satisfy SDK 2.3.0 |
| CMake | v4.3.4 |
| Ninja | v1.13.2 |

## Build / flash (no BOOTSEL)

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
cmake -S . -B build -G Ninja \
  -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350 \
  -DCMAKE_PROGRAM_PATH="$HOME/.pico-sdk/cmake/v4.3.4/bin;$HOME/.pico-sdk/ninja/v1.13.2"
cmake --build build
$HOME/.pico-sdk/picotool/2.2.0-a4/picotool/picotool load -f build/pm220.uf2
```

Size-check a 4 MB Pico 2 W binary with `-DPICO_BOARD=pico2_w` (clean the build dir or pass the flag on a fresh configure — board type is cached).

USB is a composite gadget: **CDC-ACM** (serial), **CDC-NCM** (USB Ethernet), and the Raspberry Pi **vendor reset** interface. Linux should get `usb0` / `enx…` and DHCP `192.168.7.16` (device is `192.168.7.1`).

```
http://192.168.7.1/api/status
http://pm220.local/api/status
```

`picotool load -f` still uses the vendor reset interface (and 1200-baud CDC as fallback).

Linux USB access (picotool + `/dev/ttyACM*` without sudo):

```bash
sudo cp udev/99-pico.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger
```

Then unplug/replug the board. This uses systemd `uaccess` instead of a `plugdev` group (this distro does not have one).

## v1 bring-up

Join Wi-Fi AP `PM220-Pico` / `pm220pico`, then:

```
http://pm220.local/
http://pm220.local/api/status
```

If `.local` does not resolve (some Linux setups need Avahi), use `http://192.168.4.1/` instead.

The board runs a Classic inquiry every 15 s until it finds a printer-like device, then SDP + RFCOMM (SPP) connects (channel from SDP, else 1) and sends `BATTERY?`. USB serial logs the handshake. LED stays on while connected.

```
GET  /api/status
GET  /api/scan                      Bluetooth inquiry (not Wi-Fi)
POST /api/scan
GET  /api/wifi                      AP/STA status, mDNS name, SoftAP SSID/PSK, scan policy
PUT  /api/wifi                      {"scan":"idle"|"always"|"never","mdns","ap_ssid","ap_password"}
GET  /api/wifi/scan
POST /api/wifi/scan                 start a Wi-Fi scan (briefly disturbs AP clients)
GET  /api/wifi/networks             known SSIDs and passwords
PUT  /api/wifi/networks             {"ssid","password"?,"new_ssid"?} save/rename, no join
POST /api/wifi/connect              {"ssid","password"?} save+join; omit password to join a known SSID
DELETE /api/wifi/networks           {"ssid"}
POST /api/wifi/ap                   leave STA, start SoftAP
GET  /api/printer
GET  /api/media                    label + framebuffer contract
GET  /api/print                    same as /api/media
GET  /api/print/test               print the inset frame (also POST)
POST /api/printer/connect          optional {"address":"AA:BB:CC:DD:EE:FF"}
POST /api/printer/disconnect
POST /api/print                    raw packed 1-bit framebuffer (see below)
GET  /api/fs                       list LittleFS files (root)
GET  /api/fs/labels                list saved labels
GET  /api/fs/labels/<file>         file bytes (`*.gz` has Content-Encoding: gzip)
PUT  /api/fs/<name>                create or replace (max 128 KiB; `labels/<file>` ok)
DELETE /api/fs/<name>
POST /api/fs/rename                {"from":"labels/a.json.gz","to":"labels/b.json.gz"}
GET  /                             index.html from LittleFS when present
```

On the Plus 2 W, the USER button also fires the test frame while SPP is up.

Wi-Fi: boot tries known networks in `settings/known_networks.json` for 60 s, then SoftAP `PM220-Pico` / `pm220pico` at `192.168.4.1`. A join attempt times out after **6 s** (association); if the radio already associated, DHCP gets **8 s** more. If the STA link drops for **2 s**, the board leaves STA, returns to SoftAP, and immediately scans for known networks when the `scan` policy allows (`idle` = no AP clients, `always`, not `never`). After that, AP-mode scans for known networks run every **30 s** (plus a 10 s backoff after a failed join). Successful STA join turns the SoftAP off; `http://pm220.local/` (or the configured mDNS name) is advertised on AP, STA, and USB NCM. Periodic scans while in AP mode default to **`idle`**. A scan hops the radio off-channel and can hitch or drop AP clients; `always` enables that, `never` is API-only.

```bash
python3 tools/wifi.py status
python3 tools/wifi.py scan
python3 tools/wifi.py known
python3 tools/wifi.py connect 'HomeSSID' --password 'secret'
python3 tools/wifi.py delete HomeSSID
python3 tools/wifi.py scan-mode idle    # idle | always | never
python3 tools/wifi.py ap                # force SoftAP
python3 tools/wifi.py --base http://192.168.4.1 status
```

The editor **WiFi Settings** button (top right, next to printer status) covers the same surface: mDNS, SoftAP SSID/PSK/scan policy, station scan + known networks.

## Label editor

Vanilla HTML/CSS/JS in `web/`. Rasterizes objects in the browser and `POST`s the packed framebuffer. Stdlib only; no npm. Text assets are **pre-gzipped** (not minified) so they fit LittleFS without a JS toolchain.

PC-side while iterating:

```bash
python3 tools/dev_server.py
# open http://127.0.0.1:8000/?api=http://192.168.7.1
```

Use `?api=http://192.168.4.1` on the phone AP, or `http://pm220.local` when mDNS works.

Load the same files onto the Pico (1 MiB LittleFS at flash offset 3 MiB; last 16 KiB of a 4 MB part is left for BTstack NVM):

```bash
python3 tools/pack_web.py                         # -> build/web_dist/*.gz
python3 tools/pack_web.py --upload                # USB NCM http://192.168.7.1
python3 tools/pack_web.py --upload --base http://192.168.4.1
python3 tools/pack_web.py --upload --sync         # also DELETE stale names
python3 tools/pack_web.py --list
```

`PUT` replaces an existing name. After upload, `http://192.168.7.1/` (or the AP / `.local` URL) serves `index.html.gz` with `Content-Encoding: gzip`. Same-origin `fetch` is used when the editor is served from the Pico; `?api=` still overrides on the PC dev server.

Open/Save in the editor talk to `labels/` on LittleFS (gzipped JSON). **This computer…** / **Download current** still use a local file. Each saved label can be downloaded to the PC as `.json`.

**Save / Open** writes a single `label.pm220.json`: text, QR and Code 128 store their strings; images store a downscaled grayscale PNG (base64). Objects use `x`, `y`, `width`, `height` in dots. QR encoding uses Project Nayuki’s MIT `qrcodegen` (`web/qrcodegen.js`).

## Framebuffer print (`POST /api/print`)

The Pico does **not** rasterize text or images. You send a pre-formatted 1-bit buffer; it wraps TSPL and applies the same die-cut registration as `nelko-pm220`.

**Payload**

| | |
| --- | --- |
| `Content-Type` | `application/octet-stream` |
| Size | `width_bytes * height_dots`. Full 50×30 mm label: **11520 bytes** (48 × 240) |
| Row | 384 dots = **48 bytes**, MSB first |
| Polarity | **0 = burn (black)**, 1 = white. Unused bits in a row must be 1 (`0xFF` fill) |
| Height | 1–240 rows. Send **240** for a full label. Shorter buffers are still offset/cropped as if they were a shorter image on the 30 mm page |

`GET /api/media` returns the numbers, including `safe_rect` (4 mm inset on the 384×240 canvas) and `after_offset` (what the Pico actually burns).

**Printable area (50×30 mm die-cut, 203 DPI)** — same as the Linux CLI:

- Head is 48 mm / 384 dots; a 50 mm label is 400 dots, so the buffer is centered then shifted.
- Registration: **X −1.25 mm, Y +3 mm**.
- For a full 384×240 buffer that becomes **BITMAP 384×216 at (0, 24)** (bottom 24 rows of your canvas are cropped; the image starts 3 mm down from the leading edge; ~2 dots of left shift).
- Keep ink inside `safe_rect` (`4 mm` inset). That rectangle is confirmed on-label.
- Do not send a solid-black page; the firmware will punch a few white pixels if you do (thermal-head protection).

```bash
# inspect the contract
curl http://192.168.7.1/api/media

# send a 384x240 packed framebuffer
python3 - <<'PY'
from pathlib import Path
import urllib.request

W, H, WB = 384, 240, 48
buf = bytearray(b"\xff" * (WB * H))  # white

def set_black(x, y):
    if 0 <= x < W and 0 <= y < H:
        buf[y * WB + (x >> 3)] &= ~(0x80 >> (x & 7))

# inset frame in the 4 mm safe margin (~32 dots)
x0, y0, x1, y1 = 32, 32, 351, 207
for t in range(3):
    for x in range(x0, x1 + 1):
        set_black(x, y0 + t); set_black(x, y1 - t)
    for y in range(y0, y1 + 1):
        set_black(x0 + t, y); set_black(x1 - t, y)

req = urllib.request.Request(
    "http://192.168.7.1/api/print",
    data=bytes(buf),
    method="POST",
    headers={"Content-Type": "application/octet-stream"},
)
print(urllib.request.urlopen(req, timeout=5).read().decode())
PY
```

Over the phone AP, use `http://192.168.4.1/api/print` instead. `http://pm220.local/api/print` works when mDNS resolves.
