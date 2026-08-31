import { apiBase, fetchMedia, fetchStatus, postPrint } from "./api.js";
import { Editor, defaultBarcode, defaultImage, defaultQr, defaultText } from "./editor.js";
import { boxOverflows, rasterize } from "./raster.js";
import { fromDocument, toDocument } from "./doc.js";
import { bindPicker } from "./files.js";
import { code128Error } from "./barcode.js";
import { qrError } from "./qr.js";
import { importImageFile } from "./image.js";
import "./barcode.js";
import "./qr.js";
import "./image.js";

const SCALE = 2;
const STORE = "pm220-editor-v1";
const FALLBACK_PAGE = {
    width_dots: 384,
    height_dots: 240,
    width_bytes: 48,
    bitmap_bytes: 11520,
    dpi: 203,
    width_mm: 50,
    height_mm: 30,
    safe_rect: { x0: 32, y0: 32, x1: 351, y1: 207 },
    after_offset: { origin_x: 0, origin_y: 24, width_dots: 384, height_dots: 216 },
};

const TITLES = {
    text: "Text box",
    qr: "QR code",
    barcode1d: "Barcode",
    image: "Image",
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
        saveScratch();
    },
    onSelect: (box) => focusPayload(box),
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
    const r = rasterize(page, editor.boxes, () => paint());
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

function payloadEl(box) {
    if (!box) {
        return null;
    }
    if (box.type === "qr") {
        return $("qr-payload");
    }
    if (box.type === "barcode1d") {
        return $("bc-payload");
    }
    if (box.type === "text") {
        return $("text");
    }
    return null;
}

function focusPayload(box) {
    const el = payloadEl(box);
    if (!el) {
        return;
    }
    setTimeout(() => {
        el.focus();
        if (box.pristine) {
            el.select();
        } else {
            const n = el.value.length;
            el.setSelectionRange(n, n);
        }
    }, 0);
}

function payloadWarning(box) {
    if (!box) {
        return "";
    }
    if (box.type === "qr") {
        return qrError(box.payload);
    }
    if (box.type === "barcode1d") {
        return code128Error(box.payload);
    }
    return "";
}

function fillForm() {
    const box = editor.selected();
    $("add-pane").hidden = !!box;
    $("editor-pane").hidden = !box;
    $("del").disabled = !box;
    $("pane-title").textContent = box ? (TITLES[box.type] || box.type) : "Add object";
    const clip = !!(box && box.type === "text" && !box.wrap && !box.autoSize);
    const overflow = !!(box && boxOverflows(box, editor.safe));
    const payload = payloadWarning(box);
    $("warn-clip").hidden = !clip;
    $("warn-overflow").hidden = !overflow;
    $("warn-payload").hidden = !payload;
    $("warn-payload").textContent = payload || "";
    $("warn").hidden = !clip && !overflow && !payload;
    $("form-text").hidden = !box || box.type !== "text";
    $("form-qr").hidden = !box || box.type !== "qr";
    $("form-barcode").hidden = !box || box.type !== "barcode1d";
    $("form-image").hidden = !box || box.type !== "image";
    $("advanced-qr").hidden = !$("advanced").checked || !box || box.type !== "qr";
    if (!box) {
        return;
    }
    $("rotate").value = String(box.rotate || 0);
    $("ox").value = box.x;
    $("oy").value = box.y;
    $("ow").value = box.width;
    $("oh").value = box.height;
    if (box.type === "text") {
        $("size").disabled = !!box.autoSize;
        const el = $("text");
        if (document.activeElement !== el) {
            el.value = box.text;
        }
        $("font").value = box.font || "Arial";
        $("size").value = box.size;
        $("autoSize").checked = !!box.autoSize;
        $("wrap").checked = !!box.wrap;
        $("bold").checked = !!box.bold;
        $("italic").checked = !!box.italic;
        $("underline").checked = !!box.underline;
        $("align").value = box.align;
        $("valign").value = box.valign;
    } else if (box.type === "qr") {
        if (document.activeElement !== $("qr-payload")) {
            $("qr-payload").value = box.payload || "";
        }
        $("qr-pixel").checked = !!box.pixelPerfect;
        $("qr-ecc").value = box.ecc || "M";
    } else if (box.type === "barcode1d") {
        if (document.activeElement !== $("bc-payload")) {
            $("bc-payload").value = box.payload || "";
        }
        $("bc-sym").value = box.symbology || "code128";
        $("bc-text").checked = box.showText !== false;
        $("bc-font").value = box.textSize ?? 12;
        $("bc-off").value = box.textOffset ?? 7;
    } else if (box.type === "image") {
        $("img-dither").checked = box.dither !== false;
    }
}

