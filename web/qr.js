/** Byte-mode QR (versions 1–10). Payload stored as text; modules drawn to fit the box. */

import { blitCrisp, registerDrawer } from "./raster.js";

const ECC_I = { L: 0, M: 1, Q: 2, H: 3 };
const TOTAL = [0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346];
/* per version: [n1, data1, n2, data2, ec] for L,M,Q,H */
const BLOCKS = [
    null,
    [[1, 19, 0, 0, 7], [1, 16, 0, 0, 10], [1, 13, 0, 0, 13], [1, 9, 0, 0, 17]],
    [[1, 34, 0, 0, 10], [1, 28, 0, 0, 16], [1, 22, 0, 0, 22], [1, 16, 0, 0, 28]],
    [[1, 55, 0, 0, 15], [1, 44, 0, 0, 26], [2, 17, 0, 0, 18], [2, 13, 0, 0, 22]],
    [[1, 80, 0, 0, 20], [2, 32, 0, 0, 18], [2, 24, 0, 0, 26], [4, 9, 0, 0, 16]],
    [[1, 108, 0, 0, 26], [2, 43, 0, 0, 24], [2, 15, 2, 16, 18], [2, 11, 2, 12, 22]],
    [[2, 68, 0, 0, 18], [4, 27, 0, 0, 16], [4, 19, 0, 0, 24], [4, 15, 0, 0, 28]],
    [[2, 78, 0, 0, 20], [4, 31, 0, 0, 18], [2, 14, 4, 15, 18], [4, 13, 1, 14, 26]],
    [[2, 97, 0, 0, 24], [2, 38, 2, 39, 22], [4, 18, 2, 19, 22], [4, 14, 2, 15, 26]],
    [[2, 116, 0, 0, 30], [3, 36, 2, 37, 22], [4, 16, 4, 17, 20], [4, 12, 4, 13, 24]],
    [[2, 68, 2, 69, 18], [4, 43, 1, 44, 26], [6, 19, 2, 20, 24], [6, 15, 2, 16, 28]],
];
const ALIGN = [
    [], [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34],
    [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50],
];
const REMAIN = [0, 0, 7, 7, 7, 7, 7, 0, 0, 0, 0];

const gfExp = new Uint8Array(512);
const gfLog = new Uint8Array(256);
(function initGF() {
    let x = 1;
    for (let i = 0; i < 255; i++) {
        gfExp[i] = x;
        gfLog[x] = i;
        x <<= 1;
        if (x & 0x100) {
            x ^= 0x11d;
        }
    }
    for (let i = 255; i < 512; i++) {
        gfExp[i] = gfExp[i - 255];
    }
}());

function gfMul(a, b) {
    if (!a || !b) {
        return 0;
    }
    return gfExp[gfLog[a] + gfLog[b]];
}

function rsGen(degree) {
    let g = [1];
    for (let i = 0; i < degree; i++) {
        const ng = new Array(g.length + 1).fill(0);
        const coef = gfExp[i];
        for (let j = 0; j < g.length; j++) {
            ng[j] ^= gfMul(g[j], coef);
            ng[j + 1] ^= g[j];
        }
        g = ng;
    }
    return g;
}

function rsEncode(data, ecLen) {
    const gen = rsGen(ecLen);
    const ec = new Array(ecLen).fill(0);
    for (const b of data) {
        const factor = b ^ ec[0];
        ec.shift();
        ec.push(0);
        if (!factor) {
            continue;
        }
        for (let i = 0; i < ecLen; i++) {
            ec[i] ^= gfMul(gen[i + 1], factor);
        }
    }
    return ec;
}

function bytesOf(s) {
    return new TextEncoder().encode(String(s ?? ""));
}

export function qrError(payload) {
    if (!String(payload ?? "")) {
        return "Enter QR contents.";
    }
    if (bytesOf(payload).length > 130) {
        return "QR payload too long.";
    }
    return "";
}

function dataCapacity(ver, ecc) {
    const b = BLOCKS[ver][ECC_I[ecc]];
    return b[0] * b[1] + b[2] * b[3];
}

function pickVersion(n, ecc) {
    for (let v = 1; v <= 6; v++) {
        const cap = dataCapacity(v, ecc);
        const bits = 4 + (v >= 10 ? 16 : 8) + n * 8 + 4;
        const need = Math.ceil(bits / 8);
        if (need <= cap) {
            return v;
        }
    }
    return 0;
}

function buildData(payload, ver, ecc) {
    const raw = bytesOf(payload);
    const cap = dataCapacity(ver, ecc);
    const bits = [];
    const push = (val, n) => {
        for (let i = n - 1; i >= 0; i--) {
            bits.push((val >>> i) & 1);
        }
    };
    push(0b0100, 4);
    push(raw.length, ver >= 10 ? 16 : 8);
    for (const b of raw) {
        push(b, 8);
    }
    const maxBits = cap * 8;
    const term = Math.min(4, maxBits - bits.length);
    push(0, term);
    while (bits.length % 8) {
        bits.push(0);
    }
    const bytes = [];
    for (let i = 0; i < bits.length; i += 8) {
        let v = 0;
        for (let j = 0; j < 8; j++) {
            v = (v << 1) | bits[i + j];
        }
        bytes.push(v);
    }
    let pad = 0xec;
    while (bytes.length < cap) {
        bytes.push(pad);
        pad = pad === 0xec ? 0x11 : 0xec;
    }
    return bytes;
}

