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
    const pcOpen = $("picker-pc-open");
    const pcSave = $("picker-pc-save");
    let mode = "open";
    let files = [];
    let listIndex = 0;
    let rowCol = 0;
    let region = "list";
    let actionIndex = 0;

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
        if (listIndex >= files.length) {
            listIndex = Math.max(0, files.length - 1);
        }
        paintCurrent();
    }

    function listItems() {
        return [...listEl.querySelectorAll("li")];
    }

    function rowButtons(li) {
        if (!li) {
            return [];
        }
        return [
            li.querySelector(".picker-file"),
            ...li.querySelectorAll(".picker-row-actions button"),
        ].filter(Boolean);
    }

    function paintCurrent() {
        const items = listItems();
        items.forEach((li, i) => {
            const onRow = region === "list" && i === listIndex;
            li.classList.toggle("current", onRow);
            rowButtons(li).forEach((b, c) => b.classList.toggle("nav", onRow && c === rowCol));
        });
        if (region === "list") {
            items[listIndex]?.scrollIntoView({ block: "nearest" });
        }
        const actions = actionButtons();
        if (actionIndex >= actions.length) {
            actionIndex = Math.max(0, actions.length - 1);
        }
        actions.forEach((b, i) => b.classList.toggle("nav", region === "actions" && i === actionIndex));
    }

    function moveCurrent(delta) {
        const items = listItems();
        if (!items.length) {
            return;
        }
        listIndex = (listIndex + delta + items.length) % items.length;
        const n = rowButtons(items[listIndex]).length;
        if (rowCol >= n) {
            rowCol = Math.max(0, n - 1);
        }
        paintCurrent();
    }

    function moveRowCol(delta) {
        const btns = rowButtons(listItems()[listIndex]);
        if (!btns.length) {
            return;
        }
        rowCol = (rowCol + delta + btns.length) % btns.length;
        paintCurrent();
    }

    function activateCurrent() {
        if (region === "actions") {
            actionButtons()[actionIndex]?.click();
            return;
        }
        const btn = rowButtons(listItems()[listIndex])[rowCol];
        if (btn) {
            btn.click();
        }
    }

    function setRegion(next) {
        region = next;
        if (next === "actions") {
            actionIndex = 0;
        } else {
            rowCol = 0;
        }
        paintCurrent();
    }

    function actionButtons() {
        return [...overlay.querySelector(".picker-actions").children].filter((el) => !el.hidden && !el.disabled);
    }

    function typingIn(el) {
        if (!el) {
            return false;
        }
        const tag = el.tagName;
        if (tag === "TEXTAREA") {
            return true;
        }
        if (tag === "INPUT" && el.type !== "button" && el.type !== "checkbox") {
            return true;
        }
        return false;
    }

    function rowEl(info) {
        const li = document.createElement("li");
        const shown = displayName(info.name);
        const nameBtn = document.createElement("button");
        nameBtn.type = "button";
        nameBtn.className = "picker-file";
        nameBtn.tabIndex = -1;
        nameBtn.textContent = shown;
        nameBtn.addEventListener("click", () => {
            const items = listItems();
            const idx = items.indexOf(li);
            if (idx >= 0) {
                listIndex = idx;
                paintCurrent();
            }
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
        dl.tabIndex = -1;
        dl.textContent = "Download";
        dl.addEventListener("click", (e) => {
            e.stopPropagation();
            try {
                downloadFsBlob(`${DIR}/${info.name}`, shown);
            } catch (err) {
                showErr(String(err.message || err));
            }
        });
        const ren = document.createElement("button");
        ren.type = "button";
        ren.tabIndex = -1;
        ren.textContent = "Rename";
        ren.addEventListener("click", (e) => {
            e.stopPropagation();
            startRename(li, info, shown);
        });
        const del = document.createElement("button");
        del.type = "button";
        del.tabIndex = -1;
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

    async function writeNamed(display, { confirmReplace, closeAfter }) {
        const text = JSON.stringify(getDoc());
        const gz = await gzipUtf8(text);
        const path = storedPath(display, !!gz);
        const shown = displayName(path.split("/").pop());
        if (confirmReplace) {
            const exists = files.some((f) => displayName(f.name) === shown);
            if (exists && !confirm(`Replace ${shown}?`)) {
                return false;
            }
        }
        await putFs(path, gz || text);
        setPicoName(shown);
        if (closeAfter) {
            close();
        }
        setStatus(`saved ${shown}`, "ok");
        return true;
    }

    async function saveToPico() {
        showErr("");
        try {
            await writeNamed(nameEl.value, { confirmReplace: true, closeAfter: true });
        } catch (err) {
            showErr(String(err.message || err));
        }
    }

    async function saveCurrent() {
        const name = picoName();
        if (!name) {
            return false;
        }
        try {
            await writeNamed(name, { confirmReplace: false, closeAfter: false });
            return true;
        } catch (err) {
            setStatus(String(err.message || err), "err");
            return false;
        }
    }

    async function open(nextMode) {
        mode = nextMode;
        $("picker-title").textContent = mode === "save" ? "Save label" : "Open label";
        saveRow.hidden = mode !== "save";
        saveBtn.hidden = mode !== "save";
        pcOpen.hidden = mode === "save";
        pcSave.hidden = mode !== "save";
        nameEl.value = picoName() || "label.json";
        overlay.hidden = false;
        listIndex = 0;
        rowCol = 0;
        region = "list";
        actionIndex = 0;
        overlay.querySelector(".picker-panel").setAttribute("tabindex", "-1");
        if (mode === "save") {
            nameEl.focus();
            nameEl.select();
        } else {
            overlay.querySelector(".picker-panel").focus();
        }
        await refresh();
    }

    pcSave.addEventListener("click", () => {
        downloadDocument(getDoc(), picoName() || "label.pm220.json");
        close();
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
        if ((e.key === "Escape" || (e.key === "q" && !typingIn(e.target))) &&
                e.target.className !== "picker-rename") {
            e.preventDefault();
            close();
            return;
        }
        if (e.key === "Tab") {
            e.preventDefault();
            if (e.shiftKey) {
                if (region === "actions") {
                    setRegion("list");
                    overlay.querySelector(".picker-panel").focus();
                } else if (mode === "save" && document.activeElement !== nameEl) {
                    nameEl.focus();
                    nameEl.select();
                } else {
                    setRegion("actions");
                }
            } else if (region === "list" && document.activeElement !== nameEl) {
                if (mode === "save") {
                    nameEl.focus();
                    nameEl.select();
                } else {
                    setRegion("actions");
                }
            } else {
                nameEl.blur();
                setRegion("actions");
            }
            return;
        }
        if (typingIn(e.target)) {
            if (e.target === nameEl && mode === "save" && e.key === "Enter") {
                e.preventDefault();
                saveToPico();
            }
            return;
        }
        const right = e.key === "ArrowRight" || e.key === "l";
        const left = e.key === "ArrowLeft" || e.key === "h";
        const down = e.key === "ArrowDown" || e.key === "j";
        const up = e.key === "ArrowUp" || e.key === "k";
        if (right || left) {
            e.preventDefault();
            if (region === "actions") {
                const n = actionButtons().length;
                if (n) {
                    actionIndex = (actionIndex + (right ? 1 : n - 1)) % n;
                    paintCurrent();
                }
            } else {
                moveRowCol(right ? 1 : -1);
            }
            return;
        }
        if (down || up) {
            e.preventDefault();
            if (region === "actions") {
                const n = actionButtons().length;
                if (n) {
                    actionIndex = (actionIndex + (down ? 1 : n - 1)) % n;
                    paintCurrent();
                }
            } else {
                moveCurrent(down ? 1 : -1);
            }
            return;
        }
        if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            activateCurrent();
        }
    });

    $("open").addEventListener("click", () => open("open"));
    $("save").addEventListener("click", () => open("save"));

    return {
        isOpen: () => !overlay.hidden,
        open,
        close,
        saveCurrent,
    };
}
