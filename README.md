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
GET  /api/scan
POST /api/scan
GET  /api/printer
GET  /api/media
GET  /api/print/test               print the inset frame (also POST)
POST /api/printer/connect          optional {"address":"AA:BB:CC:DD:EE:FF"}
POST /api/printer/disconnect
POST /api/print                    application/octet-stream packed 1-bit, 48 bytes/row
```

Print uses the same registration as the Linux CLI (offset X −1.25 mm, Y +3 mm, crop to the TSPL page). On the Plus 2 W, the USER button also fires the test frame while SPP is up.
