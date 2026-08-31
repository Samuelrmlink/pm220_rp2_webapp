import urllib.request

W, H, WB = 384, 240, 48
buf = bytearray(b"\xff" * (WB * H))  # white; 0-bit = burn

def set_black(x, y):
    if 0 <= x < W and 0 <= y < H:
        buf[y * WB + (x >> 3)] &= ~(0x80 >> (x & 7))

# 4 mm safe inset (same as GET /api/media safe_rect)
x0, y0, x1, y1 = 32, 32, 351, 207
for t in range(3):
    for x in range(x0, x1 + 1):
        set_black(x, y0 + t)
        set_black(x, y1 - t)
    for y in range(y0, y1 + 1):
        set_black(x0 + t, y)
        set_black(x1 - t, y)
for i in range(x1 - x0 + 1):
    set_black(x0 + i, y0 + i * (y1 - y0) // (x1 - x0))
    set_black(x1 - i, y0 + i * (y1 - y0) // (x1 - x0))

req = urllib.request.Request(
    "http://192.168.7.1/api/print",
    data=bytes(buf),
    method="POST",
    headers={"Content-Type": "application/octet-stream"},
)
print(urllib.request.urlopen(req, timeout=8).read().decode())
