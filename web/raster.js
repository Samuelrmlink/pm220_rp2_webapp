/** Host-side 1-bit raster: wrap, auto-size, rotate, pack for POST /api/print. */

export const FONT_STACK = "Arial, Helvetica, sans-serif";

export function boxWidth(box) {
    return box.x2 - box.x1 + 1;
}

export function boxHeight(box) {
    return box.y2 - box.y1 + 1;
}

/** Layout size in the unrotated text space. */
export function layoutSize(box) {
    const w = boxWidth(box);
    const h = boxHeight(box);
    if (box.rotate === 90 || box.rotate === 270) {
        return { w: h, h: w };
    }
    return { w, h };
}

export function boxOverflows(box, safe) {
    return box.x1 < safe.x0 || box.y1 < safe.y0 || box.x2 > safe.x1 || box.y2 > safe.y1;
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

function drawBox(ctx, box) {
    const ls = layoutSize(box);
    if (ls.w < 1 || ls.h < 1) {
        return;
    }
    const size = box.autoSize ? autoFontSize(ctx, box, ls.w, ls.h) : Math.max(1, Number(box.size) || 12);
    const cx = (box.x1 + box.x2 + 1) / 2;
    const cy = (box.y1 + box.y2 + 1) / 2;
    ctx.save();
    ctx.translate(cx, cy);
    ctx.rotate((Number(box.rotate) || 0) * Math.PI / 180);
    ctx.translate(-ls.w / 2, -ls.h / 2);
    ctx.beginPath();
    ctx.rect(0, 0, ls.w, ls.h);
    ctx.clip();
    ctx.font = fontCss(box, size);
    ctx.fillStyle = "#000";
    ctx.textBaseline = "alphabetic";
    ctx.textAlign = "left";
    const m = measureBlock(ctx, box, size, ls.w);
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
        ctx.fillText(text, x, baseline);
        if (box.underline && text) {
            ctx.fillRect(x, baseline + Math.max(1, Math.round(line.descent * 0.35)), line.width, ul);
        }
        y += line.height + m.gap;
    }
    ctx.restore();
}

export function rasterize(page, boxes) {
    const w = page.width_dots;
    const h = page.height_dots;
    const canvas = document.createElement("canvas");
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, w, h);
    for (const box of boxes) {
        drawBox(ctx, box);
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
