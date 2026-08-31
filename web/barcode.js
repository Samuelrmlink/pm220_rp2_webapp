/** Code 128 (set B) — payload in, bars out. No bitmap stored. */

import { blitCrisp, FONT_STACK, geom, layoutSize, registerDrawer } from "./raster.js";

/* 6 run widths (bar, space, …) summing to 11. Index = code value. */
const PAT = [
    "212222", "222122", "222221", "121223", "121322", "131222", "122213", "122312", "132212", "221213",
    "221312", "231212", "112232", "122132", "122231", "113222", "123122", "123221", "223211", "221132",
    "221231", "213212", "223112", "312131", "311222", "321122", "321221", "312212", "322112", "322211",
    "212123", "212321", "232121", "111323", "131123", "131321", "112313", "132113", "132311", "211313",
    "231113", "231311", "112133", "112331", "132131", "113123", "113321", "133121", "313121", "211331",
    "231131", "213113", "213311", "213131", "311123", "311321", "331121", "312113", "312311", "332111",
    "314111", "221411", "431111", "111224", "111422", "121124", "121421", "141122", "141221", "112214",
    "112412", "122114", "122411", "142112", "142211", "241211", "221114", "413111", "241112", "134111",
    "111242", "121142", "121241", "114212", "124112", "124211", "411212", "421112", "421211", "212141",
    "214121", "412121", "111143", "111341", "131141", "114113", "114311", "411113", "411311", "113141",
    "114131", "311141", "411131", "211412", "211214", "211232",
];
const START_B = 104;
const STOP = "2331112";

export function code128Error(payload) {
    const s = String(payload ?? "");
    if (!s) {
        return "Enter barcode data.";
    }
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c < 32 || c > 126) {
            return "Code 128 B supports ASCII 32–126.";
        }
    }
    return "";
}

export function encodeCode128(payload) {
    const err = code128Error(payload);
    if (err) {
        return null;
    }
    const values = [START_B];
    let sum = START_B;
    const s = String(payload);
    for (let i = 0; i < s.length; i++) {
        const v = s.charCodeAt(i) - 32;
        values.push(v);
        sum += v * (i + 1);
    }
    values.push(sum % 103);
    let modules = 0;
    const runs = [];
    for (const v of values) {
        const p = PAT[v];
        for (const ch of p) {
            const n = ch.charCodeAt(0) - 48;
            runs.push(n);
            modules += n;
        }
    }
    for (const ch of STOP) {
        const n = ch.charCodeAt(0) - 48;
        runs.push(n);
        modules += n;
    }
    return { runs, modules, text: s };
}

export function drawBarcode(ctx, obj) {
    const enc = encodeCode128(obj.payload);
    if (!enc) {
        return;
    }
    const qz = 10;
    const total = enc.modules + 2 * qz;
    const src = document.createElement("canvas");
    src.width = total;
    src.height = 24;
    const img = src.getContext("2d").createImageData(src.width, src.height);
    const d = img.data;
    d.fill(255);
    for (let i = 3; i < d.length; i += 4) {
        d[i] = 255;
    }
    const blackCol = (x0, x1) => {
        const xa = Math.max(0, x0);
        const xb = Math.min(src.width, x1);
        for (let y = 0; y < src.height; y++) {
            for (let x = xa; x < xb; x++) {
                const i = (y * src.width + x) * 4;
                d[i] = d[i + 1] = d[i + 2] = 0;
            }
        }
    };
    let x = qz;
    let bar = true;
    for (const n of enc.runs) {
        if (bar) {
            blackCol(x, x + n);
        }
        x += n;
        bar = !bar;
    }
    src.getContext("2d").putImageData(img, 0, 0);
    blitCrisp(ctx, obj, src);
    if (obj.showText === false) {
        return;
    }
    const g = geom(obj);
    const ls = layoutSize(obj);
    const size = Math.max(4, Number(obj.textSize) || 12);
    const off = obj.textOffset == null ? 7 : Number(obj.textOffset);
    ctx.save();
    ctx.translate(g.x + g.width / 2, g.y + g.height / 2);
    ctx.rotate((Number(obj.rotate) || 0) * Math.PI / 180);
    ctx.translate(-ls.w / 2, -ls.h / 2);
    ctx.font = `${size}px ${FONT_STACK}`;
    ctx.textAlign = "center";
    ctx.textBaseline = "alphabetic";
    const m = ctx.measureText(enc.text);
    const ascent = Number.isFinite(m.actualBoundingBoxAscent) ? m.actualBoundingBoxAscent : size * 0.8;
    const descent = Number.isFinite(m.actualBoundingBoxDescent) ? m.actualBoundingBoxDescent : size * 0.2;
    const tw = m.width;
    const inkBottom = ls.h + off;
    const baseline = inkBottom - descent;
    const inkTop = baseline - ascent;
    const pad = Math.max(2, Math.round(size * 0.15));
    ctx.fillStyle = "#fff";
    ctx.fillRect((ls.w - tw) / 2 - pad, inkTop - pad, tw + 2 * pad, ascent + descent + 2 * pad);
    ctx.fillStyle = "#000";
    ctx.fillText(enc.text, ls.w / 2, baseline);
    ctx.restore();
}

registerDrawer("barcode1d", drawBarcode);
