/** Code 128 (set B) — payload in, bars out. No bitmap stored. */

import { blitCrisp, registerDrawer } from "./raster.js";

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
    const show = obj.showText !== false;
    const qz = 10;
    const total = enc.modules + 2 * qz;
    const barH = 24;
    const textH = show ? 8 : 0;
    const src = document.createElement("canvas");
    src.width = total;
    src.height = barH + textH;
    const c = src.getContext("2d");
    c.fillStyle = "#fff";
    c.fillRect(0, 0, src.width, src.height);
    const img = c.getImageData(0, 0, src.width, src.height);
    const d = img.data;
    const blackCol = (x0, x1, y0, y1) => {
        const xa = Math.max(0, x0);
        const xb = Math.min(src.width, x1);
        for (let y = y0; y < y1; y++) {
            for (let x = xa; x < xb; x++) {
                const i = (y * src.width + x) * 4;
                d[i] = d[i + 1] = d[i + 2] = 0;
                d[i + 3] = 255;
            }
        }
    };
    let x = qz;
    let bar = true;
    for (const n of enc.runs) {
        if (bar) {
            blackCol(x, x + n, 0, barH);
        }
        x += n;
        bar = !bar;
    }
    c.putImageData(img, 0, 0);
    if (show) {
        c.fillStyle = "#000";
        c.font = "7px Arial, Helvetica, sans-serif";
        c.textAlign = "center";
        c.textBaseline = "top";
        c.imageSmoothingEnabled = false;
        c.fillText(enc.text, src.width / 2, barH, src.width - 2);
    }
    blitCrisp(ctx, obj, src);
}

registerDrawer("barcode1d", drawBarcode);
