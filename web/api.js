/** Pico HTTP API helpers. Base URL from ?api=, else http://pm220.local */

export function apiBase() {
    const q = new URLSearchParams(location.search).get("api");
    if (q) {
        return q.replace(/\/+$/, "");
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
