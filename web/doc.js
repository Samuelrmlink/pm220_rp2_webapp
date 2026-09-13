/** pm220 label document: one JSON file. */

import { geom } from "./raster.js";

export const FORMAT = "pm220-label";
export const VERSION = 2;

function adoptLegacyCorners(obj) {
    if (obj.x == null && obj.x1 != null) {
        obj.x = obj.x1;
        obj.y = obj.y1;
        obj.width = obj.x2 - obj.x1 + 1;
        obj.height = obj.y2 - obj.y1 + 1;
    }
}

function aabbOriginToCenter(obj) {
    const w = Math.max(1, Number(obj.width) || 1);
    const h = Math.max(1, Number(obj.height) || 1);
    obj.x = Math.round((Number(obj.x) || 0) + w / 2);
    obj.y = Math.round((Number(obj.y) || 0) + h / 2);
}

export function migrateObject(raw, docVersion = VERSION) {
    const obj = { ...raw };
    adoptLegacyCorners(obj);
    if ((Number(docVersion) || 1) < 2) {
        aabbOriginToCenter(obj);
    }
    geom(obj);
    delete obj.x1;
    delete obj.y1;
    delete obj.x2;
    delete obj.y2;
    if (!obj.type) {
        obj.type = "text";
    }
    if (obj.type === "text" && obj.pristine !== true) {
        obj.pristine = false;
    }
    obj.ignoreSafe = !!obj.ignoreSafe;
    return obj;
}

export function toDocument(page, objects) {
    return {
        format: FORMAT,
        version: VERSION,
        media: {
            width_dots: page.width_dots,
            height_dots: page.height_dots,
            width_bytes: page.width_bytes,
            dpi: page.dpi || 203,
            width_mm: page.width_mm || 50,
            height_mm: page.height_mm || 30,
        },
        objects: objects.map((o) => {
            const copy = migrateObject(o, VERSION);
            delete copy._pending;
            return copy;
        }),
    };
}

export function fromDocument(data) {
    if (!data || (data.format && data.format !== FORMAT)) {
        throw new Error("not a PM220 label file");
    }
    const list = data.objects || data;
    if (!Array.isArray(list)) {
        throw new Error("label file has no objects list");
    }
    const ver = Array.isArray(data) ? 1 : (Number(data.version) || 1);
    return list.map((o) => migrateObject(o, ver));
}

export function downloadDocument(doc, name) {
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name || "label.pm220.json";
    a.rel = "noopener";
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
        URL.revokeObjectURL(a.href);
        a.remove();
    }, 1000);
}

export function readLabelFile(file) {
    return file.text().then((text) => {
        const data = JSON.parse(text);
        return fromDocument(data);
    });
}
