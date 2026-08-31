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

function lineHeight(size) {
    return Math.max(1, Math.ceil(size * 1.2));
}

function blockMetrics(ctx, lines, size) {
    const lh = lineHeight(size);
    let blockW = 0;
    for (const line of lines) {
        blockW = Math.max(blockW, ctx.measureText(line).width);
    }
    return { lineH: lh, blockH: lh * Math.max(lines.length, 1), blockW };
}

function layoutFits(ctx, box, size, lw, lh) {
    ctx.font = fontCss(box, size);
    const lines = wrapLines(ctx, box.text, lw, box.wrap);
    const m = blockMetrics(ctx, lines, size);
    if (m.blockH > lh + 0.01) {
        return false;
    }
    if (m.blockW > lw + 0.01) {
        return false;
    }
    return true;
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
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    const lines = wrapLines(ctx, box.text, ls.w, box.wrap);
    const m = blockMetrics(ctx, lines, size);
    let y = 0;
    if (box.valign === "middle") {
        y = (ls.h - m.blockH) / 2;
    } else if (box.valign === "bottom") {
        y = ls.h - m.blockH;
    }
    const ul = Math.max(1, Math.round(size / 12));
    for (const line of lines) {
        const tw = ctx.measureText(line).width;
        let x = 0;
        if (box.align === "center") {
            x = (ls.w - tw) / 2;
        } else if (box.align === "right") {
            x = ls.w - tw;
        }
        ctx.fillText(line, x, y);
        if (box.underline && line) {
            ctx.fillRect(x, y + size + 1, tw, ul);
        }
        y += m.lineH;
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
