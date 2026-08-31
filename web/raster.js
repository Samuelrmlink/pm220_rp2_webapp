/** Host-side 1-bit raster: wrap, auto-size, rotate, pack for POST /api/print. */

export const FONT_STACK = "Arial, Helvetica, sans-serif";

/** Canonical geometry. Migrates legacy x1/y1/x2/y2 in place. */
export function geom(obj) {
    if (obj.x == null && obj.x1 != null) {
        obj.x = obj.x1;
        obj.y = obj.y1;
        obj.width = obj.x2 - obj.x1 + 1;
        obj.height = obj.y2 - obj.y1 + 1;
    }
    const x = Number(obj.x) || 0;
    const y = Number(obj.y) || 0;
    const width = Math.max(1, Number(obj.width) || 1);
    const height = Math.max(1, Number(obj.height) || 1);
    return { x, y, width, height, x1: x, y1: y, x2: x + width - 1, y2: y + height - 1 };
}

export function boxWidth(box) {
    return geom(box).width;
}

export function boxHeight(box) {
    return geom(box).height;
}

/** Layout size in the unrotated text space. */
export function layoutSize(box) {
    const g = geom(box);
    if (box.rotate === 90 || box.rotate === 270) {
        return { w: g.height, h: g.width };
    }
    return { w: g.width, h: g.height };
}

export function boxOverflows(box, safe) {
    const g = geom(box);
    return g.x1 < safe.x0 || g.y1 < safe.y0 || g.x2 > safe.x1 || g.y2 > safe.y1;
}

export function withBox(ctx, obj, fn) {
    const g = geom(obj);
    const ls = layoutSize(obj);
    ctx.save();
    ctx.translate(g.x + g.width / 2, g.y + g.height / 2);
    ctx.rotate((Number(obj.rotate) || 0) * Math.PI / 180);
    ctx.translate(-ls.w / 2, -ls.h / 2);
    ctx.beginPath();
    ctx.rect(0, 0, ls.w, ls.h);
    ctx.clip();
    fn(ctx, ls);
    ctx.restore();
}

function rotateCrisp(src, turns) {
    turns = ((turns % 4) + 4) % 4;
    let cur = src;
    for (let t = 0; t < turns; t++) {
        const w = cur.width;
        const h = cur.height;
        const s = cur.getContext("2d").getImageData(0, 0, w, h);
        const next = document.createElement("canvas");
        next.width = h;
        next.height = w;
        const out = next.getContext("2d").createImageData(h, w);
        for (let y = 0; y < h; y++) {
            for (let x = 0; x < w; x++) {
                const si = (y * w + x) * 4;
                const nx = h - 1 - y;
                const ny = x;
                const di = (ny * h + nx) * 4;
                out.data[di] = s.data[si];
                out.data[di + 1] = s.data[si + 1];
                out.data[di + 2] = s.data[si + 2];
                out.data[di + 3] = s.data[si + 3];
            }
        }
        next.getContext("2d").putImageData(out, 0, 0);
        cur = next;
    }
    return cur;
}

/**
 * Fit a 1-bit source into the object box with nearest-neighbor sampling.
 * Scale is continuous (fills the box); each dest dot is still fully black or white.
 */
export function blitCrisp(ctx, obj, src) {
    const g = geom(obj);
    const rot = ((Number(obj.rotate) || 0) % 360 + 360) % 360;
    const turns = Math.round(rot / 90) % 4;
    const bmp = turns ? rotateCrisp(src, turns) : src;
    const scale = Math.min(g.width / bmp.width, g.height / bmp.height);
    const dw = Math.max(1, Math.round(bmp.width * scale));
    const dh = Math.max(1, Math.round(bmp.height * scale));
    const dx = g.x + Math.floor((g.width - dw) / 2);
    const dy = g.y + Math.floor((g.height - dh) / 2);
    const srcImg = bmp.getContext("2d").getImageData(0, 0, bmp.width, bmp.height);
    const dest = ctx.createImageData(dw, dh);
    const s = srcImg.data;
    const d = dest.data;
    const sw = bmp.width;
    const sh = bmp.height;
    for (let y = 0; y < dh; y++) {
        const sy = Math.min(sh - 1, Math.floor((y + 0.5) * sh / dh));
        for (let x = 0; x < dw; x++) {
            const sx = Math.min(sw - 1, Math.floor((x + 0.5) * sw / dw));
            const si = (sy * sw + sx) * 4;
            const di = (y * dw + x) * 4;
            d[di] = s[si];
            d[di + 1] = s[si + 1];
            d[di + 2] = s[si + 2];
            d[di + 3] = 255;
        }
    }
    ctx.putImageData(dest, dx, dy);
}

function fontCss(box, size) {
    const parts = [];
    if (box.italic) {
        parts.push("italic");
    }
    if (box.bold) {
        parts.push("bold");
    }
    parts.push(`${size}px`, FONT_STACK);
    return parts.join(" ");
}

