/** Axis-aligned objects on the 384×240 label canvas. */

import { boxOverflows, geom } from "./raster.js";
import { migrateObject } from "./doc.js";

const MIN = 8;

let nextId = 1;

function place(page, safe, w, h) {
    const pad = 16;
    const x = (safe && safe.x0 != null ? safe.x0 : 32) + pad;
    const y = (safe && safe.y0 != null ? safe.y0 : 32) + pad;
    return {
        x,
        y,
        width: Math.min(w, page.width_dots - x),
        height: Math.min(h, page.height_dots - y),
        rotate: 0,
    };
}

export function defaultText(page, safe) {
    return {
        id: nextId++,
        type: "text",
        text: "Text",
        font: "Arial",
        size: 24,
        autoSize: true,
        bold: false,
        italic: false,
        underline: false,
        align: "left",
        valign: "top",
        wrap: true,
        pristine: true,
        ...place(page, safe, 220, 72),
    };
}

export function defaultQr(page, safe) {
    return {
        id: nextId++,
        type: "qr",
        payload: "https://",
        ecc: "M",
        pristine: true,
        ...place(page, safe, 80, 80),
    };
}

export function defaultBarcode(page, safe) {
    return {
        id: nextId++,
        type: "barcode1d",
        symbology: "code128",
        payload: "HELLO",
        showText: true,
        pristine: true,
        ...place(page, safe, 200, 48),
    };
}

export function defaultImage(page, safe, png) {
    return {
        id: nextId++,
        type: "image",
        png: png || "",
        dither: true,
        ...place(page, safe, 120, 120),
    };
}

function clampBox(box, page) {
    geom(box);
    let x = Math.round(box.x);
    let y = Math.round(box.y);
    let w = Math.round(box.width);
    let h = Math.round(box.height);
    if (w < MIN) {
        w = MIN;
    }
    if (h < MIN) {
        h = MIN;
    }
    if (w > page.width_dots) {
        w = page.width_dots;
    }
    if (h > page.height_dots) {
        h = page.height_dots;
    }
    x = Math.max(0, Math.min(page.width_dots - w, x));
    y = Math.max(0, Math.min(page.height_dots - h, y));
    box.x = x;
    box.y = y;
    box.width = w;
    box.height = h;
    delete box.x1;
    delete box.y1;
    delete box.x2;
    delete box.y2;
}

export class Editor {
    constructor(stage, opts) {
        this.stage = stage;
        this.page = opts.page;
        this.safe = opts.safe;
        this.scale = opts.scale || 2;
        this.onChange = opts.onChange || (() => {});
        this.onSelect = opts.onSelect || (() => {});
        this.boxes = [];
        this.selectedId = null;
        this.drag = null;
        this.overlay = document.createElement("div");
        this.overlay.className = "box-layer";
        stage.appendChild(this.overlay);
        this.boundMove = (e) => this.onMove(e);
        this.boundUp = (e) => this.onUp(e);
        window.addEventListener("pointermove", this.boundMove);
        window.addEventListener("pointerup", this.boundUp);
        window.addEventListener("pointercancel", this.boundUp);
        stage.addEventListener("pointerdown", (e) => {
            if (e.target === stage || e.target.classList.contains("box-layer") ||
                e.target.classList.contains("safe-rect") || e.target.classList.contains("crop-band") ||
                e.target.tagName === "CANVAS") {
                this.select(null);
            }
        });
    }

    setPage(page, safe) {
        this.page = page;
        this.safe = safe;
        this.syncDom();
        this.onChange();
    }

    addBox(box) {
        if (!box) {
            box = defaultText(this.page, this.safe);
        }
        if (!box.id) {
            box.id = nextId++;
        } else {
            nextId = Math.max(nextId, box.id + 1);
        }
        clampBox(box, this.page);
        this.boxes.push(box);
        this.select(box.id, { focus: true });
        return box;
    }

    removeSelected() {
        if (this.selectedId == null) {
            return;
        }
        this.boxes = this.boxes.filter((b) => b.id !== this.selectedId);
        this.selectedId = null;
        this.syncDom();
        this.onChange();
    }

    selected() {
        return this.boxes.find((b) => b.id === this.selectedId) || null;
    }

    select(id, opts) {
        this.selectedId = id;
        this.syncDom();
        this.onChange();
        if (opts && opts.focus && id != null) {
            this.onSelect(this.selected());
        }
    }

