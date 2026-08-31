/** Pico LittleFS label picker. */

import {
    deleteFs,
    downloadFsBlob,
    getFsJson,
    gzipUtf8,
    listFs,
    putFs,
    renameFs,
} from "./api.js";
import { downloadDocument, fromDocument, toDocument } from "./doc.js";

const DIR = "labels";
const $ = (id) => document.getElementById(id);

function validSegment(name) {
    return /^[A-Za-z0-9._-]+$/.test(name) && !name.startsWith(".") && name.length <= 64;
}

export function displayName(stored) {
    const base = String(stored || "").split("/").pop();
    return base.endsWith(".gz") ? base.slice(0, -3) : base;
}

export function storedPath(display, gzip) {
    let n = String(display || "").trim();
    n = n.split("/").pop() || "";
    if (!n) {
        throw new Error("enter a file name");
    }
    if (n.endsWith(".gz")) {
        n = n.slice(0, -3);
    }
    if (!n.endsWith(".json")) {
        n += ".json";
    }
    if (gzip) {
        n += ".gz";
    }
    if (!validSegment(n) || `${DIR}/${n}`.length > 64) {
        throw new Error("name must be letters, numbers, . _ -");
    }
    return `${DIR}/${n}`;
}

function formatSize(n) {
    if (n < 1024) {
        return `${n} B`;
    }
    return `${(n / 1024).toFixed(1)} KB`;
}

export function bindPicker({ getDoc, loadDoc, picoName, setPicoName, setStatus }) {
    const overlay = $("picker");
    const listEl = $("picker-list");
    const emptyEl = $("picker-empty");
    const errEl = $("picker-err");
    const nameEl = $("picker-name");
    const saveRow = $("picker-save-row");
    const saveBtn = $("picker-save");
    const pcBtn = $("picker-pc");
    let mode = "open";
    let files = [];

    function showErr(msg) {
        if (!msg) {
            errEl.hidden = true;
            errEl.textContent = "";
            return;
        }
        errEl.hidden = false;
        errEl.textContent = msg;
    }

    function close() {
        overlay.hidden = true;
    }

    async function refresh() {
        showErr("");
        listEl.replaceChildren();
        emptyEl.hidden = true;
        try {
            const listing = await listFs(DIR);
            files = listing.files || [];
        } catch (err) {
            files = [];
            showErr(String(err.message || err));
        }
        emptyEl.hidden = files.length > 0 || !errEl.hidden;
        for (const info of files) {
            listEl.appendChild(rowEl(info));
        }
    }

    function rowEl(info) {
        const li = document.createElement("li");
        const shown = displayName(info.name);
        const nameBtn = document.createElement("button");
        nameBtn.type = "button";
        nameBtn.className = "picker-file";
        nameBtn.textContent = shown;
        nameBtn.addEventListener("click", () => {
            if (mode === "save") {
                nameEl.value = shown;
                nameEl.focus();
                return;
            }
            openStored(`${DIR}/${info.name}`, shown);
        });
        const size = document.createElement("span");
        size.className = "picker-size";
        size.textContent = formatSize(info.size || 0);
        const actions = document.createElement("span");
        actions.className = "picker-row-actions";
        const dl = document.createElement("button");
        dl.type = "button";
        dl.textContent = "Download";
        dl.addEventListener("click", async (e) => {
            e.stopPropagation();
            try {
                await downloadFsBlob(`${DIR}/${info.name}`, shown);
            } catch (err) {
                showErr(String(err.message || err));
            }
        });
        const ren = document.createElement("button");
        ren.type = "button";
        ren.textContent = "Rename";
        ren.addEventListener("click", (e) => {
            e.stopPropagation();
            startRename(li, info, shown);
        });
        const del = document.createElement("button");
        del.type = "button";
        del.textContent = "Delete";
        del.addEventListener("click", async (e) => {
            e.stopPropagation();
            if (!confirm(`Delete ${shown}?`)) {
                return;
            }
            try {
                await deleteFs(`${DIR}/${info.name}`);
                if (picoName() === shown) {
                    setPicoName("");
                }
                await refresh();
            } catch (err) {
                showErr(String(err.message || err));
            }
        });
        actions.append(dl, ren, del);
        li.append(nameBtn, size, actions);
        return li;
    }

    function startRename(li, info, shown) {
        const input = document.createElement("input");
        input.type = "text";
        input.value = shown;
        input.className = "picker-rename";
        let done = false;
        const finish = async (ok) => {
            if (done) {
                return;
            }
            done = true;
            if (!ok) {
                await refresh();
                return;
            }
            try {
                const gzip = String(info.name).endsWith(".gz");
                const to = storedPath(input.value, gzip);
                const from = `${DIR}/${info.name}`;
                if (to === from) {
                    await refresh();
                    return;
                }
                await renameFs(from, to);
                if (picoName() === shown) {
                    setPicoName(displayName(to.split("/").pop()));
                }
                await refresh();
            } catch (err) {
                showErr(String(err.message || err));
                await refresh();
            }
        };
        input.addEventListener("keydown", (e) => {
            if (e.key === "Enter") {
                e.preventDefault();
                finish(true);
            }
            if (e.key === "Escape") {
                e.preventDefault();
                finish(false);
            }
        });
        input.addEventListener("blur", () => finish(true));
        li.replaceChildren(input);
        input.focus();
        input.select();
    }

    async function openStored(path, shown) {
        try {
            const data = await getFsJson(path);
            loadDoc(fromDocument(data));
            setPicoName(shown);
            close();
            setStatus(`opened ${shown}`, "ok");
        } catch (err) {
            showErr(String(err.message || err));
        }
    }

    async function saveToPico() {
        showErr("");
        try {
            const text = JSON.stringify(getDoc());
            const gz = await gzipUtf8(text);
            const path = storedPath(nameEl.value, !!gz);
            const shown = displayName(path.split("/").pop());
            const exists = files.some((f) => displayName(f.name) === shown);
            if (exists && !confirm(`Replace ${shown}?`)) {
                return;
            }
            await putFs(path, gz || text);
            setPicoName(shown);
            close();
            setStatus(`saved ${shown}`, "ok");
        } catch (err) {
            showErr(String(err.message || err));
        }
    }

    async function open(nextMode) {
        mode = nextMode;
        $("picker-title").textContent = mode === "save" ? "Save label" : "Open label";
        saveRow.hidden = mode !== "save";
        saveBtn.hidden = mode !== "save";
        pcBtn.textContent = mode === "save" ? "Download current" : "This computer…";
        nameEl.value = picoName() || "label.json";
        overlay.hidden = false;
        if (mode === "save") {
            nameEl.focus();
            nameEl.select();
        }
        await refresh();
    }

    pcBtn.addEventListener("click", () => {
        if (mode === "save") {
            downloadDocument(getDoc());
            close();
            return;
        }
        close();
        $("file").click();
    });
    saveBtn.addEventListener("click", () => saveToPico());
    $("picker-cancel").addEventListener("click", () => close());
    overlay.addEventListener("click", (e) => {
        if (e.target === overlay || e.target.classList.contains("picker-backdrop")) {
            close();
        }
    });
    document.addEventListener("keydown", (e) => {
        if (overlay.hidden) {
            return;
        }
        if (e.key === "Escape" && e.target.className !== "picker-rename") {
            e.preventDefault();
            close();
        }
    });

    $("open").addEventListener("click", () => open("open"));
    $("save").addEventListener("click", () => open("save"));

    return { isOpen: () => !overlay.hidden };
}