function interleave(data, ver, ecc) {
    const [n1, d1, n2, d2, ec] = BLOCKS[ver][ECC_I[ecc]];
    const blocks = [];
    let off = 0;
    for (let i = 0; i < n1; i++) {
        const d = data.slice(off, off + d1);
        off += d1;
        blocks.push({ d, e: rsEncode(d, ec) });
    }
    for (let i = 0; i < n2; i++) {
        const d = data.slice(off, off + d2);
        off += d2;
        blocks.push({ d, e: rsEncode(d, ec) });
    }
    const out = [];
    const maxD = Math.max(d1, d2);
    for (let i = 0; i < maxD; i++) {
        for (const b of blocks) {
            if (i < b.d.length) {
                out.push(b.d[i]);
            }
        }
    }
    for (let i = 0; i < ec; i++) {
        for (const b of blocks) {
            out.push(b.e[i]);
        }
    }
    return out;
}

function sizeOf(ver) {
    return 21 + 4 * (ver - 1);
}

function isFunc(mod, r, c, ver) {
    const n = sizeOf(ver);
    if (r < 9 && c < 9) {
        return true;
    }
    if (r < 9 && c >= n - 8) {
        return true;
    }
    if (r >= n - 8 && c < 9) {
        return true;
    }
    if (r === 6 || c === 6) {
        return true;
    }
    const ap = ALIGN[ver];
    for (const ar of ap) {
        for (const ac of ap) {
            if (Math.abs(r - ar) <= 2 && Math.abs(c - ac) <= 2) {
                if (!(ar === 6 && ac === 6) && !(ar === 6 && ac === n - 7) && !(ar === n - 7 && ac === 6)) {
                    return true;
                }
            }
        }
    }
    if (ver >= 7) {
        if (r < 6 && c >= n - 11) {
            return true;
        }
        if (c < 6 && r >= n - 11) {
            return true;
        }
    }
    return false;
}

function placeFinders(mod, n) {
    const stamp = (r0, c0) => {
        for (let r = -1; r < 8; r++) {
            for (let c = -1; c < 8; c++) {
                const rr = r0 + r;
                const cc = c0 + c;
                if (rr < 0 || cc < 0 || rr >= n || cc >= n) {
                    continue;
                }
                const on = r >= 0 && r <= 6 && c >= 0 && c <= 6 &&
                    (r === 0 || r === 6 || c === 0 || c === 6 || (r >= 2 && r <= 4 && c >= 2 && c <= 4));
                mod[rr][cc] = on ? 1 : 0;
            }
        }
    };
    stamp(0, 0);
    stamp(0, n - 7);
    stamp(n - 7, 0);
}

function placeTiming(mod, n) {
    for (let i = 8; i < n - 8; i++) {
        mod[6][i] = i % 2 === 0 ? 1 : 0;
        mod[i][6] = i % 2 === 0 ? 1 : 0;
    }
}

function placeAlign(mod, ver) {
    const n = sizeOf(ver);
    const ap = ALIGN[ver];
    for (const ar of ap) {
        for (const ac of ap) {
            if ((ar === 6 && ac === 6) || (ar === 6 && ac === n - 7) || (ar === n - 7 && ac === 6)) {
                continue;
            }
            for (let dr = -2; dr <= 2; dr++) {
                for (let dc = -2; dc <= 2; dc++) {
                    const on = Math.max(Math.abs(dr), Math.abs(dc)) !== 1;
                    mod[ar + dr][ac + dc] = on ? 1 : 0;
                }
            }
        }
    }
}

function maskFn(mask, r, c) {
    switch (mask) {
        case 0: return (r + c) % 2 === 0;
        case 1: return r % 2 === 0;
        case 2: return c % 3 === 0;
        case 3: return (r + c) % 3 === 0;
        case 4: return (Math.floor(r / 2) + Math.floor(c / 3)) % 2 === 0;
        case 5: return (r * c) % 2 + (r * c) % 3 === 0;
        case 6: return ((r * c) % 2 + (r * c) % 3) % 2 === 0;
        default: return ((r + c) % 2 + (r * c) % 3) % 2 === 0;
    }
}

function placeData(mod, ver, data, mask) {
    const n = sizeOf(ver);
    let bit = 0;
    const bits = [];
    for (const b of data) {
        for (let i = 7; i >= 0; i--) {
            bits.push((b >>> i) & 1);
        }
    }
    for (let i = 0; i < REMAIN[ver]; i++) {
        bits.push(0);
    }
    let dir = -1;
    let row = n - 1;
    for (let col = n - 1; col > 0; col -= 2) {
        if (col === 6) {
            col--;
        }
        for (;;) {
            for (const dc of [0, -1]) {
                const c = col + dc;
                const r = row;
                if (!isFunc(mod, r, c, ver)) {
                    let v = bits[bit] || 0;
                    if (maskFn(mask, r, c)) {
                        v ^= 1;
                    }
                    mod[r][c] = v;
                    bit++;
                }
            }
            row += dir;
            if (row < 0 || row >= n) {
                dir = -dir;
                row += dir;
                break;
            }
        }
    }
}

