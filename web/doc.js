/** pm220 label document: one JSON file. */

import { geom } from "./raster.js";

export const FORMAT = "pm220-label";
export const VERSION = 1;

export function migrateObject(raw) {
    const obj = { ...raw };
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
            const copy = migrateObject(o);
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
    return list.map(migrateObject);
}

export function downloadDocument(doc, name) {
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name || "label.pm220.json";
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

export function readLabelFile(file) {
    return file.text().then((text) => {
        const data = JSON.parse(text);
        return fromDocument(data);
    });
}