    updateSelected(patch) {
        const box = this.selected();
        if (!box) {
            return;
        }
        Object.assign(box, patch);
        clampBox(box, this.page);
        this.syncDom();
        this.onChange();
    }

    load(boxes) {
        this.boxes = [];
        for (const b of boxes || []) {
            const copy = migrateObject(b);
            if (!copy.id) {
                copy.id = nextId++;
            }
            nextId = Math.max(nextId, copy.id + 1);
            clampBox(copy, this.page);
            this.boxes.push(copy);
        }
        this.selectedId = null;
        this.syncDom();
        this.onChange();
    }

    syncDom() {
        const s = this.scale;
        this.overlay.replaceChildren();
        for (const box of this.boxes) {
            const g = geom(box);
            const el = document.createElement("div");
            el.className = "tbox";
            el.dataset.id = String(box.id);
            if (box.type === "text" && !box.wrap && !box.autoSize) {
                el.classList.add("clip");
            }
            if (boxOverflows(box, this.safe)) {
                el.classList.add("overflow");
            }
            if (box.id === this.selectedId) {
                el.classList.add("selected");
            }
            el.style.left = `${g.x * s}px`;
            el.style.top = `${g.y * s}px`;
            el.style.width = `${g.width * s}px`;
            el.style.height = `${g.height * s}px`;
            el.style.zIndex = box.id === this.selectedId ? "2" : "1";
            el.addEventListener("pointerdown", (e) => this.onBoxDown(e, box, "move"));
            if (box.id === this.selectedId) {
                for (const corner of ["nw", "ne", "sw", "se"]) {
                    const h = document.createElement("div");
                    h.className = `handle ${corner}`;
                    h.addEventListener("pointerdown", (e) => this.onBoxDown(e, box, corner));
                    el.appendChild(h);
                }
            }
            this.overlay.appendChild(el);
        }
    }

    onBoxDown(e, box, mode) {
        e.stopPropagation();
        e.preventDefault();
        this.select(box.id);
        const g = geom(box);
        this.drag = {
            id: box.id,
            mode,
            x: g.x,
            y: g.y,
            width: g.width,
            height: g.height,
            px: e.clientX,
            py: e.clientY,
            s: this.scale,
        };
        e.target.setPointerCapture?.(e.pointerId);
    }

    onMove(e) {
        if (!this.drag) {
            return;
        }
        const box = this.boxes.find((b) => b.id === this.drag.id);
        if (!box) {
            return;
        }
        const dx = Math.round((e.clientX - this.drag.px) / this.drag.s);
        const dy = Math.round((e.clientY - this.drag.py) / this.drag.s);
        const d = this.drag;
        if (d.mode === "move") {
            box.x = Math.max(0, Math.min(this.page.width_dots - d.width, d.x + dx));
            box.y = Math.max(0, Math.min(this.page.height_dots - d.height, d.y + dy));
            box.width = d.width;
            box.height = d.height;
        } else {
            let x1 = d.x;
            let y1 = d.y;
            let x2 = d.x + d.width - 1;
            let y2 = d.y + d.height - 1;
            if (d.mode.indexOf("w") >= 0) {
                x1 = d.x + dx;
            }
            if (d.mode.indexOf("e") >= 0) {
                x2 = d.x + d.width - 1 + dx;
            }
            if (d.mode.indexOf("n") >= 0) {
                y1 = d.y + dy;
            }
            if (d.mode.indexOf("s") >= 0) {
                y2 = d.y + d.height - 1 + dy;
            }
            if (x2 < x1) {
                const t = x1; x1 = x2; x2 = t;
            }
            if (y2 < y1) {
                const t = y1; y1 = y2; y2 = t;
            }
            box.x = x1;
            box.y = y1;
            box.width = x2 - x1 + 1;
            box.height = y2 - y1 + 1;
            clampBox(box, this.page);
        }
        this.syncDom();
        this.onChange();
    }

    onUp(e) {
        if (!this.drag) {
            return;
        }
        const d = this.drag;
        const moved = e ? Math.hypot(e.clientX - d.px, e.clientY - d.py) : 0;
        this.drag = null;
        if (d.mode === "move" && moved < 6) {
            this.onSelect(this.selected());
        }
    }
}
