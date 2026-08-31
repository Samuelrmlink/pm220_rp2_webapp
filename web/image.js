/** Image import (downscaled grayscale PNG) and 1-bit blit. */

import { registerDrawer, withBox } from "./raster.js";

const MAX_SIDE = 384;
const cache = new Map();

export function importImageFile(file, onReady) {
    return new Promise((resolve, reject) => {
        const url = URL.createObjectURL(file);
        const im = new Image();
        im.onload = () => {
            URL.revokeObjectURL(url);
            const scale = Math.min(1, MAX_SIDE / Math.max(im.naturalWidth, im.naturalHeight));
            const w = Math.max(1, Math.round(im.naturalWidth * scale));
            const h = Math.max(1, Math.round(im.naturalHeight * scale));
            const c = document.createElement("canvas");
            c.width = w;
            c.height = h;
            const ctx = c.getContext("2d");
            ctx.fillStyle = "#fff";
            ctx.fillRect(0, 0, w, h);
            ctx.drawImage(im, 0, 0, w, h);
            const img = ctx.getImageData(0, 0, w, h);
            for (let i = 0; i < img.data.length; i += 4) {
                const y = img.data[i] * 0.299 + img.data[i + 1] * 0.587 + img.data[i + 2] * 0.114;
                img.data[i] = img.data[i + 1] = img.data[i + 2] = y;
                img.data[i + 3] = 255;
            }
            ctx.putImageData(img, 0, 0);
            const dataUrl = c.toDataURL("image/png");
            const png = dataUrl.replace(/^data:image\/png;base64,/, "");
            resolve({ png, srcW: w, srcH: h });
            onReady?.();
        };
        im.onerror = () => {
            URL.revokeObjectURL(url);
            reject(new Error("could not load image"));
        };
        im.src = url;
    });
}

function cachedPng(b64, onReady) {
    let im = cache.get(b64);
    if (im) {
        return im;
    }
    im = new Image();
    im.onload = () => onReady?.();
    im.src = "data:image/png;base64," + b64;
    cache.set(b64, im);
    return im;
}

function ditherOrThreshold(data, w, h, dither) {
    const lum = new Float32Array(w * h);
    for (let i = 0, p = 0; i < data.length; i += 4, p++) {
        lum[p] = data[i] * 0.299 + data[i + 1] * 0.587 + data[i + 2] * 0.114;
    }
    if (dither) {
        for (let y = 0; y < h; y++) {
            for (let x = 0; x < w; x++) {
                const i = y * w + x;
                const old = lum[i];
                const neu = old < 128 ? 0 : 255;
                const err = old - neu;
                lum[i] = neu;
                if (x + 1 < w) {
                    lum[i + 1] += err * 7 / 16;
                }
                if (y + 1 < h) {
                    if (x > 0) {
                        lum[i + w - 1] += err * 3 / 16;
                    }
                    lum[i + w] += err * 5 / 16;
                    if (x + 1 < w) {
                        lum[i + w + 1] += err * 1 / 16;
                    }
                }
            }
        }
    }
    for (let p = 0, i = 0; p < lum.length; p++, i += 4) {
        const v = lum[p] < 128 ? 0 : 255;
        data[i] = data[i + 1] = data[i + 2] = v;
        data[i + 3] = 255;
    }
}

export function drawImageObj(ctx, obj, onReady) {
    if (!obj.png) {
        return;
    }
    const im = cachedPng(obj.png, onReady);
    if (!im.complete || !im.naturalWidth) {
        return;
    }
    withBox(ctx, obj, (c, ls) => {
        const tmp = document.createElement("canvas");
        tmp.width = ls.w;
        tmp.height = ls.h;
        const t = tmp.getContext("2d");
        t.fillStyle = "#fff";
        t.fillRect(0, 0, ls.w, ls.h);
        const scale = Math.min(ls.w / im.naturalWidth, ls.h / im.naturalHeight);
        const dw = Math.max(1, Math.round(im.naturalWidth * scale));
        const dh = Math.max(1, Math.round(im.naturalHeight * scale));
        const dx = Math.floor((ls.w - dw) / 2);
        const dy = Math.floor((ls.h - dh) / 2);
        t.imageSmoothingEnabled = true;
        t.drawImage(im, dx, dy, dw, dh);
        const img = t.getImageData(0, 0, ls.w, ls.h);
        ditherOrThreshold(img.data, ls.w, ls.h, obj.dither !== false);
        t.putImageData(img, 0, 0);
        c.drawImage(tmp, 0, 0);
    });
}

registerDrawer("image", drawImageObj);