function readForm() {
    const box = editor.selected();
    if (!box) {
        return {};
    }
    const patch = {
        rotate: Number($("rotate").value),
        x: Number($("ox").value),
        y: Number($("oy").value),
        width: Number($("ow").value),
        height: Number($("oh").value),
    };
    if (box.type === "text") {
        Object.assign(patch, {
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
        });
    } else if (box.type === "qr") {
        Object.assign(patch, {
            payload: $("qr-payload").value,
            ecc: $("qr-ecc").value,
            pixelPerfect: $("qr-pixel").checked,
        });
        if (patch.width !== patch.height) {
            patch.height = patch.width;
        }
    } else if (box.type === "barcode1d") {
        Object.assign(patch, {
            payload: $("bc-payload").value,
            symbology: $("bc-sym").value,
            showText: $("bc-text").checked,
            textSize: Number($("bc-font").value),
            textOffset: Number($("bc-off").value),
        });
    } else if (box.type === "image") {
        patch.dither = $("img-dither").checked;
    }
    return patch;
}

function saveScratch() {
    try {
        localStorage.setItem(STORE, JSON.stringify(toDocument(page, editor.boxes)));
    } catch {
        /* quota */
    }
}

function restore() {
    try {
        const raw = localStorage.getItem(STORE);
        if (raw) {
            editor.load(fromDocument(JSON.parse(raw)));
        }
    } catch {
        /* ignore */
    }
}

function syncAdvanced() {
    const on = $("advanced").checked;
    const box = editor.selected();
    $("coords").hidden = !on;
    $("advanced-qr").hidden = !on || !box || box.type !== "qr";
}

$("editor-pane").addEventListener("input", (e) => {
    const box = editor.selected();
    if (!box) {
        return;
    }
    if (e.target && (e.target.id === "text" || e.target.id === "qr-payload" || e.target.id === "bc-payload")) {
        box.pristine = false;
    }
    if (e.target && e.target.id === "advanced") {
        try {
            localStorage.setItem(STORE + "-advanced", $("advanced").checked ? "1" : "0");
        } catch {
            /* ignore */
        }
        syncAdvanced();
        return;
    }
    editor.updateSelected(readForm());
});

$("add-text").addEventListener("click", () => editor.addBox(defaultText(page, editor.safe)));
$("add-qr").addEventListener("click", () => editor.addBox(defaultQr(page, editor.safe)));
$("add-barcode").addEventListener("click", () => editor.addBox(defaultBarcode(page, editor.safe)));
$("add-image").addEventListener("click", () => $("image-file").click());
$("replace-image").addEventListener("click", () => $("image-file").click());

$("image-file").addEventListener("change", async (e) => {
    const file = e.target.files && e.target.files[0];
    e.target.value = "";
    if (!file) {
        return;
    }
    try {
        const { png } = await importImageFile(file, () => paint());
        const sel = editor.selected();
        if (sel && sel.type === "image") {
            editor.updateSelected({ png });
        } else {
            editor.addBox(defaultImage(page, editor.safe, png));
        }
    } catch (err) {
        setStatus(String(err.message || err), "err");
    }
});

$("del").addEventListener("click", () => editor.removeSelected());

let picoName = "";
const picker = bindPicker({
    getDoc: () => toDocument(page, editor.boxes),
    loadDoc: (objects) => editor.load(objects),
    picoName: () => picoName,
    setPicoName: (name) => { picoName = name; },
    setStatus,
});

$("file").addEventListener("change", async (e) => {
    const file = e.target.files && e.target.files[0];
    e.target.value = "";
    if (!file) {
        return;
    }
    try {
        const text = await file.text();
        editor.load(fromDocument(JSON.parse(text)));
        picoName = file.name.replace(/\.gz$/i, "");
        setStatus(`opened ${picoName}`, "ok");
    } catch (err) {
        setStatus(String(err.message || err), "err");
    }
});

document.addEventListener("keydown", (e) => {
    if (picker.isOpen()) {
        return;
    }
    const inField = e.target && (e.target.tagName === "TEXTAREA" || e.target.tagName === "INPUT");
    if (e.key === "Delete" && editor.selected()) {
        e.preventDefault();
        editor.removeSelected();
        return;
    }
    if (inField) {
        if (e.key === "Escape") {
            e.target.blur();
            editor.select(null);
        }
        return;
    }
    if (e.key === "Backspace" && editor.selected()) {
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
    try {
        $("advanced").checked = localStorage.getItem(STORE + "-advanced") === "1";
    } catch {
        $("advanced").checked = false;
    }
    syncAdvanced();
    layoutGuides();
    restore();
    try {
        const media = await fetchMedia();
        page = {
            width_dots: media.width_dots,
            height_dots: media.height_dots,
            width_bytes: media.width_bytes,
            bitmap_bytes: media.bitmap_bytes,
            dpi: media.dpi,
            width_mm: media.width_mm,
            height_mm: media.height_mm,
            safe_rect: media.safe_rect,
            after_offset: media.after_offset,
        };
        preview.width = page.width_dots;
        preview.height = page.height_dots;
        editor.setPage(page, page.safe_rect);
        layoutGuides();
        paint();
    } catch {
        /* fallback page */
    }
    await refreshStatus();
    setInterval(refreshStatus, 4000);
}

boot();
