/** Pico HTTP API helpers. ?api= overrides; same-origin when served from the Pico. */

export function apiBase() {
    const q = new URLSearchParams(location.search).get("api");
    if (q) {
        return q.replace(/\/+$/, "");
    }
    const host = location.hostname;
    if (host && host !== "localhost" && host !== "127.0.0.1") {
        return "";
    }
    return "http://pm220.local";
}

async function getJson(path) {
    const r = await fetch(apiBase() + path);
    if (!r.ok) {
        throw new Error(`${path} ${r.status}`);
    }
    return r.json();
}

export function fetchStatus() {
    return getJson("/api/status");
}

export function fetchMedia() {
    return getJson("/api/media");
}

export async function postPrint(packed) {
    const r = await fetch(apiBase() + "/api/print", {
        method: "POST",
        headers: { "Content-Type": "application/octet-stream" },
        body: packed,
    });
    const text = await r.text();
    let json;
    try {
        json = JSON.parse(text);
    } catch {
        json = { ok: false, error: text || r.statusText };
    }
    if (!r.ok) {
        throw new Error(json.error || `${r.status}`);
    }
    return json;
}

async function fsJson(path, opts) {
    const r = await fetch(apiBase() + path, opts);
    const text = await r.text();
    let json;
    try {
        json = JSON.parse(text);
    } catch {
        json = { ok: false, error: text || r.statusText };
    }
    if (!r.ok) {
        throw new Error(json.error || `${path} ${r.status}`);
    }
    return json;
}

export function listFs(dir) {
    const path = dir ? `/api/fs/${dir}` : "/api/fs";
    return fsJson(path);
}

export async function getFsJson(name) {
    const r = await fetch(apiBase() + "/api/fs/" + name);
    if (!r.ok) {
        const text = await r.text();
        throw new Error(text || `${name} ${r.status}`);
    }
    return r.json();
}

export async function putFs(name, body) {
    return fsJson("/api/fs/" + name, {
        method: "PUT",
        headers: { "Content-Type": "application/octet-stream" },
        body,
    });
}

export function deleteFs(name) {
    return fsJson("/api/fs/" + name, { method: "DELETE" });
}

export function renameFs(from, to) {
    return fsJson("/api/fs/rename", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ from, to }),
    });
}

export async function gzipUtf8(text) {
    if (typeof CompressionStream !== "function") {
        return null;
    }
    const stream = new Blob([text]).stream().pipeThrough(new CompressionStream("gzip"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}

export async function downloadFsBlob(name, filename) {
    const r = await fetch(apiBase() + "/api/fs/" + name);
    if (!r.ok) {
        throw new Error(`${name} ${r.status}`);
    }
    const blob = await r.blob();
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = filename;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}
