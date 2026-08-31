/** Axis-aligned text boxes on the 384×240 label canvas. */

import { boxOverflows } from "./raster.js";

const MIN = 8;

let nextId = 1;

export function defaultBox(page, safe) {
    const pad = 16;
    const x1 = (safe && safe.x0 != null ? safe.x0 : 32) + pad;
    const y1 = (safe && safe.y0 != null ? safe.y0 : 32) + pad;
    const x2 = Math.min(page.width_dots - 1, x1 + 220);
    const y2 = Math.min(page.height_dots - 1, y1 + 72);
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
        rotate: 0,
        pristine: true,
        x1, y1, x2, y2,
    };
}

function clampBox(box, page) {
    let x1 = Math.round(box.x1);
    let y1 = Math.round(box.y1);
    let x2 = Math.round(box.x2);
    let y2 = Math.round(box.y2);
    if (x2 < x1) {
        const t = x1; x1 = x2; x2 = t;
    }
    if (y2 < y1) {
        const t = y1; y1 = y2; y2 = t;
    }
    x1 = Math.max(0, Math.min(page.width_dots - MIN, x1));
    y1 = Math.max(0, Math.min(page.height_dots - MIN, y1));
    x2 = Math.max(x1 + MIN - 1, Math.min(page.width_dots - 1, x2));
    y2 = Math.max(y1 + MIN - 1, Math.min(page.height_dots - 1, y2));
    box.x1 = x1;
    box.y1 = y1;
    box.x2 = x2;
    box.y2 = y2;
}

export class Editor {
    /**
     * @param {HTMLElement} stage
     * @param {{ page: object, safe: object, scale: number, onChange: function }} opts
     */
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
            box = defaultBox(this.page, this.safe);
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
        this.selectedId = this.boxes.length ? this.boxes[this.boxes.length - 1].id : null;
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
            const copy = { ...b };
            if (!copy.id) {
                copy.id = nextId++;
            }
            nextId = Math.max(nextId, copy.id + 1);
            if (!copy.type) {
                copy.type = "text";
            }
            if (copy.pristine !== true) {
                copy.pristine = false;
            }
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
            const el = document.createElement("div");
            el.className = "tbox";
            el.dataset.id = String(box.id);
            const overflow = boxOverflows(box, this.safe);
            if (!box.wrap && !box.autoSize) {
                el.classList.add("clip");
            }
            if (overflow) {
                el.classList.add("overflow");
            }
            if (box.id === this.selectedId) {
                el.classList.add("selected");
            }
            el.style.left = `${box.x1 * s}px`;
            el.style.top = `${box.y1 * s}px`;
            el.style.width = `${(box.x2 - box.x1 + 1) * s}px`;
            el.style.height = `${(box.y2 - box.y1 + 1) * s}px`;
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
        const s = this.scale;
        this.drag = {
            id: box.id,
            mode,
            x1: box.x1,
            y1: box.y1,
            x2: box.x2,
            y2: box.y2,
            px: e.clientX,
            py: e.clientY,
            s,
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
            const w = d.x2 - d.x1;
            const h = d.y2 - d.y1;
            const x1 = Math.max(0, Math.min(this.page.width_dots - 1 - w, d.x1 + dx));
            const y1 = Math.max(0, Math.min(this.page.height_dots - 1 - h, d.y1 + dy));
            box.x1 = x1;
            box.y1 = y1;
            box.x2 = x1 + w;
            box.y2 = y1 + h;
        } else {
            box.x1 = d.x1;
            box.y1 = d.y1;
            box.x2 = d.x2;
            box.y2 = d.y2;
            if (d.mode.indexOf("w") >= 0) {
                box.x1 = d.x1 + dx;
            }
            if (d.mode.indexOf("e") >= 0) {
                box.x2 = d.x2 + dx;
            }
            if (d.mode.indexOf("n") >= 0) {
                box.y1 = d.y1 + dy;
            }
            if (d.mode.indexOf("s") >= 0) {
                box.y2 = d.y2 + dy;
            }
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
        const moved = e
            ? Math.hypot(e.clientX - d.px, e.clientY - d.py)
            : 0;
        this.drag = null;
        if (d.mode === "move" && moved < 6) {
            this.onSelect(this.selected());
        }
    }
}
