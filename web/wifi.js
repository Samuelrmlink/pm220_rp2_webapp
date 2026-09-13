/** Wi-Fi settings modal. */

import {
    connectWifi,
    deleteWifiNetwork,
    getWifi,
    getWifiNetworks,
    getWifiScan,
    putWifi,
    putWifiNetwork,
    startWifiScan,
    wifiForceAp,
} from "./api.js";

const $ = (id) => document.getElementById(id);

function sleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}

export function bindWifiSettings({ setStatus }) {
    const overlay = $("wifi");
    const sub = $("wifi-sub");
    const errEl = $("wifi-err");
    const saveBtn = $("wifi-save");
    const startApBtn = $("wifi-start-ap");
    const scanBox = $("wifi-scan-box");
    const scanList = $("wifi-scan-list");
    const scanEmpty = $("wifi-scan-empty");
    const scanWarn = $("wifi-scan-warn");
    const knownList = $("wifi-known-list");
    const knownEmpty = $("wifi-known-empty");
    const knownActions = $("wifi-known-actions");

    let snapshot = { mdns: "", apSsid: "", apPass: "", scan: "idle" };
    let knownPass = {};
    let filling = false;
    let live = null;
    let pollTimer = 0;
    let selectedKnown = "";
    let scanStarted = false;
    let scanning = false;
    let subOk = null;
    let wifiEpoch = 0;
    let wifiAbort = null;
    let wifiTab = "mdns";
    let wifiListIndex = 0;
    let fieldIndex = 0;
    let knownActionIndex = 0;
    const TAB_ORDER = ["mdns", "ap", "sta"];

    function wifiLive() {
        return !overlay.hidden;
    }

    function wifiSignal() {
        if (!wifiAbort) {
            wifiAbort = new AbortController();
        }
        return wifiAbort.signal;
    }

    function showErr(el, msg) {
        if (!msg) {
            el.hidden = true;
            el.textContent = "";
            return;
        }
        el.hidden = false;
        el.textContent = msg;
    }

    function dirty() {
        if (filling) {
            return false;
        }
        const mdns = $("wifi-mdns").value.trim().toLowerCase();
        const ssid = $("wifi-ap-ssid").value.trim();
        const psk = $("wifi-ap-psk").value;
        const scan = $("wifi-scan-policy").value;
        return mdns !== snapshot.mdns || ssid !== snapshot.apSsid ||
            psk !== snapshot.apPass || scan !== snapshot.scan;
    }

    function updateSave() {
        saveBtn.hidden = !dirty();
    }

    function setTab(name) {
        wifiTab = name;
        for (const btn of overlay.querySelectorAll(".wifi-tab")) {
            btn.setAttribute("aria-selected", btn.dataset.tab === name ? "true" : "false");
            btn.tabIndex = -1;
        }
        $("wifi-pane-mdns").hidden = name !== "mdns";
        $("wifi-pane-ap").hidden = name !== "ap";
        $("wifi-pane-sta").hidden = name !== "sta";
        paintWifiField();
    }

    function wifiFields() {
        const f = [{ kind: "tabs" }];
        if (wifiTab === "mdns") {
            f.push({ kind: "el", id: "wifi-mdns" });
        } else if (wifiTab === "ap") {
            f.push({ kind: "el", id: "wifi-ap-ssid" });
            f.push({ kind: "el", id: "wifi-ap-psk" });
            f.push({ kind: "el", id: "wifi-scan-policy" });
            if (!$("wifi-start-ap").hidden) {
                f.push({ kind: "el", id: "wifi-start-ap" });
            }
        } else {
            f.push({ kind: "el", id: "wifi-scan-btn" });
            if (stationRows().length) {
                f.push({ kind: "list" });
            }
            if (!knownActions.hidden) {
                f.push({ kind: "known-actions" });
            }
        }
        if (!saveBtn.hidden) {
            f.push({ kind: "el", id: "wifi-save" });
        }
        f.push({ kind: "el", id: "wifi-close" });
        return f;
    }

    function clearWifiNav() {
        overlay.querySelector(".wifi-tabs")?.classList.remove("nav");
        overlay.querySelectorAll(".nav-field, .picker-actions button.nav, .wifi-known-actions button.nav")
            .forEach((el) => el.classList.remove("nav-field", "nav"));
        stationRows().forEach((li) => li.classList.remove("current"));
    }

    function paintWifiField() {
        const fields = wifiFields();
        if (!fields.length) {
            return;
        }
        if (fieldIndex < 0) {
            fieldIndex = 0;
        }
        if (fieldIndex >= fields.length) {
            fieldIndex = fields.length - 1;
        }
        const cur = fields[fieldIndex];
        clearWifiNav();
        const tabs = overlay.querySelector(".wifi-tabs");
        if (cur.kind === "tabs") {
            tabs.classList.add("nav");
            overlay.querySelector(".picker-panel")?.focus();
            return;
        }
        if (cur.kind === "list") {
            paintWifiCurrent();
            overlay.querySelector(".picker-panel")?.focus();
            return;
        }
        if (cur.kind === "known-actions") {
            const btns = [...knownActions.querySelectorAll("button")];
            if (knownActionIndex >= btns.length) {
                knownActionIndex = Math.max(0, btns.length - 1);
            }
            btns[knownActionIndex]?.classList.add("nav");
            overlay.querySelector(".picker-panel")?.focus();
            return;
        }
        const el = $(cur.id);
        if (!el) {
            return;
        }
        el.classList.add(el.closest(".picker-actions") ? "nav" : "nav-field");
        el.focus();
        if (el.tagName === "INPUT" && el.select) {
            el.select();
        }
    }

    function moveWifiField(delta) {
        const fields = wifiFields();
        if (!fields.length) {
            return;
        }
        fieldIndex = (fieldIndex + delta + fields.length) % fields.length;
        paintWifiField();
    }

    function typingIn(el) {
        if (!el) {
            return false;
        }
        const tag = el.tagName;
        if (tag === "TEXTAREA") {
            return true;
        }
        if (tag === "SELECT") {
            return true;
        }
        if (tag === "INPUT" && el.type !== "button" && el.type !== "checkbox") {
            return true;
        }
        return false;
    }

    function stationRows() {
        return [...scanList.children, ...knownList.children];
    }

    function paintWifiCurrent() {
        const rows = stationRows();
        if (!rows.length) {
            return;
        }
        if (wifiListIndex < 0) {
            wifiListIndex = 0;
        }
        if (wifiListIndex >= rows.length) {
            wifiListIndex = rows.length - 1;
        }
        rows.forEach((li, i) => li.classList.toggle("current", i === wifiListIndex));
        rows[wifiListIndex]?.scrollIntoView({ block: "nearest" });
    }

    function moveWifiCurrent(delta) {
        const rows = stationRows();
        if (!rows.length) {
            return;
        }
        wifiListIndex = (wifiListIndex + delta + rows.length) % rows.length;
        paintWifiCurrent();
    }

    function activateWifiCurrent() {
        const row = stationRows()[wifiListIndex];
        if (row) {
            row.click();
        }
    }

    function liveLine(st) {
        if (!st) {
            return "status unavailable";
        }
        const parts = [st.mode || "?"];
        if (st.mode === "sta" && st.sta_ssid) {
            parts.push(st.sta_ssid);
        } else if (st.ap_ssid) {
            parts.push(st.ap_ssid);
        }
        if (st.ip) {
            parts.push(st.ip);
        }
        if (st.connecting) {
            parts.push("joining");
        }
        if (st.printer_connected === false) {
            parts.push("wait for printer");
        }
        if (st.last_error) {
            parts.push(st.last_error);
        }
        return parts.join(" · ");
    }

    function fillFromStatus(st) {
        live = st;
        $("wifi-live").textContent = liveLine(st);
        filling = true;
        $("wifi-mdns").value = st.mdns || "";
        $("wifi-mdns-hint").textContent = `http://${st.mdns || "pm220"}.local/`;
        $("wifi-ap-ssid").value = st.ap_ssid || "";
        $("wifi-ap-psk").value = st.ap_password || "";
        $("wifi-scan-policy").value = st.scan || "idle";
        filling = false;
        snapshot = {
            mdns: (st.mdns || "").toLowerCase(),
            apSsid: st.ap_ssid || "",
            apPass: st.ap_password || "",
            scan: st.scan || "idle",
        };
        startApBtn.hidden = st.mode === "ap";
        scanWarn.hidden = !(st.scan_disturbs_ap && st.mode === "ap");
        $("wifi-scan-btn").disabled = st.printer_connected === false;
        updateSave();
    }

    async function refreshLive() {
        if (!wifiLive()) {
            return;
        }
        try {
            const st = await getWifi({ signal: wifiSignal() });
            if (!wifiLive()) {
                return;
            }
            live = st;
            $("wifi-live").textContent = liveLine(st);
            startApBtn.hidden = st.mode === "ap";
            scanWarn.hidden = !(st.scan_disturbs_ap && st.mode === "ap");
            $("wifi-scan-btn").disabled = st.printer_connected === false;
            if (!dirty()) {
                fillFromStatus(st);
            }
        } catch (err) {
            if (err && err.name === "AbortError") {
                return;
            }
            if (!wifiLive()) {
                return;
            }
            $("wifi-live").textContent = String(err.message || err);
        }
    }

    function knownRow(net) {
        const ssid = net.ssid;
        const pw = net.password || "";
        const li = document.createElement("li");
        li.dataset.ssid = ssid;
        if (ssid === selectedKnown) {
            li.className = "selected";
        }
        const name = document.createElement("span");
        name.className = "wifi-ssid";
        name.textContent = ssid;
        const meta = document.createElement("span");
        meta.className = "wifi-meta";
        meta.textContent = pw || "(open)";
        li.append(name, meta);
        li.addEventListener("click", () => {
            selectedKnown = ssid;
            paintKnownSelection();
        });
        return li;
    }

    function paintKnownSelection() {
        for (const li of knownList.children) {
            li.classList.toggle("selected", li.dataset.ssid === selectedKnown);
        }
        knownActions.hidden = !selectedKnown;
    }

    async function refreshKnown() {
        if (!wifiLive()) {
            return;
        }
        try {
            const data = await getWifiNetworks({ signal: wifiSignal() });
            if (!wifiLive()) {
                return;
            }
            const nets = data.networks || [];
            knownList.replaceChildren();
            knownPass = {};
            if (selectedKnown && !nets.some((n) => n.ssid === selectedKnown)) {
                selectedKnown = "";
            }
            for (const n of nets) {
                if (n.ssid) {
                    knownPass[n.ssid] = n.password || "";
                    knownList.appendChild(knownRow(n));
                }
            }
            knownEmpty.hidden = nets.length > 0;
            paintKnownSelection();
            paintWifiCurrent();
        } catch (err) {
            showErr(errEl, String(err.message || err));
        }
    }

    function scanRow(ap) {
        const li = document.createElement("li");
        const name = document.createElement("span");
        name.className = "wifi-ssid";
        name.textContent = ap.ssid || "(hidden)";
        const meta = document.createElement("span");
        meta.className = "wifi-meta";
        const bits = [];
        if (ap.rssi != null) {
            bits.push(`${ap.rssi} dBm`);
        }
        if (ap.chan) {
            bits.push(`ch ${ap.chan}`);
        }
        if (ap.auth) {
            bits.push(ap.auth);
        }
        if (ap.known) {
            bits.push("saved");
        }
        meta.textContent = bits.join(" · ");
        li.append(name, meta);
        li.addEventListener("click", () => {
            if (!ap.ssid) {
                return;
            }
            openConnect(ap);
        });
        return li;
    }

    function paintScan(data) {
        const aps = (data.aps || []).slice().sort((a, b) => (b.rssi || -999) - (a.rssi || -999));
        scanList.replaceChildren();
        for (const ap of aps) {
            scanList.appendChild(scanRow(ap));
        }
        wifiListIndex = 0;
        paintWifiCurrent();
        scanEmpty.textContent = (live && live.printer_connected === false)
            ? "Connect the printer before scanning."
            : "No networks found.";
        scanEmpty.hidden = aps.length > 0 || data.scanning;
        if (data.scanning) {
            scanEmpty.hidden = true;
        }
    }

    async function runScan() {
        if (scanning) {
            return;
        }
        if (live && live.printer_connected === false) {
            showErr(errEl, "Connect the printer before scanning.");
            return;
        }
        scanning = true;
        scanStarted = true;
        scanBox.hidden = false;
        $("wifi-scan-btn").disabled = true;
        showErr(errEl, "");
        try {
            await startWifiScan();
            const t0 = Date.now();
            let seen = false;
            let data = { scanning: true, aps: [] };
            while (Date.now() - t0 < 12000) {
                if (!wifiLive()) {
                    return;
                }
                data = await getWifiScan({ signal: wifiSignal() });
                if (!wifiLive()) {
                    return;
                }
                paintScan(data);
                if (data.scanning) {
                    seen = true;
                } else if (seen || Date.now() - t0 > 1500) {
                    break;
                }
                await sleep(400);
            }
            if (!wifiLive()) {
                return;
            }
            paintScan(data);
        } catch (err) {
            showErr(errEl, String(err.message || err));
        } finally {
            scanning = false;
            $("wifi-scan-btn").disabled = false;
        }
    }

    function closeSub() {
        sub.hidden = true;
        subOk = null;
        showErr($("wifi-sub-err"), "");
    }

    function openSub(opts) {
        $("wifi-sub-title").textContent = opts.title;
        $("wifi-sub-text").textContent = opts.text || "";
        $("wifi-sub-text").hidden = !opts.text;
        $("wifi-sub-form").hidden = !opts.form;
        $("wifi-sub-ssid").value = opts.ssid || "";
        $("wifi-sub-ssid").readOnly = !!opts.ssidReadOnly;
        $("wifi-sub-psk").value = opts.pskValue || "";
        $("wifi-sub-psk").placeholder = opts.pskPlaceholder || "";
        $("wifi-sub-psk-label").hidden = !!opts.pskHidden;
        $("wifi-sub-psk").hidden = !!opts.pskHidden;
        $("wifi-sub-ok").textContent = opts.okLabel || "OK";
        showErr($("wifi-sub-err"), "");
        subOk = opts.onOk;
        sub.hidden = false;
        const focusEl = opts.form
            ? (opts.ssidReadOnly ? $("wifi-sub-psk") : $("wifi-sub-ssid"))
            : $("wifi-sub-ok");
        if (!opts.pskHidden && opts.form && opts.ssidReadOnly) {
            $("wifi-sub-psk").focus();
        } else {
            focusEl.focus();
        }
    }

    function openConnect(ap) {
        const openNet = !ap.auth || ap.auth === "open";
        const stored = knownPass[ap.ssid];
        openSub({
            title: "Connect",
            text: "Saves this network and joins it. SoftAP drops if the join succeeds.",
            form: true,
            ssid: ap.ssid,
            ssidReadOnly: true,
            pskHidden: openNet,
            pskValue: openNet ? "" : (stored || ""),
            pskPlaceholder: "",
            okLabel: "Connect",
            onOk: async () => {
                const pw = openNet ? "" : $("wifi-sub-psk").value;
                await connectWifi(ap.ssid, pw);
                closeSub();
                setStatus(`joining ${ap.ssid}`, "");
                await refreshKnown();
                await refreshLive();
            },
        });
    }

    async function saveSettings() {
        showErr(errEl, "");
        const body = {};
        const mdns = $("wifi-mdns").value.trim().toLowerCase();
        const ssid = $("wifi-ap-ssid").value.trim();
        const psk = $("wifi-ap-psk").value;
        const scan = $("wifi-scan-policy").value;
        if (mdns !== snapshot.mdns) {
            body.mdns = mdns;
        }
        if (ssid !== snapshot.apSsid) {
            body.ap_ssid = ssid;
        }
        if (psk !== snapshot.apPass) {
            body.ap_password = psk;
        }
        if (scan !== snapshot.scan) {
            body.scan = scan;
        }
        if (!Object.keys(body).length) {
            updateSave();
            return;
        }
        try {
            const st = await putWifi(body);
            fillFromStatus(st);
            setStatus("Wi-Fi settings saved", "ok");
        } catch (err) {
            showErr(errEl, String(err.message || err));
        }
    }

    async function open() {
        const epoch = ++wifiEpoch;
        showErr(errEl, "");
        selectedKnown = "";
        overlay.hidden = false;
        overlay.querySelector(".picker-panel").setAttribute("tabindex", "-1");
        fieldIndex = 0;
        knownActionIndex = 0;
        wifiListIndex = 0;
        if (wifiAbort) {
            wifiAbort.abort();
        }
        wifiAbort = new AbortController();
        setTab("mdns");
        paintWifiField();
        try {
            const st = await getWifi({ signal: wifiSignal() });
            if (epoch !== wifiEpoch || !wifiLive()) {
                return;
            }
            fillFromStatus(st);
            paintWifiField();
        } catch (err) {
            if (err && err.name === "AbortError") {
                return;
            }
            if (epoch !== wifiEpoch || !wifiLive()) {
                return;
            }
            showErr(errEl, String(err.message || err));
        }
        await refreshKnown();
        if (epoch !== wifiEpoch || !wifiLive()) {
            return;
        }
        if (scanStarted) {
            try {
                paintScan(await getWifiScan({ signal: wifiSignal() }));
            } catch {
                /* keep last list */
            }
        }
        if (epoch !== wifiEpoch || !wifiLive()) {
            return;
        }
        startLivePoll();
    }

    function startLivePoll() {
        stopLivePoll();
        pollTimer = setInterval(() => {
            if (!overlay.hidden) {
                refreshLive();
            }
        }, 4000);
    }

    function stopLivePoll() {
        if (pollTimer) {
            clearInterval(pollTimer);
            pollTimer = 0;
        }
    }

    function close() {
        wifiEpoch++;
        scanning = false;
        closeSub();
        overlay.hidden = true;
        stopLivePoll();
        if (wifiAbort) {
            wifiAbort.abort();
            wifiAbort = null;
        }
    }

    for (const btn of overlay.querySelectorAll(".wifi-tab")) {
        btn.addEventListener("click", () => {
            fieldIndex = 0;
            setTab(btn.dataset.tab);
        });
    }
    $("wifi-mdns").addEventListener("input", () => {
        $("wifi-mdns-hint").textContent =
            `http://${$("wifi-mdns").value.trim().toLowerCase() || "pm220"}.local/`;
        updateSave();
    });
    $("wifi-ap-ssid").addEventListener("input", updateSave);
    $("wifi-ap-psk").addEventListener("input", updateSave);
    $("wifi-scan-policy").addEventListener("change", updateSave);
    saveBtn.addEventListener("click", () => saveSettings());
    $("wifi-close").addEventListener("click", () => close());
    $("wifi-settings").addEventListener("click", () => open());
    $("wifi-scan-btn").addEventListener("click", () => runScan());
    startApBtn.addEventListener("click", async () => {
        showErr(errEl, "");
        try {
            await wifiForceAp();
            setStatus("starting SoftAP", "");
            await refreshLive();
        } catch (err) {
            showErr(errEl, String(err.message || err));
        }
    });
    $("wifi-known-connect").addEventListener("click", async () => {
        if (!selectedKnown) {
            return;
        }
        showErr(errEl, "");
        try {
            await connectWifi(selectedKnown);
            setStatus(`joining ${selectedKnown}`, "");
            await refreshLive();
        } catch (err) {
            showErr(errEl, String(err.message || err));
        }
    });
    $("wifi-known-modify").addEventListener("click", () => {
        if (!selectedKnown) {
            return;
        }
        const old = selectedKnown;
        openSub({
            title: "Modify network",
            text: "",
            form: true,
            ssid: old,
            ssidReadOnly: false,
            pskValue: knownPass[old] || "",
            okLabel: "Save",
            onOk: async () => {
                const ssid = $("wifi-sub-ssid").value.trim();
                const psk = $("wifi-sub-psk").value;
                if (!ssid) {
                    throw new Error("ssid required");
                }
                const opts = { password: psk };
                if (ssid !== old) {
                    opts.newSsid = ssid;
                }
                await putWifiNetwork(old, opts);
                selectedKnown = ssid;
                closeSub();
                await refreshKnown();
            },
        });
    });
    $("wifi-known-delete").addEventListener("click", () => {
        if (!selectedKnown) {
            return;
        }
        const ssid = selectedKnown;
        openSub({
            title: "Delete network",
            text: `Delete “${ssid}”?`,
            form: false,
            okLabel: "Delete",
            onOk: async () => {
                await deleteWifiNetwork(ssid);
                selectedKnown = "";
                closeSub();
                await refreshKnown();
            },
        });
    });
    $("wifi-sub-cancel").addEventListener("click", () => closeSub());
    $("wifi-sub-ok").addEventListener("click", async () => {
        if (!subOk) {
            return;
        }
        showErr($("wifi-sub-err"), "");
        $("wifi-sub-ok").disabled = true;
        try {
            await subOk();
        } catch (err) {
            showErr($("wifi-sub-err"), String(err.message || err));
        } finally {
            $("wifi-sub-ok").disabled = false;
        }
    });
    overlay.addEventListener("click", (e) => {
        if (e.target === overlay || e.target.classList.contains("picker-backdrop")) {
            if (sub.hidden) {
                close();
            }
        }
    });
    sub.addEventListener("click", (e) => {
        if (e.target === sub || e.target.classList.contains("picker-backdrop")) {
            closeSub();
        }
    });
    document.addEventListener("keydown", (e) => {
        if (!sub.hidden) {
            if (e.key === "Escape") {
                e.preventDefault();
                closeSub();
            } else if (e.key === "Enter" && e.target !== $("wifi-sub-cancel")) {
                e.preventDefault();
                $("wifi-sub-ok").click();
            }
            return;
        }
        if (overlay.hidden) {
            return;
        }
        if (e.key === "Escape") {
            e.preventDefault();
            close();
            return;
        }
        const mod = e.ctrlKey || e.metaKey;
        if (mod && !e.altKey && e.key.toLowerCase() === "s") {
            e.preventDefault();
            if (!saveBtn.hidden) {
                saveSettings();
            }
            return;
        }
        if (e.key === "Tab") {
            e.preventDefault();
            moveWifiField(e.shiftKey ? -1 : 1);
            return;
        }
        const fields = wifiFields();
        const cur = fields[fieldIndex] || { kind: "tabs" };
        const right = e.key === "ArrowRight" || e.key === "l";
        const left = e.key === "ArrowLeft" || e.key === "h";
        const down = e.key === "ArrowDown" || e.key === "j";
        const up = e.key === "ArrowUp" || e.key === "k";
        if (cur.kind === "tabs" && (right || left || down || up)) {
            e.preventDefault();
            const i = TAB_ORDER.indexOf(wifiTab);
            const n = TAB_ORDER.length;
            const next = (right || down) ? (i + 1) % n : (i - 1 + n) % n;
            setTab(TAB_ORDER[next]);
            return;
        }
        if (cur.kind === "list" && (right || left || down || up)) {
            e.preventDefault();
            moveWifiCurrent(right || down ? 1 : -1);
            return;
        }
        if (cur.kind === "known-actions" && (right || left || down || up)) {
            e.preventDefault();
            const btns = [...knownActions.querySelectorAll("button")];
            const n = btns.length;
            if (n) {
                knownActionIndex = (knownActionIndex + (right || down ? 1 : n - 1)) % n;
                paintWifiField();
            }
            return;
        }
        if (cur.kind === "el" && (right || left || down || up) && !typingIn(e.target)) {
            e.preventDefault();
            moveWifiField(right || down ? 1 : -1);
            return;
        }
        if (typingIn(e.target)) {
            return;
        }
        if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            if (cur.kind === "list") {
                activateWifiCurrent();
            } else if (cur.kind === "known-actions") {
                knownActions.querySelectorAll("button")[knownActionIndex]?.click();
            } else if (cur.kind === "el") {
                const el = $(cur.id);
                if (el && el.tagName === "BUTTON") {
                    el.click();
                }
            }
        }
    });

    return {
        isOpen: () => !overlay.hidden,
    };
}
