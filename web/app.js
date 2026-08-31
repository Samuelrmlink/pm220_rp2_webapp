import { apiBase, fetchMedia, fetchStatus, postPrint } from "./api.js";
import { Editor } from "./editor.js";
import { rasterize } from "./raster.js";

const SCALE = 2;
const STORE = "pm220-editor-v1";
const FALLBACK_PAGE = {
    width_dots: 384,
    height_dots: 240,
    width_bytes: 48,
    bitmap_bytes: 11520,
    safe_rect: { x0: 32, y0: 32, x1: 351, y1: 207 },
    after_offset: { origin_x: 0, origin_y: 24, width_dots: 384, height_dots: 216 },
};

const $ = (id) => document.getElementById(id);
const preview = $("preview");
const pctx = preview.getContext("2d", { willReadFrequently: true });
pctx.imageSmoothingEnabled = false;

let page = FALLBACK_PAGE;
let packed = new Uint8Array(FALLBACK_PAGE.bitmap_bytes);
packed.fill(0xff);

const editor = new Editor($("stage"), {
    page,
    safe: page.safe_rect,
    scale: SCALE,
    onChange: () => {
        paint();
        fillForm();
        save();
    },
});

function layoutGuides() {
    const s = SCALE;
    const stage = $("stage");
    stage.style.width = `${page.width_dots * s}px`;
    stage.style.height = `${page.height_dots * s}px`;
    preview.style.width = `${page.width_dots * s}px`;
    preview.style.height = `${page.height_dots * s}px`;
    const r = page.safe_rect;
    const safe = $("safe");
    safe.style.left = `${r.x0 * s}px`;
    safe.style.top = `${r.y0 * s}px`;
    safe.style.width = `${(r.x1 - r.x0 + 1) * s}px`;
    safe.style.height = `${(r.y1 - r.y0 + 1) * s}px`;
    const cropH = page.height_dots - (page.after_offset?.height_dots ?? page.height_dots);
    const crop = $("crop");
    if (cropH > 0) {
        crop.style.top = `${(page.height_dots - cropH) * s}px`;
        crop.style.height = `${cropH * s}px`;
        crop.hidden = false;
    } else {
        crop.hidden = true;
    }
}

function paint() {
    const r = rasterize(page, editor.boxes);
    packed = r.packed;
    const w = page.width_dots;
    const h = page.height_dots;
    const wb = page.width_bytes;
    const img = pctx.createImageData(w, h);
    const d = img.data;
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const white = packed[y * wb + (x >> 3)] & (0x80 >> (x & 7));
            const v = white ? 255 : 0;
            const i = (y * w + x) * 4;
            d[i] = d[i + 1] = d[i + 2] = v;
            d[i + 3] = 255;
        }
    }
    pctx.putImageData(img, 0, 0);
}

function fillForm() {
    const box = editor.selected();
    $("none").hidden = !!box;
    $("form").hidden = !box;
    $("del").disabled = !box;
    if (!box) {
        return;
    }
    $("size").disabled = !!box.autoSize;
    if ($("form").contains(document.activeElement)) {
        return;
    }
    $("text").value = box.text;
    $("font").value = box.font || "Arial";
    $("size").value = box.size;
    $("autoSize").checked = !!box.autoSize;
    $("wrap").checked = !!box.wrap;
    $("bold").checked = !!box.bold;
    $("italic").checked = !!box.italic;
    $("underline").checked = !!box.underline;
    $("align").value = box.align;
    $("valign").value = box.valign;
    $("rotate").value = String(box.rotate);
    $("x1").value = box.x1;
    $("y1").value = box.y1;
    $("x2").value = box.x2;
    $("y2").value = box.y2;
}

function readForm() {
    return {
        text: $("text").value,
        font: $("font").value,
        size: Number($("size").value),
        autoSize: $("autoSize").checked,
        wrap: $("wrap").checked,
        bold: $("bold").checked,
        italic: $("italic").checked,
        underline: $("underline").checked,
        align: $("align").value,
        valign: $("valign").value,
        rotate: Number($("rotate").value),
        x1: Number($("x1").value),
        y1: Number($("y1").value),
        x2: Number($("x2").value),
        y2: Number($("y2").value),
    };
}

function save() {
    try {
        localStorage.setItem(STORE, JSON.stringify(editor.boxes));
    } catch {
        /* quota / private mode */
    }
}

function restore() {
    try {
        const raw = localStorage.getItem(STORE);
        if (raw) {
            editor.load(JSON.parse(raw));
            return;
        }
    } catch {
        /* ignore */
    }
    editor.addBox();
}

$("form").addEventListener("input", () => {
    editor.updateSelected(readForm());
});

$("add").addEventListener("click", () => editor.addBox());
$("del").addEventListener("click", () => editor.removeSelected());

document.addEventListener("keydown", (e) => {
    if (e.target && (e.target.tagName === "TEXTAREA" || e.target.tagName === "INPUT")) {
        if (e.key === "Escape") {
            e.target.blur();
            editor.select(null);
        }
        return;
    }
    if (e.key === "Delete" || e.key === "Backspace") {
        e.preventDefault();
        editor.removeSelected();
    }
    if (e.key === "Escape") {
        editor.select(null);
    }
});

function setStatus(msg, kind) {
    const el = $("status");
    el.textContent = msg;
    el.className = kind || "";
}

async function refreshStatus() {
    try {
        const st = await fetchStatus();
        const bit = st.printer_connected ? "printer connected" : "no printer";
        setStatus(`${bit} · ${apiBase()}`, st.printer_connected ? "ok" : "");
        $("print").disabled = !st.printer_connected;
        return st;
    } catch (err) {
        setStatus(`API unreachable (${apiBase()})`, "err");
        $("print").disabled = true;
        return null;
    }
}

$("print").addEventListener("click", async () => {
    $("print").disabled = true;
    try {
        paint();
        const res = await postPrint(packed);
        setStatus(`sent ${res.bytes} bytes · ${res.print_width_dots}×${res.print_height_dots} at ${res.origin_x},${res.origin_y}`, "ok");
    } catch (err) {
        setStatus(String(err.message || err), "err");
    } finally {
        $("print").disabled = false;
        refreshStatus();
    }
});

async function boot() {
    layoutGuides();
    restore();
    try {
        const media = await fetchMedia();
        page = {
            width_dots: media.width_dots,
            height_dots: media.height_dots,
            width_bytes: media.width_bytes,
            bitmap_bytes: media.bitmap_bytes,
            safe_rect: media.safe_rect,
            after_offset: media.after_offset,
        };
        preview.width = page.width_dots;
        preview.height = page.height_dots;
        editor.setPage(page, page.safe_rect);
        layoutGuides();
        paint();
    } catch {
        /* fallback page already in use */
    }
    await refreshStatus();
    setInterval(refreshStatus, 4000);
}

boot();
