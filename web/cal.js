/** Printer registration calibration (settings/print.json). */

import { fetchMedia, getFsJson, postPrintTest, putFs } from "./api.js";

const PATH = "settings/print.json";
const ADJ_LIM = 80;
const MM_LIM = 10;
const MM_DEFAULT = { offset_x_mm: -1.25, offset_y_mm: 3, gap_mm: 2 };
const SAFE_DEFAULT = { safe_x0: 32, safe_y0: 32, safe_x1: 351, safe_y1: 207 };
const DPI = 203;
const WIDTH_MM = 50;
const HEIGHT_MM = 30;
const SRC_W = 384;
const SRC_H = 240;
const $ = (id) => document.getElementById(id);

function clampAdj(n) {
    if (!Number.isFinite(n)) {
        return 0;
    }
    n = Math.round(n);
    if (n < -ADJ_LIM) {
        return -ADJ_LIM;
    }
    if (n > ADJ_LIM) {
        return ADJ_LIM;
    }
    return n;
}

function clampSafe(n, lo, hi) {
    if (!Number.isFinite(n)) {
        return lo;
    }
    n = Math.round(n);
    if (n < lo) {
        return lo;
    }
    if (n > hi) {
        return hi;
    }
    return n;
}

function clampMm(n) {
    if (!Number.isFinite(n)) {
        return 0;
    }
    if (n < -MM_LIM) {
        return -MM_LIM;
    }
    if (n > MM_LIM) {
        return MM_LIM;
    }
    return Math.round(n * 100) / 100;
}

function mmToDots(mm) {
    return Math.round(mm * DPI / 25.4);
}

function zeros() {
    return {
        origin_x_adj: 0,
        origin_y_adj: 0,
        print_height_adj: 0,
        offset_x_mm: MM_DEFAULT.offset_x_mm,
        offset_y_mm: MM_DEFAULT.offset_y_mm,
        gap_mm: MM_DEFAULT.gap_mm,
        ...SAFE_DEFAULT,
    };
}

function readAdj() {
    return {
        origin_x_adj: clampAdj(Number($("cal-ox").value)),
        origin_y_adj: clampAdj(Number($("cal-oy").value)),
        print_height_adj: clampAdj(Number($("cal-ph").value)),
        offset_x_mm: clampMm(Number($("cal-offx").value)),
        offset_y_mm: clampMm(Number($("cal-offy").value)),
        gap_mm: clampMm(Math.max(0, Number($("cal-gap").value))),
        safe_x0: clampSafe(Number($("cal-sx0").value), 0, SRC_W - 2),
        safe_y0: clampSafe(Number($("cal-sy0").value), 0, SRC_H - 2),
        safe_x1: clampSafe(Number($("cal-sx1").value), 1, SRC_W - 1),
        safe_y1: clampSafe(Number($("cal-sy1").value), 1, SRC_H - 1),
    };
}

function fillAdj(data) {
    const d = { ...zeros(), ...(data || {}) };
    $("cal-ox").value = String(clampAdj(Number(d.origin_x_adj) || 0));
    $("cal-oy").value = String(clampAdj(Number(d.origin_y_adj) || 0));
    $("cal-ph").value = String(clampAdj(Number(d.print_height_adj) || 0));
    const ox = d.offset_x_mm;
    const oy = d.offset_y_mm;
    $("cal-offx").value = String(clampMm(ox == null ? MM_DEFAULT.offset_x_mm : Number(ox)));
    $("cal-offy").value = String(clampMm(oy == null ? MM_DEFAULT.offset_y_mm : Number(oy)));
    const gap = d.gap_mm;
    $("cal-gap").value = String(clampMm(gap == null ? MM_DEFAULT.gap_mm : Math.max(0, Number(gap))));
    $("cal-sx0").value = String(d.safe_x0 == null ? SAFE_DEFAULT.safe_x0 : clampSafe(Number(d.safe_x0), 0, SRC_W - 2));
    $("cal-sy0").value = String(d.safe_y0 == null ? SAFE_DEFAULT.safe_y0 : clampSafe(Number(d.safe_y0), 0, SRC_H - 2));
    $("cal-sx1").value = String(d.safe_x1 == null ? SAFE_DEFAULT.safe_x1 : clampSafe(Number(d.safe_x1), 1, SRC_W - 1));
    $("cal-sy1").value = String(d.safe_y1 == null ? SAFE_DEFAULT.safe_y1 : clampSafe(Number(d.safe_y1), 1, SRC_H - 1));
}

