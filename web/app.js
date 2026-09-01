import { apiBase, fetchMedia, fetchStatus, postPrint } from "./api.js";
import { Editor, defaultBarcode, defaultImage, defaultQr, defaultText } from "./editor.js";
import { boxOverflows, rasterize } from "./raster.js";
import { fromDocument, toDocument } from "./doc.js";
import { bindPicker } from "./files.js";
import { bindWifiSettings } from "./wifi.js";
import { code128Error } from "./barcode.js";
import { qrError } from "./qr.js";
import { importImageFile } from "./image.js";
import "./barcode.js";
import "./qr.js";
import "./image.js";

const SCALE_MAX = 2;
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
const INT_RE = /^-?\d+$/;

const VIEWPORT_LOCK =
    "width=device-width, initial-scale=1, minimum-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover";

function lockViewport() {
    const meta = document.querySelector('meta[name="viewport"]');
    if (!meta) {
        return;
    }
    meta.setAttribute(
        "content",
        "width=device-width, initial-scale=1, minimum-scale=1, maximum-scale=5, user-scalable=yes",
    );
    requestAnimationFrame(() => {
        meta.setAttribute("content", VIEWPORT_LOCK);
    });
}

lockViewport();
window.addEventListener("pageshow", lockViewport);
window.addEventListener("orientationchange", () => setTimeout(lockViewport, 50));

for (const ev of ["gesturestart", "gesturechange", "gestureend"]) {
    document.addEventListener(ev, (e) => e.preventDefault(), { passive: false });
}

document.addEventListener("touchstart", (e) => {
    if (e.touches.length > 1) {
        e.preventDefault();
    }
}, { passive: false });

document.addEventListener("touchmove", (e) => {
    if (e.touches.length > 1 || (typeof e.scale === "number" && e.scale !== 1)) {
        e.preventDefault();
    }
}, { passive: false });

let lastTapAt = 0;
document.addEventListener("touchend", (e) => {
    if (e.target && e.target.closest && e.target.closest("input, textarea, select, button, a")) {
        lastTapAt = Date.now();
        return;
    }
    const now = Date.now();
    if (now - lastTapAt < 350) {
        e.preventDefault();
    }
    lastTapAt = now;
}, { passive: false });

function setField(id, value) {
    const el = $(id);
    if (document.activeElement === el) {
        return;
    }
    el.value = value;
}

function parseIntField(id) {
    const el = $(id);
    const raw = String(el.value || "").trim();
    if (!INT_RE.test(raw)) {
        return null;
    }
    const n = Number(raw);
    return Number.isFinite(n) ? n : null;
}

function isNumField(el) {
    return el && el.tagName === "INPUT" && el.getAttribute("inputmode") === "numeric";
}

function numBounds(el) {
    switch (el.id) {
        case "size":
            return [4, 240];
        case "bc-font":
            return [4, 72];
        case "bc-off":
            return [-240, 240];
        case "ox":
            return [0, page.width_dots];
        case "oy":
            return [0, page.height_dots];
        case "ow":
            return [8, page.width_dots];
        case "oh":
            return [8, page.height_dots];
        default:
            return [null, null];
    }
}

function stepNumField(el, delta) {
    if (!el || el.disabled || !isNumField(el)) {
        return;
    }
    let n = parseIntField(el.id);
    if (n == null) {
        n = 0;
    }
    n += delta;
    const [lo, hi] = numBounds(el);
    if (lo != null && n < lo) {
        n = lo;
    }
    if (hi != null && n > hi) {
        n = hi;
    }
    el.value = String(n);
    el.dispatchEvent(new Event("input", { bubbles: true }));
}

const preview = $("preview");
const pctx = preview.getContext("2d", { willReadFrequently: true });
pctx.imageSmoothingEnabled = false;

let page = FALLBACK_PAGE;
let packed = new Uint8Array(FALLBACK_PAGE.bitmap_bytes);
packed.fill(0xff);

const editor = new Editor($("stage"), {
    page,
    safe: page.safe_rect,
    scale: SCALE_MAX,
    onChange: () => {
        paint();
        fillForm();
        saveScratch();
        markNudgeFocus();
    },
    onSelect: (box) => focusPayload(box),
});

function viewScale() {
    const wrap = $("stage-wrap");
    if (!wrap || wrap.clientWidth < 32) {
        return SCALE_MAX;
    }
    const cs = getComputedStyle(wrap);
    const padX = parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight);
    const padY = parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
    const availW = Math.max(64, wrap.clientWidth - padX);
    const stacked = window.matchMedia("(max-width: 900px)").matches;
    let availH;
    if (stacked) {
        const headerH = document.querySelector("header").offsetHeight;
        availH = Math.max(80, window.innerHeight - headerH - padY - 72);
    } else {
        availH = Math.max(80, wrap.clientHeight - padY);
    }
    const s = Math.min(availW / page.width_dots, availH / page.height_dots);
    return Math.min(SCALE_MAX, Math.max(0.5, s));
}