function wrapLines(ctx, text, maxW, wrap) {
    const paragraphs = String(text ?? "").split("\n");
    if (!wrap) {
        return paragraphs;
    }
    const lines = [];
    for (const para of paragraphs) {
        if (para === "") {
            lines.push("");
            continue;
        }
        const words = para.split(/\s+/);
        let cur = "";
        for (const word of words) {
            const trial = cur ? `${cur} ${word}` : word;
            if (!cur || ctx.measureText(trial).width <= maxW) {
                cur = trial;
            } else {
                lines.push(cur);
                cur = word;
            }
        }
        lines.push(cur);
    }
    return lines;
}

/** Ink bounds from the alphabetic baseline. Avoid em-square / 1.2× line-height slack. */
function glyphMetrics(ctx, text, size) {
    const m = ctx.measureText(text || "Hg");
    let ascent = m.actualBoundingBoxAscent;
    let descent = m.actualBoundingBoxDescent;
    if (!Number.isFinite(ascent) || ascent < 0) {
        ascent = m.fontBoundingBoxAscent || size * 0.8;
    }
    if (!Number.isFinite(descent) || descent < 0) {
        descent = m.fontBoundingBoxDescent || size * 0.2;
    }
    const width = text ? ctx.measureText(text).width : 0;
    if (!text) {
        ascent = m.fontBoundingBoxAscent || ascent;
        descent = m.fontBoundingBoxDescent || descent;
    }
    return { width, ascent, descent, height: ascent + descent };
}

function measureBlock(ctx, box, size, lw) {
    ctx.font = fontCss(box, size);
    ctx.textBaseline = "alphabetic";
    ctx.textAlign = "left";
    const texts = wrapLines(ctx, box.text, lw, box.wrap);
    const gap = texts.length > 1 ? Math.max(1, Math.round(size * 0.12)) : 0;
    const lines = texts.map((text) => glyphMetrics(ctx, text, size));
    let blockW = 0;
    let blockH = 0;
    for (let i = 0; i < lines.length; i++) {
        blockW = Math.max(blockW, lines[i].width);
        blockH += lines[i].height;
        if (i < lines.length - 1) {
            blockH += gap;
        }
    }
    if (!lines.length) {
        const empty = glyphMetrics(ctx, "", size);
        lines.push(empty);
        blockH = empty.height;
    }
    return { lines, texts, gap, blockW, blockH };
}

function layoutFits(ctx, box, size, lw, lh) {
    const m = measureBlock(ctx, box, size, lw);
    return m.blockH <= lh + 0.01 && m.blockW <= lw + 0.01;
}

export function autoFontSize(ctx, box, lw, lh) {
    const hiMax = Math.max(4, Math.min(lw, lh));
    let lo = 4;
    let hi = hiMax;
    let best = 4;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (layoutFits(ctx, box, mid, lw, lh)) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

export function drawText(ctx, box) {
    withBox(ctx, box, (c, ls) => {
        if (ls.w < 1 || ls.h < 1) {
            return;
        }
        const size = box.autoSize ? autoFontSize(c, box, ls.w, ls.h) : Math.max(1, Number(box.size) || 12);
        c.font = fontCss(box, size);
        c.fillStyle = "#000";
        c.textBaseline = "alphabetic";
        c.textAlign = "left";
        const m = measureBlock(c, box, size, ls.w);
        let y = 0;
        if (box.valign === "middle") {
            y = (ls.h - m.blockH) / 2;
        } else if (box.valign === "bottom") {
            y = ls.h - m.blockH;
        }
        const ul = Math.max(1, Math.round(size / 12));
        for (let i = 0; i < m.texts.length; i++) {
            const line = m.lines[i];
            const text = m.texts[i];
            let x = 0;
            if (box.align === "center") {
                x = (ls.w - line.width) / 2;
            } else if (box.align === "right") {
                x = ls.w - line.width;
            }
            const baseline = y + line.ascent;
            c.fillText(text, x, baseline);
            if (box.underline && text) {
                c.fillRect(x, baseline + Math.max(1, Math.round(line.descent * 0.35)), line.width, ul);
            }
            y += line.height + m.gap;
        }
    });
}

const drawers = { text: drawText };

export function registerDrawer(type, fn) {
    drawers[type] = fn;
}

export function rasterize(page, objects, onReady) {
    const w = page.width_dots;
    const h = page.height_dots;
    const canvas = document.createElement("canvas");
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, w, h);
    for (const obj of objects) {
        const draw = drawers[obj.type] || drawText;
        draw(ctx, obj, onReady);
    }
    const img = ctx.getImageData(0, 0, w, h);
    const wb = page.width_bytes;
    const packed = new Uint8Array(wb * h);
    packed.fill(0xff);
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const i = (y * w + x) * 4;
            const lum = img.data[i] * 0.299 + img.data[i + 1] * 0.587 + img.data[i + 2] * 0.114;
            if (lum < 160) {
                packed[y * wb + (x >> 3)] &= ~(0x80 >> (x & 7));
            }
        }
    }
    return { canvas, packed };
}