function computeLayout(adj) {
    const pageW = mmToDots(WIDTH_MM);
    const pageH = mmToDots(HEIGHT_MM);
    let originX = Math.floor((pageW - SRC_W) / 2);
    let originY = Math.floor((pageH - SRC_H) / 2);
    if (originX < 0) {
        originX = 0;
    }
    if (originY < 0) {
        originY = 0;
    }
    const dx = mmToDots(adj.offset_x_mm);
    const dy = mmToDots(adj.offset_y_mm);
    let shiftX = 0;
    let shiftY = 0;
    if (originX + dx < 0) {
        shiftX = originX + dx;
        originX = 0;
    } else {
        originX += dx;
    }
    if (originY + dy < 0) {
        shiftY = originY + dy;
        originY = 0;
    } else {
        originY += dy;
    }
    originX += adj.origin_x_adj;
    originY += adj.origin_y_adj;
    if (originX < 0) {
        shiftX += originX;
        originX = 0;
    }
    if (originY < 0) {
        shiftY += originY;
        originY = 0;
    }
    if (originX > pageW - 1) {
        originX = pageW - 1;
    }
    if (originY > pageH - 1) {
        originY = pageH - 1;
    }
    const availW = Math.max(1, pageW - originX);
    const availH = Math.max(1, pageH - originY);
    let printW = SRC_W < availW ? SRC_W : availW;
    let printH = SRC_H < availH ? SRC_H : availH;
    printH += adj.print_height_adj;
    if (printH < 1) {
        printH = 1;
    }
    if (printH > availH) {
        printH = availH;
    }
    return { originX, originY, printW, printH, shiftX, shiftY, pageW, pageH };
}

export function bindCalibrate({ setStatus, applyMedia }) {
    const overlay = $("cal");
    const errEl = $("cal-err");

    function showErr(msg) {
        if (!msg) {
            errEl.hidden = true;
            errEl.textContent = "";
            return;
        }
        errEl.hidden = false;
        errEl.textContent = msg;
    }

    function updateEffective() {
        const L = computeLayout(readAdj());
        $("cal-effective").textContent =
            `BITMAP ${L.originX},${L.originY}  ${L.printW}×${L.printH}  ` +
            `(canvas ${SRC_W}×${SRC_H}, offset ${readAdj().offset_x_mm}/${readAdj().offset_y_mm} mm, gap ${readAdj().gap_mm} mm, ` +
            `safe ${readAdj().safe_x0},${readAdj().safe_y0}–${readAdj().safe_x1},${readAdj().safe_y1})`;
    }

    async function loadFile() {
        try {
            return await getFsJson(PATH);
        } catch {
            return zeros();
        }
    }

    async function open() {
        showErr("");
        fillAdj(zeros());
        overlay.hidden = false;
        fillAdj(await loadFile());
        updateEffective();
        $("cal-offx").focus();
        $("cal-offx").select();
    }

    function close() {
        overlay.hidden = true;
    }

    async function save() {
        showErr("");
        const body = JSON.stringify(readAdj());
        try {
            await putFs(PATH, body);
            try {
                const media = await fetchMedia();
                if (applyMedia && media) {
                    applyMedia(media);
                }
            } catch {
                /* keep last media */
            }
            updateEffective();
            setStatus("calibration saved", "ok");
        } catch (err) {
            showErr(String(err.message || err));
        }
    }

    overlay.addEventListener("input", () => updateEffective());
    $("calibrate").addEventListener("click", () => open());
    $("cal-close").addEventListener("click", () => close());
    $("cal-save").addEventListener("click", () => save());
    $("cal-reset").addEventListener("click", () => {
        fillAdj(zeros());
        updateEffective();
    });
    $("cal-test").addEventListener("click", async () => {
        showErr("");
        try {
            await save();
            const res = await postPrintTest();
            setStatus(res.job ? `test print ${res.job}` : "test print sent", "ok");
        } catch (err) {
            showErr(String(err.message || err));
        }
    });
    overlay.addEventListener("click", (e) => {
        if (e.target === overlay || e.target.classList.contains("picker-backdrop")) {
            close();
        }
    });
    document.addEventListener("keydown", (e) => {
        if (overlay.hidden) {
            return;
        }
        if (e.key === "Escape") {
            e.preventDefault();
            close();
        }
    });

    return { isOpen: () => !overlay.hidden };
}
