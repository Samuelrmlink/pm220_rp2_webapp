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
    return "http://192.168.7.1";
}

function withTimeout(userSignal, ms) {
    const ctrl = new AbortController();
    const id = setTimeout(() => ctrl.abort(), ms);
    const onUser = () => ctrl.abort();
    if (userSignal) {
        if (userSignal.aborted) {
            ctrl.abort();
        } else {
            userSignal.addEventListener("abort", onUser, { once: true });
        }
    }
    return {
        signal: ctrl.signal,
        done() {
            clearTimeout(id);
            if (userSignal) {
                userSignal.removeEventListener("abort", onUser);
            }
        },
    };
}

async function apiFetch(path, opts) {
    opts = opts || {};
    const ms = opts.timeout == null ? 8000 : opts.timeout;
    const userSignal = opts.signal;
    const timed = withTimeout(userSignal, ms);
    const url = apiBase() + path + (path.includes("?") ? "&" : "?") + "_=" + Date.now();
    const fetchOpts = { ...opts, cache: "no-store", signal: timed.signal };
    delete fetchOpts.timeout;
    try {
        return await fetch(url, fetchOpts);
    } catch (err) {
        if (err && err.name === "AbortError") {
            if (userSignal && userSignal.aborted) {
                throw err;
            }
            throw new Error("request timed out");
        }
        throw err;
    } finally {
        timed.done();
    }
}

async function getJson(path, opts) {
    const r = await apiFetch(path, opts);
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

export async function postPrintTest() {
    const r = await apiFetch("/api/print/test", { method: "POST" });
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

export async function postPrint(packed) {
    const r = await apiFetch("/api/print", {
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
    const r = await apiFetch(path, opts);
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

function isGzip(bytes) {
    return bytes && bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b;
}

async function gunzipBytes(bytes) {
    if (typeof DecompressionStream !== "function") {
        throw new Error("cannot decompress gzip in this browser");
    }
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}

function fsHttpError(path, r, text) {
    let msg = text || r.statusText || String(r.status);
    try {
        const j = JSON.parse(text);
        if (j && j.error) {
            msg = j.error;
        }
    } catch {
        /* keep raw */
    }
    if (r.status === 409) {
        return new Error(msg === "busy" ? "device busy, try again" : msg);
    }
    return new Error(msg || `${path} ${r.status}`);
}

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function retryableFsError(err) {
    const msg = String((err && err.message) || err);
    return msg.includes("timed out") ||
        msg.includes("Failed to fetch") ||
        msg.includes("network") ||
        msg.includes("device busy");
}

export async function getFsJson(name) {
    const path = "/api/fs/" + name;
    let lastErr;
    for (let i = 0; i < 4; i++) {
        try {
            const r = await apiFetch(path, { timeout: 60000 });
            if (r.status === 409 && i < 3) {
                await sleep(150 * (i + 1));
                continue;
            }
            if (!r.ok) {
                throw fsHttpError(path, r, await r.text());
            }
            const bytes = new Uint8Array(await r.arrayBuffer());
            let raw = bytes;
            if (isGzip(bytes)) {
                raw = await gunzipBytes(bytes);
            }
            const text = new TextDecoder().decode(raw);
            try {
                return JSON.parse(text);
            } catch {
                throw new Error("file is not valid JSON");
            }
        } catch (err) {
            lastErr = err;
            if (!retryableFsError(err) || i === 3) {
                throw err;
            }
            await sleep(200 * (i + 1));
        }
    }
    throw lastErr;
}

export async function putFs(name, body) {
    return fsJson("/api/fs/" + name, {
        method: "PUT",
        timeout: 60000,
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

export function getWifi(opts) {
    return getJson("/api/wifi", opts);
}

export function putWifi(body) {
    return fsJson("/api/wifi", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
    });
}

export function startWifiScan() {
    return fsJson("/api/wifi/scan", { method: "POST" });
}

export function getWifiScan(opts) {
    return getJson("/api/wifi/scan", opts);
}

export function getWifiNetworks(opts) {
    return getJson("/api/wifi/networks", opts);
}

export function putWifiNetwork(ssid, opts) {
    const body = { ssid };
    if (opts && opts.password !== undefined) {
        body.password = opts.password;
    }
    if (opts && opts.newSsid) {
        body.new_ssid = opts.newSsid;
    }
    return fsJson("/api/wifi/networks", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
    });
}

export function connectWifi(ssid, password) {
    const body = { ssid };
    if (password !== undefined) {
        body.password = password;
    }
    return fsJson("/api/wifi/connect", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
    });
}

export function deleteWifiNetwork(ssid) {
    return fsJson("/api/wifi/networks", {
        method: "DELETE",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ssid }),
    });
}

export function wifiForceAp() {
    return fsJson("/api/wifi/ap", { method: "POST" });
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
