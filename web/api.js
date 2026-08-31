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