function formatBits(ecc, mask) {
    const e = { L: 1, M: 0, Q: 3, H: 2 }[ecc];
    let data = (e << 3) | mask;
    let d = data << 10;
    const gen = 0b10100110111;
    for (let i = 14; i >= 10; i--) {
        if ((d >>> i) & 1) {
            d ^= gen << (i - 10);
        }
    }
    return ((data << 10) | d) ^ 0x5412;
}

function placeFormat(mod, ver, ecc, mask) {
    const n = sizeOf(ver);
    const bits = formatBits(ecc, mask);
    const set = (r, c, i) => {
        mod[r][c] = (bits >>> i) & 1;
    };
    for (let i = 0; i < 6; i++) {
        set(8, i, i);
    }
    set(8, 7, 6);
    set(8, 8, 7);
    set(7, 8, 8);
    for (let i = 9; i < 15; i++) {
        set(14 - i, 8, i);
    }
    for (let i = 0; i < 8; i++) {
        set(n - 1 - i, 8, i);
    }
    for (let i = 8; i < 15; i++) {
        set(8, n - 15 + i, i);
    }
    mod[n - 8][8] = 1;
}

function penalty(mod, n) {
    let s = 0;
    for (let r = 0; r < n; r++) {
        let run = 1;
        for (let c = 1; c <= n; c++) {
            if (c < n && mod[r][c] === mod[r][c - 1]) {
                run++;
            } else {
                if (run >= 5) {
                    s += 3 + (run - 5);
                }
                run = 1;
            }
        }
    }
    for (let c = 0; c < n; c++) {
        let run = 1;
        for (let r = 1; r <= n; r++) {
            if (r < n && mod[r][c] === mod[r - 1][c]) {
                run++;
            } else {
                if (run >= 5) {
                    s += 3 + (run - 5);
                }
                run = 1;
            }
        }
    }
    for (let r = 0; r < n - 1; r++) {
        for (let c = 0; c < n - 1; c++) {
            if (mod[r][c] === mod[r][c + 1] && mod[r][c] === mod[r + 1][c] && mod[r][c] === mod[r + 1][c + 1]) {
                s += 3;
            }
        }
    }
    let dark = 0;
    for (let r = 0; r < n; r++) {
        for (let c = 0; c < n; c++) {
            dark += mod[r][c];
        }
    }
    s += Math.floor(Math.abs(dark * 100 / (n * n) - 50) / 5) * 10;
    return s;
}

const qrCache = new Map();

export function makeQr(payload, ecc) {
    ecc = ECC_I[ecc] != null ? ecc : "M";
    const err = qrError(payload);
    if (err) {
        return null;
    }
    const key = ecc + "\0" + payload;
    const hit = qrCache.get(key);
    if (hit) {
        return hit;
    }
    const ver = pickVersion(bytesOf(payload).length, ecc);
    if (!ver) {
        return null;
    }
    const n = sizeOf(ver);
    const data = interleave(buildData(payload, ver, ecc), ver, ecc);
    let best = null;
    let bestScore = Infinity;
    for (let mask = 0; mask < 8; mask++) {
        const mod = Array.from({ length: n }, () => new Uint8Array(n));
        placeFinders(mod, n);
        placeTiming(mod, n);
        placeAlign(mod, ver);
        placeData(mod, ver, data, mask);
        placeFormat(mod, ver, ecc, mask);
        const score = penalty(mod, n);
        if (score < bestScore) {
            bestScore = score;
            best = mod;
        }
    }
    const out = { mod: best, n };
    qrCache.set(key, out);
    return out;
}

function qrBitmap(qr) {
    const qz = 4;
    const dim = qr.n + 2 * qz;
    const src = document.createElement("canvas");
    src.width = dim;
    src.height = dim;
    const c = src.getContext("2d");
    c.fillStyle = "#fff";
    c.fillRect(0, 0, dim, dim);
    const img = c.getImageData(0, 0, dim, dim);
    const d = img.data;
    for (let r = 0; r < qr.n; r++) {
        for (let col = 0; col < qr.n; col++) {
            if (!qr.mod[r][col]) {
                continue;
            }
            const x = col + qz;
            const y = r + qz;
            const i = (y * dim + x) * 4;
            d[i] = d[i + 1] = d[i + 2] = 0;
            d[i + 3] = 255;
        }
    }
    c.putImageData(img, 0, 0);
    return src;
}

export function drawQr(ctx, obj) {
    const qr = makeQr(obj.payload, obj.ecc || "M");
    if (!qr) {
        return;
    }
    blitCrisp(ctx, obj, qrBitmap(qr));
}

registerDrawer("qr", drawQr);