function layoutGuides() {
    const s = viewScale();
    editor.scale = s;
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
    editor.syncDom();
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
        if (box) {
            setTimeout(() => $("nudge-pad").focus(), 0);
        }
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

function isNudgeFocused() {
    const pad = $("nudge-pad");
    const el = document.activeElement;
    return el === pad || (pad && pad.contains(el));
}

function markNudgeFocus() {
    const on = isNudgeFocused();
    const boxEl = document.querySelector(".tbox.selected");
    if (boxEl) {
        boxEl.classList.toggle("nudge", on);
    }
}

function nudgeSelected(dx, dy) {
    const box = editor.selected();
    if (!box) {
        return;
    }
    editor.updateSelected({ x: box.x + dx, y: box.y + dy });
}

function resizeSelected(dw, dh) {
    const box = editor.selected();
    if (!box) {
        return;
    }
    if (box.type === "qr") {
        const d = dw !== 0 ? dw : dh;
        editor.updateSelected({ width: box.width + d, height: box.height + d });
        return;
    }
    editor.updateSelected({ width: box.width + dw, height: box.height + dh });
}

function isShownControl(el) {
    if (!el || el.disabled || el.tabIndex < 0) {
        return false;
    }
    if (el.closest("[hidden]")) {
        return false;
    }
    const st = getComputedStyle(el);
    if (st.display === "none" || st.visibility === "hidden") {
        return false;
    }
    return true;
}

function editorTabOrder(box) {
    const pad = $("nudge-pad");
    const pay = payloadEl(box);
    const skip = new Set(["font", "del", "pane-close"]);
    const rest = [];
    const root = $("editor-pane");
    for (const el of root.querySelectorAll("input, select, textarea, button, [tabindex]")) {
        if (el === pad || el === pay || skip.has(el.id) || el.classList.contains("nudge-btn")) {
            continue;
        }
        if (!isShownControl(el)) {
            continue;
        }
        rest.push(el);
    }
    const order = [];
    if (pay) {
        order.push(pay);
    }
    order.push(pad);
    order.push(...rest);
    return order;
}

function moveEditorFocus(dir) {
    const box = editor.selected();
    if (!box) {
        return false;
    }
    const order = editorTabOrder(box);
    if (!order.length) {
        return false;
    }
    const active = document.activeElement;
    let i = order.findIndex((el) => el === active || el.contains(active));
    if (i < 0) {
        i = dir > 0 ? -1 : 0;
    }
    const next = order[(i + dir + order.length) % order.length];
    if (next) {
        next.focus();
        markNudgeFocus();
        return true;
    }
    return false;
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
    $("pane-close").hidden = !box;
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
    setField("ox", box.x);
    setField("oy", box.y);
    setField("ow", box.width);
    setField("oh", box.height);
    if (box.type === "text") {
        $("size").disabled = !!box.autoSize;
        setField("text", box.text);
        $("font").value = box.font || "Arial";
        setField("size", box.size);
        $("autoSize").checked = !!box.autoSize;
        $("wrap").checked = !!box.wrap;
        $("bold").checked = !!box.bold;
        $("italic").checked = !!box.italic;
        $("underline").checked = !!box.underline;
        $("align").value = box.align;
        $("valign").value = box.valign;
    } else if (box.type === "qr") {
        setField("qr-payload", box.payload || "");
        $("qr-pixel").checked = !!box.pixelPerfect;
        $("qr-ecc").value = box.ecc || "M";
    } else if (box.type === "barcode1d") {
        setField("bc-payload", box.payload || "");
        $("bc-sym").value = box.symbology || "code128";
        $("bc-text").checked = box.showText !== false;
        setField("bc-font", box.textSize ?? 12);
        setField("bc-off", box.textOffset ?? 7);
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
    };
    const x = parseIntField("ox");
    const y = parseIntField("oy");
    const w = parseIntField("ow");
    const h = parseIntField("oh");
    if (x != null) {
        patch.x = x;
    }
    if (y != null) {
        patch.y = y;
    }
    if (w != null) {
        patch.width = w;
    }
    if (h != null) {
        patch.height = h;
    }
    if (box.type === "text") {
        Object.assign(patch, {
            text: $("text").value,
            font: $("font").value,
            autoSize: $("autoSize").checked,
            wrap: $("wrap").checked,
            bold: $("bold").checked,
            italic: $("italic").checked,
            underline: $("underline").checked,
            align: $("align").value,
            valign: $("valign").value,
        });
        const size = parseIntField("size");
        if (size != null) {
            patch.size = size;
        }
    } else if (box.type === "qr") {
        Object.assign(patch, {
            payload: $("qr-payload").value,
            ecc: $("qr-ecc").value,
            pixelPerfect: $("qr-pixel").checked,
        });
        if (patch.width != null && patch.width !== patch.height) {
            patch.height = patch.width;
        }
    } else if (box.type === "barcode1d") {
        Object.assign(patch, {
            payload: $("bc-payload").value,
            symbology: $("bc-sym").value,
            showText: $("bc-text").checked,
        });
        const textSize = parseIntField("bc-font");
        const textOffset = parseIntField("bc-off");
        if (textSize != null) {
            patch.textSize = textSize;
        }
        if (textOffset != null) {
            patch.textOffset = textOffset;
        }
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

$("editor-pane").addEventListener("focusout", () => {
    setTimeout(fillForm, 0);
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
$("pane-close").addEventListener("click", () => editor.select(null));
$("obj-next").addEventListener("click", () => editor.cyclePreview(1));
$("obj-prev").addEventListener("click", () => editor.cyclePreview(-1));
$("obj-select").addEventListener("click", () => editor.confirmPreview());

let picoName = "";
const picker = bindPicker({
    getDoc: () => toDocument(page, editor.boxes),
    loadDoc: (objects) => editor.load(objects),
    picoName: () => picoName,
    setPicoName: (name) => { picoName = name; },
    setStatus,
});
const wifiUi = bindWifiSettings({ setStatus });

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
    if (picker.isOpen() || wifiUi.isOpen()) {
        return;
    }
    const box = editor.selected();
    if (box && e.key === "Tab") {
        e.preventDefault();
        moveEditorFocus(e.shiftKey ? -1 : 1);
        return;
    }
    const typing = e.target && (
        e.target.tagName === "TEXTAREA" ||
        (e.target.tagName === "INPUT" && !isNumField(e.target))
    );
    if (!typing && (e.key === "n" || e.key === "N")) {
        e.preventDefault();
        if (box) {
            moveEditorFocus(e.key === "N" ? -1 : 1);
        } else {
            editor.cyclePreview(e.key === "N" ? -1 : 1);
        }
        return;
    }
    if (!box && (e.key === " " || e.key === "Enter")) {
        const tag = e.target && e.target.tagName;
        if (tag !== "BUTTON" && tag !== "INPUT" && tag !== "TEXTAREA" && tag !== "SELECT") {
            e.preventDefault();
            editor.confirmPreview();
            return;
        }
    }
    const inField = e.target && (e.target.tagName === "TEXTAREA" || e.target.tagName === "INPUT");
    if (e.key === "Delete" && box) {
        e.preventDefault();
        editor.removeSelected();
        return;
    }
    if (inField) {
        if (isNumField(e.target) && !e.target.disabled) {
            const step = e.shiftKey ? 8 : 1;
            let d = 0;
            if (e.key === "ArrowUp" || e.key === "k") {
                d = step;
            } else if (e.key === "ArrowDown" || e.key === "j") {
                d = -step;
            } else if (e.key === "K") {
                d = 8;
            } else if (e.key === "J") {
                d = -8;
            }
            if (d) {
                e.preventDefault();
                stepNumField(e.target, d);
                return;
            }
        }
        if (e.key === "Escape") {
            e.target.blur();
            editor.select(null);
        }
        return;
    }
    if (isNudgeFocused() && box) {
        const keys = {
            ArrowLeft: [-1, 0],
            ArrowRight: [1, 0],
            ArrowUp: [0, -1],
            ArrowDown: [0, 1],
            h: [-1, 0],
            l: [1, 0],
            k: [0, -1],
            j: [0, 1],
            H: [-1, 0],
            L: [1, 0],
            K: [0, -1],
            J: [0, 1],
        };
        const delta = keys[e.key];
        if (delta) {
            e.preventDefault();
            if (e.shiftKey) {
                resizeSelected(delta[0], delta[1]);
            } else {
                nudgeSelected(delta[0], delta[1]);
            }
            return;
        }
        if (e.key === "Backspace") {
            e.preventDefault();
            return;
        }
    }
    if (e.key === "Backspace" && box) {
        e.preventDefault();
        editor.removeSelected();
    }
    if (e.key === "Escape") {
        editor.select(null);
    }
});

$("nudge-pad").addEventListener("focusin", markNudgeFocus);
$("nudge-pad").addEventListener("focusout", () => setTimeout(markNudgeFocus, 0));

{
    let holdWait = 0;
    let holdRep = 0;
    const stopHold = () => {
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = 0;
        holdRep = 0;
    };
    $("nudge-pad").addEventListener("pointerdown", (e) => {
        const btn = e.target.closest(".nudge-btn");
        if (!btn || !editor.selected()) {
            return;
        }
        e.preventDefault();
        const dx = Number(btn.dataset.dx) || 0;
        const dy = Number(btn.dataset.dy) || 0;
        $("nudge-pad").focus();
        nudgeSelected(dx, dy);
        stopHold();
        holdWait = setTimeout(() => {
            holdRep = setInterval(() => nudgeSelected(dx, dy), 50);
        }, 300);
        btn.setPointerCapture?.(e.pointerId);
    });
    $("nudge-pad").addEventListener("pointerup", stopHold);
    $("nudge-pad").addEventListener("pointercancel", stopHold);
    $("nudge-pad").addEventListener("lostpointercapture", stopHold);
}

{
    let holdWait = 0;
    let holdRep = 0;
    const stopHold = () => {
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = 0;
        holdRep = 0;
    };
    $("resize-pad").addEventListener("pointerdown", (e) => {
        const btn = e.target.closest(".nudge-btn");
        if (!btn || !editor.selected()) {
            return;
        }
        e.preventDefault();
        const dw = Number(btn.dataset.dw) || 0;
        const dh = Number(btn.dataset.dh) || 0;
        resizeSelected(dw, dh);
        stopHold();
        holdWait = setTimeout(() => {
            holdRep = setInterval(() => resizeSelected(dw, dh), 50);
        }, 300);
        btn.setPointerCapture?.(e.pointerId);
    });
    $("resize-pad").addEventListener("pointerup", stopHold);
    $("resize-pad").addEventListener("pointercancel", stopHold);
    $("resize-pad").addEventListener("lostpointercapture", stopHold);
}

function wrapNumSpins() {
    for (const el of document.querySelectorAll("aside input[inputmode='numeric']")) {
        if (el.parentElement && el.parentElement.classList.contains("num-spin")) {
            continue;
        }
        const wrap = document.createElement("div");
        wrap.className = "num-spin";
        el.replaceWith(wrap);
        wrap.appendChild(el);
        const col = document.createElement("div");
        col.className = "num-spin-btns";
        const up = document.createElement("button");
        up.type = "button";
        up.tabIndex = -1;
        up.className = "num-spin-up";
        up.setAttribute("aria-label", "Increase");
        up.textContent = "▲";
        const down = document.createElement("button");
        down.type = "button";
        down.tabIndex = -1;
        down.className = "num-spin-down";
        down.setAttribute("aria-label", "Decrease");
        down.textContent = "▼";
        col.append(up, down);
        wrap.appendChild(col);
    }
    let holdWait = 0;
    let holdRep = 0;
    const stopHold = () => {
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = 0;
        holdRep = 0;
    };
    document.querySelector("aside").addEventListener("pointerdown", (e) => {
        const btn = e.target.closest(".num-spin-btns button");
        if (!btn) {
            return;
        }
        const input = btn.closest(".num-spin").querySelector("input");
        if (!input || input.disabled) {
            return;
        }
        e.preventDefault();
        const delta = btn.classList.contains("num-spin-up") ? 1 : -1;
        stepNumField(input, delta);
        stopHold();
        holdWait = setTimeout(() => {
            holdRep = setInterval(() => stepNumField(input, delta), 50);
        }, 300);
        btn.setPointerCapture?.(e.pointerId);
    });
    document.querySelector("aside").addEventListener("pointerup", stopHold);
    document.querySelector("aside").addEventListener("pointercancel", stopHold);
    document.querySelector("aside").addEventListener("lostpointercapture", stopHold);
}

wrapNumSpins();

function setStatus(msg, kind) {
    const el = $("status");
    el.textContent = msg;
    el.className = kind || "";
}

let statusTimer = 0;
let statusBusy = false;
let printerUp = false;

function scheduleStatus() {
    clearTimeout(statusTimer);
    let ms = 10000;
    if (document.hidden) {
        ms = 20000;
    } else if (typeof wifiUi !== "undefined" && wifiUi.isOpen()) {
        ms = 15000;
    } else if (!printerUp) {
        ms = 5000;
    }
    statusTimer = setTimeout(() => refreshStatus(), ms);
}

async function refreshStatus() {
    if (statusBusy) {
        return null;
    }
    statusBusy = true;
    try {
        const st = await fetchStatus();
        printerUp = !!st.printer_connected;
        const bit = printerUp ? "printer connected" : "no printer";
        setStatus(`${bit} · ${apiBase()}`, printerUp ? "ok" : "");
        $("print").disabled = !printerUp;
        return st;
    } catch (err) {
        printerUp = false;
        setStatus(`API unreachable (${apiBase()})`, "err");
        $("print").disabled = true;
        return null;
    } finally {
        statusBusy = false;
        scheduleStatus();
    }
}

document.addEventListener("visibilitychange", () => {
    scheduleStatus();
});

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
    layoutGuides();
    new ResizeObserver(() => layoutGuides()).observe($("stage-wrap"));
    window.addEventListener("resize", () => layoutGuides());
}

boot();
