/** QR drawing. Encoding is Nayuki's MIT qrcodegen (web/qrcodegen.js). */

import { blitCrisp, registerDrawer } from "./raster.js";
import { qrcodegen } from "./qrcodegen.js";

const Ecc = qrcodegen.QrCode.Ecc;
const ECC = { L: Ecc.LOW, M: Ecc.MEDIUM, Q: Ecc.QUARTILE, H: Ecc.HIGH };

export function qrError(payload) {
    const s = String(payload ?? "");
    if (!s) {
        return "Enter QR contents.";
    }
    try {
        qrcodegen.QrCode.encodeText(s, Ecc.LOW);
    } catch {
        return "QR payload too long.";
    }
    return "";
}

export function makeQr(payload, ecc) {
    const s = String(payload ?? "");
    if (!s) {
        return null;
    }
    const level = ECC[ecc] || Ecc.MEDIUM;
    try {
        const qr = qrcodegen.QrCode.encodeText(s, level);
        const n = qr.size;
        const mod = Array.from({ length: n }, () => new Uint8Array(n));
        for (let y = 0; y < n; y++) {
            for (let x = 0; x < n; x++) {
                if (qr.getModule(x, y)) {
                    mod[y][x] = 1;
                }
            }
        }
        return { mod, n };
    } catch {
        return null;
    }
}

function qrBitmap(qr) {
    const qz = 4;
    const dim = qr.n + 2 * qz;
    const src = document.createElement("canvas");
    src.width = dim;
    src.height = dim;
    const c = src.getContext("2d");
    const img = c.createImageData(dim, dim);
    const d = img.data;
    d.fill(255);
    for (let i = 3; i < d.length; i += 4) {
        d[i] = 255;
    }
    for (let r = 0; r < qr.n; r++) {
        for (let col = 0; col < qr.n; col++) {
            if (!qr.mod[r][col]) {
                continue;
            }
            const x = col + qz;
            const y = r + qz;
            const p = (y * dim + x) * 4;
            d[p] = d[p + 1] = d[p + 2] = 0;
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
