/**
 * @file app.js
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The pinled configuration app (docs/WEBUI.md).
 * @version 0.1.0
 * @date 2026-08-19
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * State model, worth stating once: the app holds a WORKING COPY of the two
 * documents (install config, machine profile) and edits it locally; nothing
 * reaches the board until "Apply & restart", because applying is
 * write-then-restart (FR-CFG-16) and a restart per keystroke would be
 * absurd. Live channel state is the one exception — it streams continuously
 * and is never stored.
 *
 * The learn flow is the acceptance test (FIRMWARE_PLAN.md §5.2 step 4):
 * fire a lamp from the test card, watch the grid, click the channel that
 * lit. The sticky bit makes the flash unmissable even at 30 Hz.
 */
/* global pinledApi */
(function () {
    "use strict";

    var $ = function (id) { return document.getElementById(id); };

    // ------------------------------------------------------------- state --

    var S = {
        api: null,       // PinledApi
        info: null,      // last GET /info
        config: null,    // working copy, InstallConfig shape
        profile: null,   // working copy, MachineProfile shape
        dirtyConfig: false,
        dirtyProfile: false,
        selected: 0,
        learning: false,
        live: null,      // last frame {seq, channelCount, samples}
        liveSock: null,
        cellEls: {},
        frames: 0,       // for the rail-foot rate readout
        framesAt: 0,
        fwWatch: null,   // interval while an image is staged
        uploading: false, // background polling pauses while true
    };

    var PRESETS = [
        { name: "Warm white", hex: "#ffe9c4" },
        { name: "Amber", hex: "#ffb23f" },
        { name: "Red", hex: "#ff4436" },
        { name: "Deep red", hex: "#c9203a" },
        { name: "Green", hex: "#3ad171" },
        { name: "Blue", hex: "#2f8fe8" },
        { name: "Ice", hex: "#9fd8ff" },
        { name: "Purple", hex: "#a95cf0" },
        { name: "Pink", hex: "#ff6b9d" },
        { name: "Teal", hex: "#19c7b4" },
    ];

    // The order a person meets them, not enum order (the mockup's call).
    var CLASS_ORDER = ["DRIVE_CLASS_UNSPECIFIED", "DRIVE_CLASS_STEADY",
                       "DRIVE_CLASS_AC_STEADY", "DRIVE_CLASS_AC_DIMMED",
                       "DRIVE_CLASS_MATRIX", "DRIVE_CLASS_OFF"];
    var CLASS_SHORT = {
        DRIVE_CLASS_UNSPECIFIED: "auto",
        DRIVE_CLASS_OFF: "off",
        DRIVE_CLASS_STEADY: "steady",
        DRIVE_CLASS_MATRIX: "matrix",
        DRIVE_CLASS_AC_STEADY: "AC steady",
        DRIVE_CLASS_AC_DIMMED: "AC dimmed",
    };
    // LiveFrame flags bits 2..4 carry the classification, by enum value.
    var CLASS_BY_VALUE = ["auto", "off", "steady", "matrix",
                          "AC steady", "AC dimmed", "?", "?"];

    // ------------------------------------------------------------ helpers --

    function toast(title, sub, kind) {
        var el = document.createElement("div");
        el.className = "toast";
        el.setAttribute("data-kind", kind || "");
        el.innerHTML = "<div>" + esc(title) + "</div>" +
                       (sub ? "<small>" + esc(sub) + "</small>" : "");
        $("toasts").appendChild(el);
        setTimeout(function () {
            el.style.transition = "opacity 300ms ease";
            el.style.opacity = "0";
            setTimeout(function () { el.remove(); }, 320);
        }, 4200);
    }

    function esc(s) {
        return String(s).replace(/[&<>"']/g, function (c) {
            return { "&": "&amp;", "<": "&lt;", ">": "&gt;",
                     '"': "&quot;", "'": "&#39;" }[c];
        });
    }

    function channelCount() {
        var g = (S.config && S.config.geometry) ||
                (S.info && S.info.geometry) || {};
        return (g.numModules || 0) * (g.channelsPerModule || 0);
    }

    function wiringByChannel() {
        var map = {};
        ((S.config && S.config.wiring) || []).forEach(function (w) {
            map[w.channel] = w;
        });
        return map;
    }

    function lampEntry(lamp, create) {
        var lamps = S.profile.lamps || (S.profile.lamps = []);
        for (var i = 0; i < lamps.length; ++i)
            if (lamps[i].lamp === lamp) return lamps[i];
        if (!create) return null;
        var e = { lamp: lamp, name: "", color: null,
                  classLock: "DRIVE_CLASS_UNSPECIFIED",
                  attackMs: 0, decayMs: 0, gainPermille: 0 };
        lamps.push(e);
        return e;
    }

    function colorHex(c) {
        if (!c || (!c.r && !c.g && !c.b)) return PRESETS[0].hex;
        return "#" + [c.r || 0, c.g || 0, c.b || 0].map(function (v) {
            return v.toString(16).padStart(2, "0");
        }).join("");
    }
    function hexColor(hex) {
        var n = parseInt(hex.slice(1), 16);
        return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
    }

    function setDirty() {
        var dirty = S.dirtyConfig || S.dirtyProfile;
        $("dirty-badge").hidden = !dirty;
        $("apply-btn").disabled = !dirty;
    }

    /// Poll /info until the board answers again — the shape of every apply
    /// (FR-CFG-16) and of a confirmed OTA.
    function waitForReboot(timeoutMs) {
        var deadline = Date.now() + (timeoutMs || 45000);
        return new Promise(function (resolve, reject) {
            (function poll() {
                S.api.info().then(resolve, function () {
                    if (Date.now() > deadline)
                        reject(new Error("the board did not come back"));
                    else setTimeout(poll, 1200);
                });
            })();
        });
    }

    // ---------------------------------------------------------------- nav --

    var ORDER = ["map", "lamps", "install", "versions", "device"];
    function go(name) {
        document.querySelectorAll(".navitem").forEach(function (b) {
            b.setAttribute("aria-current",
                           b.getAttribute("data-nav") === name ? "true" : "false");
        });
        document.querySelectorAll(".panel").forEach(function (p) {
            p.setAttribute("data-active",
                           p.getAttribute("data-panel") === name ? "true" : "false");
        });
    }

    // ---------------------------------------------------------------- map --

    function buildGrid() {
        var host = $("modules");
        var g = (S.config && S.config.geometry) || S.info.geometry;
        var wiring = wiringByChannel();
        var html = "";
        for (var m = 0; m < g.numModules; m++) {
            var lo = m * g.channelsPerModule;
            var hi = lo + g.channelsPerModule - 1;
            html += '<div class="modblock"><div class="modhead"><span>Module ' +
                    (m + 1) + '</span><span class="ch-range">ch ' + lo + "–" + hi +
                    '</span><i class="hr"></i></div><div class="chgrid">';
            for (var i = lo; i <= hi; i++) {
                var w = wiring[i];
                html += '<button class="cell" data-ch="' + i + '"' +
                        (!w ? ' data-unbound="true"' : "") + ">" +
                        '<i class="sticky"></i>' +
                        (w ? '<span class="lampno">' + w.lamp + "</span>" : "") +
                        '<i class="bulb"></i>' +
                        '<span class="cid">' + String(i).padStart(2, "0") +
                        "</span></button>";
            }
            html += "</div></div>";
        }
        host.innerHTML = html;
        S.cellEls = {};
        host.querySelectorAll(".cell").forEach(function (el) {
            S.cellEls[el.getAttribute("data-ch")] = {
                root: el,
                bulb: el.querySelector(".bulb"),
            };
            el.addEventListener("click", onCellClick);
        });
        selectChannel(S.selected);
    }

    function paintLive() {
        if (!S.live) return;
        var wiring = wiringByChannel();
        var n = Math.min(S.live.channelCount, channelCount());
        for (var ch = 0; ch < n; ch++) {
            var e = S.cellEls[ch];
            if (!e) continue;
            var level = S.live.samples[ch * 2] / 255;
            var flags = S.live.samples[ch * 2 + 1];
            var w = wiring[ch];
            var lamp = w ? lampEntry(w.lamp, false) : null;
            var rgb = hexColor(lamp && lamp.color ? colorHex(lamp.color)
                                                  : PRESETS[0].hex);
            e.bulb.style.background = "rgb(" + Math.round(rgb.r * level) + "," +
                Math.round(rgb.g * level) + "," + Math.round(rgb.b * level) + ")";
            e.bulb.style.boxShadow = level > 0.03
                ? "0 0 " + (5 + level * 15).toFixed(1) + "px " +
                  (level * 2.5).toFixed(1) + "px rgba(" + rgb.r + "," + rgb.g +
                  "," + rgb.b + "," + (level * 0.55).toFixed(3) + ")"
                : "none";
            var sticky = (flags & 1) ? "true" : "false";
            if (e.root.getAttribute("data-sticky") !== sticky)
                e.root.setAttribute("data-sticky", sticky);
        }
        if (String(S.selected) in S.cellEls) paintInspectorLive();
    }

    function onCellClick(ev) {
        var ch = parseInt(ev.currentTarget.getAttribute("data-ch"), 10);
        var wiring = wiringByChannel();

        if (S.learning && !wiring[ch]) {
            var want = parseInt($("learn-lamp").value, 10);
            if (isNaN(want) || want < 1 || want > 65535) {
                toast("Enter the lamp number you fired", "1 to 65535.", "warn");
                return;
            }
            var taken = (S.config.wiring || []).some(function (w) {
                return w.lamp === want;
            });
            if (taken) {
                toast("Lamp " + want + " is already bound",
                      "Unbind it first, or fire a different lamp.", "warn");
                return;
            }
            if (!S.config.wiring) S.config.wiring = [];
            // LED index defaults to the channel: the common build wires the
            // string in channel order, and the inspector can retune it.
            S.config.wiring.push({ channel: ch, lamp: want, ledIndex: ch });
            S.dirtyConfig = true;
            setDirty();
            buildGrid();
            selectChannel(ch);
            renderLamps();
            updateLearnHint();
            toast("Bound lamp " + want + " to channel " + ch,
                  "LED " + ch + " — name it in the inspector.", "ok");
            $("learn-lamp").value = want + 1;
            return;
        }
        selectChannel(ch);
    }

    function selectChannel(ch) {
        S.selected = ch;
        Object.keys(S.cellEls).forEach(function (key) {
            S.cellEls[key].root.setAttribute(
                "data-sel", String(key) === String(ch) ? "true" : "false");
        });
        renderInspector();
    }

    function updateLearnHint() {
        var bound = (S.config.wiring || []).length;
        var total = channelCount();
        $("learn-hint").textContent = S.learning
            ? (total - bound) + " of " + total + " channels unbound — fire a " +
              "lamp from the test card, then click the channel that lit."
            : "Off — turn on, fire a lamp from the test card, and click the " +
              "channel that lit.";
    }

    // ---------------------------------------------------------- inspector --

    function renderInspector() {
        var host = $("inspector");
        var ch = S.selected;
        var w = wiringByChannel()[ch];

        if (!w) {
            host.innerHTML = '<div class="insp-empty">Channel ' + ch +
                " is not bound to a lamp.<br>" +
                (S.learning ? "Fire a lamp and click the cell to bind it."
                            : "Turn on Learn wiring to bind it.") + "</div>";
            return;
        }

        var lamp = lampEntry(w.lamp, false) || {};
        var hex = colorHex(lamp.color);
        var cls = lamp.classLock || "DRIVE_CLASS_UNSPECIFIED";

        var swatches = PRESETS.map(function (p) {
            return '<button class="sw" data-hex="' + p.hex + '" title="' +
                   p.name + '" style="background:' + p.hex + '" aria-pressed="' +
                   (p.hex.toLowerCase() === hex.toLowerCase() ? "true" : "false") +
                   '"></button>';
        }).join("");

        var classes = CLASS_ORDER.map(function (k) {
            return '<button class="classopt" data-cls="' + k +
                   '" aria-pressed="' + (k === cls ? "true" : "false") + '">' +
                   "<span>" + CLASS_SHORT[k] + "</span></button>";
        }).join("");

        host.innerHTML =
            '<div class="insp-head"><i class="bulb" id="insp-bulb"></i>' +
            '<div class="who"><b>' + esc(lamp.name || "Lamp " + w.lamp) +
            "</b><small>lamp " + w.lamp + " · channel " + ch + " · LED " +
            w.ledIndex + "</small></div></div>" +
            '<div class="insp-body">' +
            '<div class="field"><label for="f-name">Name</label>' +
            '<input class="input" id="f-name" type="text" value="' +
            esc(lamp.name || "") + '" placeholder="as the manual calls it"></div>' +
            '<div class="field"><label>Colour</label><div class="swatches">' +
            swatches + '<input type="color" id="f-color" value="' + hex +
            '" title="custom"></div></div>' +
            '<div class="field"><label>Drive class</label>' +
            '<small class="lb-hint">Live: ' +
            '<span id="f-live-class">—</span>. Lock only what the profiler ' +
            "gets wrong.</small>" +
            '<div class="classrow">' + classes + "</div></div>" +
            '<div class="field"><label for="f-attack">Attack <span id="f-attack-v">' +
            (lamp.attackMs || 0) + '</span> ms (0 = default)</label>' +
            '<input class="slider" id="f-attack" type="range" min="0" max="120" value="' +
            (lamp.attackMs || 0) + '"></div>' +
            '<div class="field"><label for="f-decay">Decay <span id="f-decay-v">' +
            (lamp.decayMs || 0) + '</span> ms (0 = default)</label>' +
            '<input class="slider" id="f-decay" type="range" min="0" max="120" value="' +
            (lamp.decayMs || 0) + '"></div>' +
            '<div class="field"><label for="f-led">LED index</label>' +
            '<input class="input" id="f-led" type="number" min="-1" max="65534" value="' +
            w.ledIndex + '"></div>' +
            '<dl class="readout"><dt>Reconstructed</dt><dd id="ro-level">—</dd></dl>' +
            '<div class="btn-row" style="margin-top:12px;">' +
            '<button class="btn sm" id="f-preview">Preview on LED</button>' +
            '<button class="btn sm ghost" id="f-preview-stop">Stop</button>' +
            '<button class="btn sm ghost" id="f-unbind">Unbind channel</button>' +
            "</div></div>";

        function touchProfile() {
            S.dirtyProfile = true;
            setDirty();
        }

        $("f-name").addEventListener("input", function () {
            lampEntry(w.lamp, true).name = $("f-name").value;
            touchProfile();
            renderLamps();
        });
        host.querySelectorAll(".sw").forEach(function (b) {
            b.addEventListener("click", function () {
                lampEntry(w.lamp, true).color = hexColor(b.getAttribute("data-hex"));
                touchProfile();
                renderInspector();
                renderLamps();
            });
        });
        $("f-color").addEventListener("change", function () {
            lampEntry(w.lamp, true).color = hexColor($("f-color").value);
            touchProfile();
            renderInspector();
            renderLamps();
        });
        host.querySelectorAll(".classopt").forEach(function (b) {
            b.addEventListener("click", function () {
                lampEntry(w.lamp, true).classLock = b.getAttribute("data-cls");
                touchProfile();
                renderInspector();
                renderLamps();
            });
        });
        $("f-attack").addEventListener("input", function () {
            lampEntry(w.lamp, true).attackMs = +$("f-attack").value;
            $("f-attack-v").textContent = $("f-attack").value;
            touchProfile();
        });
        $("f-decay").addEventListener("input", function () {
            lampEntry(w.lamp, true).decayMs = +$("f-decay").value;
            $("f-decay-v").textContent = $("f-decay").value;
            touchProfile();
        });
        $("f-led").addEventListener("change", function () {
            w.ledIndex = parseInt($("f-led").value, 10);
            if (isNaN(w.ledIndex)) w.ledIndex = -1;
            S.dirtyConfig = true;
            setDirty();
        });
        $("f-preview").addEventListener("click", function () {
            var lampNow = lampEntry(w.lamp, false) || {};
            var rgb = hexColor(colorHex(lampNow.color));
            S.api.colorTest(w.ledIndex, rgb, 255, 0).then(function (r) {
                toast("Previewing on LED " + w.ledIndex, r.message, "ok");
            }).catch(function (e) { toast("Preview failed", e.message, "crit"); });
        });
        $("f-preview-stop").addEventListener("click", function () {
            S.api.colorTestClear().catch(function () {});
        });
        $("f-unbind").addEventListener("click", function () {
            S.config.wiring = S.config.wiring.filter(function (x) {
                return x.channel !== ch;
            });
            S.dirtyConfig = true;
            setDirty();
            buildGrid();
            renderLamps();
            updateLearnHint();
            toast("Channel " + ch + " unbound",
                  "Lamp " + w.lamp + "'s styling stays in the profile.", "");
        });
    }

    function paintInspectorLive() {
        if (!S.live) return;
        var ch = S.selected;
        if (ch * 2 + 1 >= S.live.samples.length) return;
        var level = S.live.samples[ch * 2];
        var flags = S.live.samples[ch * 2 + 1];
        var ro = $("ro-level");
        if (ro) ro.textContent = Math.round((level / 255) * 100) + "%";
        var lc = $("f-live-class");
        if (lc) lc.textContent = CLASS_BY_VALUE[(flags >> 2) & 7];
        var bulb = $("insp-bulb");
        if (bulb) {
            var w = wiringByChannel()[ch];
            var lamp = w ? lampEntry(w.lamp, false) : null;
            var rgb = hexColor(colorHex(lamp && lamp.color));
            var L = level / 255;
            bulb.style.background = "rgb(" + Math.round(rgb.r * L) + "," +
                Math.round(rgb.g * L) + "," + Math.round(rgb.b * L) + ")";
        }
    }

    // -------------------------------------------------------------- lamps --

    function renderLamps() {
        var wiring = ((S.config && S.config.wiring) || []).slice()
            .sort(function (a, b) { return a.lamp - b.lamp; });
        $("lamps-body").innerHTML = wiring.map(function (w) {
            var lamp = lampEntry(w.lamp, false) || {};
            var hex = colorHex(lamp.color);
            return "<tr data-ch='" + w.channel + "'><td>" + w.lamp + "</td><td>" +
                   esc(lamp.name || "—") + "</td><td>" + w.channel + "</td><td>" +
                   w.ledIndex + "</td><td><i class='swatch' style='background:" +
                   hex + "'></i> " + hex + "</td><td>" +
                   CLASS_SHORT[lamp.classLock || "DRIVE_CLASS_UNSPECIFIED"] +
                   "</td><td>" + (lamp.attackMs || "·") + "</td><td>" +
                   (lamp.decayMs || "·") + "</td></tr>";
        }).join("") ||
            "<tr><td colspan='8' style='color:var(--dim)'>Nothing bound yet — " +
            "use Learn wiring on the Map panel.</td></tr>";
        $("lamps-body").querySelectorAll("tr[data-ch]").forEach(function (tr) {
            tr.addEventListener("click", function () {
                go("map");
                selectChannel(parseInt(tr.getAttribute("data-ch"), 10));
            });
        });
        $("profile-note").textContent = shortfallText();
    }

    /// WEBUI.md §8.2: report the shortfall, never block on it.
    function shortfallText() {
        var lamps = (S.profile && S.profile.lamps) || [];
        if (!lamps.length) return "";
        var wired = {};
        ((S.config && S.config.wiring) || []).forEach(function (w) {
            wired[w.lamp] = true;
        });
        var matched = lamps.filter(function (l) { return wired[l.lamp]; }).length;
        return matched + " of " + lamps.length +
               " lamps in this profile match your wiring.";
    }

    // ------------------------------------------------------------ install --

    function renderInstall() {
        var c = S.config;
        function card(title, rows) {
            return '<div class="card"><h3 class="card-title">' + title + "</h3>" +
                rows.map(function (r) {
                    return '<dl class="readout"><dt>' + r[0] + "</dt><dd>" +
                           esc(r[1]) + "</dd></dl>";
                }).join("") + "</div>";
        }
        // Absence means "inherit the build default" throughout the document
        // (the append-only rule) — showing an absent value as 0 Hz would be
        // reporting a configuration nobody wrote.
        var kDefault = "build default";
        function v(section, field, fmt) {
            if (!section || section[field] === undefined ||
                section[field] === null || section[field] === 0)
                return kDefault;
            return fmt ? fmt(section[field]) : String(section[field]);
        }
        var g = c.geometry || {}, sc = c.scan, p = c.pins, r = c.render;
        var gpio = function (x) { return "GPIO " + x; };
        $("install-cards").innerHTML =
            card("Geometry", [
                ["Modules", g.numModules || 0],
                ["Channels per module", g.channelsPerModule || 0],
                ["Total channels", channelCount()],
                ["LEDs in string", g.ledCount || 0]]) +
            card("Pins", [
                ["CLK", v(p, "clk", gpio)], ["/PL", v(p, "pl", gpio)],
                ["DATA", v(p, "data", gpio)], ["LED string", v(p, "led", gpio)]]) +
            card("Scan", [
                ["Sample rate", v(sc, "sampleRateHz", function (x) { return x + " Hz"; })],
                ["Chain clock", v(sc, "spiHz", function (x) { return x + " Hz"; })],
                ["SPI mode", v(sc, "spiMode")],
                ["Active polarity", sc ? (sc.activeLow ? "low" : "high") : kDefault]]) +
            card("Render", [
                ["Refresh", v(r, "refreshHz", function (x) { return x + " Hz"; })],
                ["Gamma", v(r, "gammaX100", function (x) { return (x / 100).toFixed(2); })],
                ["Brightness cap", v(r, "brightnessCap", function (x) { return x + " / 255"; })],
                ["Colour order", r && r.colorOrder ? String(r.colorOrder)
                    .replace("COLOR_ORDER_", "").replace("UNSPECIFIED", "GRB")
                    : kDefault]]);
        $("rail-foot").innerHTML =
            "scan <b>" + (sc && sc.sampleRateHz
                ? (sc.sampleRateHz / 1000).toFixed(1) + " kHz" : "default") +
            "</b><br>render <b>" + (r && r.refreshHz ? r.refreshHz + " Hz" : "default") +
            "</b><br>live <b><span id='foot-rate'>—</span></b>";
    }

    // -------------------------------------------------------- files & JSON --

    function download(name, text) {
        var a = document.createElement("a");
        a.href = URL.createObjectURL(new Blob([text], { type: "application/json" }));
        a.download = name;
        a.click();
        setTimeout(function () { URL.revokeObjectURL(a.href); }, 5000);
    }

    function pickFile(accept) {
        return new Promise(function (resolve, reject) {
            var input = $("file-input");
            input.accept = accept || "";
            input.value = "";
            input.onchange = function () {
                var f = input.files[0];
                if (!f) return reject(new Error("no file"));
                resolve(f);
            };
            input.click();
        });
    }

    /// The export stamps the author (FR-REG-1) — the device stores the
    /// handle, the browser does the stamping, the cloud does verification.
    function stampedProfile() {
        var p = JSON.parse(JSON.stringify(S.profile || {}));
        var handle = ($("author-input").value || "").trim();
        if (handle) p.author = { handle: handle, verified: false };
        return p;
    }

    // ------------------------------------------------------------- apply --

    function applyAll() {
        var btn = $("apply-btn");
        btn.disabled = true;
        btn.textContent = "Applying…";
        var steps = Promise.resolve();
        if (S.dirtyConfig)
            steps = steps.then(function () {
                toast("Writing install config", "The board restarts to apply it.", "");
                return S.api.putConfig(S.config).then(function () {
                    return waitForReboot();
                });
            });
        if (S.dirtyProfile)
            steps = steps.then(function () {
                toast("Writing machine profile", "The board restarts to apply it.", "");
                return S.api.putProfile(stampedProfile()).then(function () {
                    return waitForReboot();
                });
            });
        steps.then(function () {
            S.dirtyConfig = S.dirtyProfile = false;
            setDirty();
            btn.textContent = "Apply & restart";
            var sf = shortfallText();
            toast("Applied", sf || "The board is back.", "ok");
            return refresh();
        }).catch(function (e) {
            btn.disabled = false;
            btn.textContent = "Apply & restart";
            toast("Apply failed", e.message, "crit");
        });
    }

    // ------------------------------------------------------------- device --

    function renderDevice() {
        var i = S.info;
        $("device-identity").innerHTML = [
            ["Device ID", i.deviceId], ["Firmware", i.firmwareVersion],
            ["API version", i.apiVersion], ["Build", i.buildId],
            ["Running slot", i.runningSlot],
        ].map(function (r) {
            return '<dl class="readout"><dt>' + r[0] + "</dt><dd>" +
                   esc(r[1]) + "</dd></dl>";
        }).join("");
        $("author-input").value = i.authorHandle || "";
        $("brand-device").textContent = i.deviceId.slice(-6);
        renderFw();
    }

    function renderProfiler(ps) {
        $("profiler-status").innerHTML = [
            ["State", String(ps.state).replace("PROFILER_STATE_", "").toLowerCase()],
            ["Window", ps.windowMs + " ms (" + ps.windowFrames + " frames)"],
            ["Progress", ps.framesObserved + " / " + ps.windowFrames],
            ["Passes since boot", ps.passes],
        ].map(function (r) {
            return '<dl class="readout"><dt>' + r[0] + "</dt><dd>" +
                   esc(r[1]) + "</dd></dl>";
        }).join("");
    }

    function renderFw() {
        var i = S.info;
        $("fw-status").innerHTML = [
            ["Running", i.firmwareVersion + " (" + i.buildId + ")"],
            ["Slot", i.runningSlot],
            ["Staged image", i.otaPending
                ? "yes — " + i.otaConfirmSecondsLeft + " s to confirm" : "none"],
        ].map(function (r) {
            return '<dl class="readout"><dt>' + r[0] + "</dt><dd>" +
                   esc(r[1]) + "</dd></dl>";
        }).join("");
        $("fw-discard").hidden = !i.otaPending;
        var box = $("fw-armbox");
        box.hidden = !i.otaPending;
        box.setAttribute("data-armed", i.otaPending ? "true" : "false");
        if (i.otaPending)
            $("fw-armtext").textContent =
                "Staged. Press the button on the board within " +
                i.otaConfirmSecondsLeft + " s to boot it — only the button can.";
    }

    /// After an upload: watch /info once a second. Three exits — confirmed
    /// (the board vanishes and returns with a different build), expired
    /// (pending drops with the same build), or discarded from here.
    function watchStaged(prevBuild, prevSlot) {
        if (S.fwWatch) clearInterval(S.fwWatch);
        S.fwWatch = setInterval(function () {
            S.api.info().then(function (info) {
                S.info = info;
                renderFw();
                if (!info.otaPending) {
                    clearInterval(S.fwWatch);
                    S.fwWatch = null;
                    if (info.buildId !== prevBuild || info.runningSlot !== prevSlot)
                        toast("Firmware applied",
                              "Now " + info.firmwareVersion + " (" + info.buildId +
                              ") from " + info.runningSlot + ".", "ok");
                    else
                        toast("Confirmation window closed",
                              "The staged image was discarded.", "warn");
                }
            }, function () {
                // Mid-restart. Keep polling; the interval survives failures.
                $("fw-armtext").textContent = "Restarting into the new image…";
            });
        }, 1000);
    }

    // --------------------------------------------------------------- boot --

    function wireStatic() {
        document.querySelectorAll(".navitem").forEach(function (b) {
            b.addEventListener("click", function () {
                go(b.getAttribute("data-nav"));
            });
        });
        document.addEventListener("keydown", function (e) {
            if (e.target.matches("input, textarea, select")) return;
            var n = parseInt(e.key, 10);
            if (n >= 1 && n <= ORDER.length) go(ORDER[n - 1]);
        });

        $("learn-toggle").addEventListener("change", function () {
            S.learning = $("learn-toggle").checked;
            $("learnbar").setAttribute("data-on", S.learning ? "true" : "false");
            $("learn-controls").style.display = S.learning ? "flex" : "none";
            updateLearnHint();
            renderInspector();
        });

        $("apply-btn").addEventListener("click", applyAll);

        $("config-export").addEventListener("click", function () {
            download("install-config.json",
                     S.api.toJson("InstallConfig", S.config));
            toast("Exported install-config.json",
                  "Credentials are not in it, at any privacy level.", "ok");
        });
        $("config-import").addEventListener("click", function () {
            pickFile(".json").then(function (f) { return f.text(); })
                .then(function (text) {
                    S.config = S.api.fromJson("InstallConfig", text);
                    S.dirtyConfig = true;
                    setDirty();
                    buildGrid();
                    renderLamps();
                    renderInstall();
                    toast("Install config loaded", "Apply to write it to the board.", "ok");
                }).catch(function (e) {
                    toast("Import failed", e.message, "crit");
                });
        });

        $("profile-export").addEventListener("click", function () {
            download("machine-profile.json",
                     S.api.toJson("MachineProfile", stampedProfile()));
            toast("Exported machine-profile.json", shortfallText(), "ok");
        });
        $("profile-import").addEventListener("click", function () {
            pickFile(".json").then(function (f) { return f.text(); })
                .then(function (text) {
                    S.profile = S.api.fromJson("MachineProfile", text);
                    S.dirtyProfile = true;
                    setDirty();
                    renderLamps();
                    renderInspector();
                    toast("Profile loaded", shortfallText() +
                          " Apply to write it to the board.", "ok");
                }).catch(function (e) {
                    toast("Import failed", e.message, "crit");
                });
        });

        $("version-save").addEventListener("click", function () {
            var now = new Date().toISOString();
            var v = {
                schema: 1,
                versionId: now.replace(/[-:TZ.]/g, "").slice(0, 14),
                label: "",
                created: now,
                install: S.config,
                profile: stampedProfile(),
            };
            download("pinled-version-" + v.versionId + ".json",
                     S.api.toJson("Version", v));
            toast("Version saved", "Install config and profile, one file.", "ok");
        });
        $("version-load").addEventListener("click", function () {
            pickFile(".json").then(function (f) { return f.text(); })
                .then(function (text) {
                    var v = S.api.fromJson("Version", text);
                    if (v.install) { S.config = v.install; S.dirtyConfig = true; }
                    if (v.profile) { S.profile = v.profile; S.dirtyProfile = true; }
                    setDirty();
                    buildGrid();
                    renderLamps();
                    renderInstall();
                    var note = $("version-note");
                    note.hidden = false;
                    note.textContent = "Loaded " + (v.label || v.versionId || "version") +
                        (v.created ? " (" + v.created + ")" : "") + ". " +
                        shortfallText() + " Apply & restart to make it live.";
                }).catch(function (e) {
                    toast("Load failed", e.message, "crit");
                });
        });

        // The write and the follow-up refresh are separate failures and must
        // say so: a merged catch once reported "Rejected" for a store the
        // device had already committed, because the REFRESH was what failed.
        $("author-save").addEventListener("click", function () {
            var h = $("author-input").value.trim();
            if (!h) return toast("Type a handle first", "Or use Clear.", "warn");
            S.api.setAuthor(h).then(function (r) {
                toast("Author handle stored", r.message, "ok");
                refreshInfo().catch(function () {
                    toast("Stored — but the page could not refresh",
                          "The device has it; reload to see it.", "warn");
                });
            }, function (e) { toast("Store failed", e.message, "crit"); });
        });
        $("author-clear").addEventListener("click", function () {
            S.api.clearAuthor().then(function () {
                $("author-input").value = "";
                toast("Author handle cleared", "", "ok");
                refreshInfo().catch(function () {});
            }, function (e) { toast("Clear failed", e.message, "crit"); });
        });

        $("profiler-rearm").addEventListener("click", function () {
            S.api.rearmProfiler().then(function (ps) {
                renderProfiler(ps);
                toast("Classification pass started",
                      ps.windowMs + " ms observation window.", "ok");
            }).catch(function (e) { toast("Could not re-arm", e.message, "crit"); });
        });

        var labPin = function () {
            S.api.colorTest(parseInt($("lab-led").value, 10) || 0,
                            hexColor($("lab-color").value),
                            parseInt($("lab-level").value, 10) || 255,
                            parseInt($("lab-gamma").value, 10) || 0)
                .then(function (r) { toast("Pinned", r.message, "ok"); })
                .catch(function (e) { toast("Colour lab", e.message, "crit"); });
        };
        $("lab-pin").addEventListener("click", labPin);
        // Sliders re-pin live, so the sweep is a drag rather than a loop of
        // clicks — the whole point of the lab.
        $("lab-gamma").addEventListener("input", function () {
            $("lab-gamma-v").textContent =
                (parseInt($("lab-gamma").value, 10) / 100).toFixed(2);
        });
        $("lab-level").addEventListener("input", function () {
            $("lab-level-v").textContent = $("lab-level").value;
        });
        $("lab-gamma").addEventListener("change", labPin);
        $("lab-level").addEventListener("change", labPin);
        $("lab-color").addEventListener("change", labPin);
        $("lab-clear").addEventListener("click", function () {
            S.api.colorTestClear().then(function () {
                toast("Override cleared", "", "ok");
            }).catch(function (e) { toast("Colour lab", e.message, "crit"); });
        });

        $("fw-pick").addEventListener("click", function () {
            var bar = $("fw-progress");
            var fill = bar.querySelector("i");
            var text = $("fw-progress-text");
            pickFile(".bin").then(function (f) {
                bar.hidden = false;
                text.hidden = false;
                fill.style.width = "0%";
                text.textContent = "Hashing " + f.name + "…";
                // Background polling pauses for the duration: an upload
                // shares the board's one radio and one httpd task, and every
                // concurrent request measurably slows it (51 -> 10 KB/s on
                // the bench).
                S.uploading = true;
                return f.arrayBuffer().then(function (buf) {
                    return S.api.otaUpload(new Uint8Array(buf),
                        function (loaded, total) {
                            var pct = Math.round((loaded / total) * 100);
                            fill.style.width = pct + "%";
                            text.textContent = "Uploading " + f.name + " — " +
                                Math.round(loaded / 1024) + " of " +
                                Math.round(total / 1024) + " KB (" + pct + "%)";
                        });
                });
            }).then(function (r) {
                S.uploading = false;
                bar.hidden = true;
                text.hidden = true;
                toast("Staged", r.message, "ok");
                var prev = { build: S.info.buildId, slot: S.info.runningSlot };
                return refreshInfo().then(function () {
                    watchStaged(prev.build, prev.slot);
                });
            }).catch(function (e) {
                S.uploading = false;
                bar.hidden = true;
                text.hidden = true;
                if (e.message !== "no file") toast("Upload failed", e.message, "crit");
            });
        });
        $("fw-discard").addEventListener("click", function () {
            S.api.otaDiscard().then(function () {
                toast("Staged image discarded", "", "ok");
                return refreshInfo();
            }).catch(function (e) { toast("Discard failed", e.message, "crit"); });
        });
    }

    function refreshInfo() {
        return S.api.info().then(function (info) {
            S.info = info;
            renderDevice();
            return info;
        });
    }

    function refresh() {
        return Promise.all([
            S.api.info(), S.api.config(), S.api.profile(), S.api.profiler(),
        ]).then(function (r) {
            S.info = r[0];
            S.config = r[1];
            S.profile = r[2];
            buildGrid();
            renderLamps();
            renderInstall();
            renderDevice();
            renderProfiler(r[3]);
            updateLearnHint();
            if (S.info.otaPending)
                watchStaged(S.info.buildId, S.info.runningSlot);
        });
    }

    function startLive() {
        var badge = $("live-badge");
        S.liveSock = S.api.openLive(function (frame) {
            S.live = frame;
            S.frames += 1;
            paintLive();
        }, function (state) {
            badge.textContent = state;
            badge.className = "badge " + (state === "live" ? "live" : "down");
        });
        setInterval(function () {
            var el = $("foot-rate");
            if (el) el.textContent = (S.frames - S.framesAt) + " Hz";
            S.framesAt = S.frames;
        }, 1000);
    }

    function boot(base) {
        try {
            S.api = new pinledApi.PinledApi(base);
        } catch (e) {
            return Promise.reject(e);
        }
        return refresh().then(function () {
            $("connect").hidden = true;
            $("app").hidden = false;
            wireStatic();
            startLive();
            setInterval(function () {
                if (S.uploading) return;
                if (document.querySelector('[data-panel="device"][data-active="true"]'))
                    S.api.profiler().then(renderProfiler, function () {});
            }, 2000);
        });
    }

    // Served by the device (or any http host): same origin, no ceremony.
    // Opened from disk (file://): ask for the address once and remember it.
    if (location.protocol === "file:") {
        $("connect").hidden = false;
        var saved = localStorage.getItem("pinled-device");
        if (saved) $("connect-addr").value = saved;
        var tryConnect = function () {
            var addr = $("connect-addr").value.trim().replace(/\/+$/, "");
            if (!/^https?:\/\//.test(addr)) addr = "http://" + addr;
            $("connect-err").textContent = "";
            boot(addr).then(function () {
                localStorage.setItem("pinled-device", addr);
            }).catch(function (e) {
                $("connect-err").textContent =
                    "No pinled at " + addr + " — " + e.message;
            });
        };
        $("connect-btn").addEventListener("click", tryConnect);
        $("connect-addr").addEventListener("keydown", function (e) {
            if (e.key === "Enter") tryConnect();
        });
        if (saved) tryConnect();
    } else {
        // location.origin, never "": the shell injects <base href="<bundle>">
        // so RELATIVE URLs — including fetch("/api/v1/...") — resolve to the
        // S3 bundle, not the device. An explicit absolute origin is immune.
        // location itself is unaffected by <base>, so it still names the
        // device that served the shell.
        boot(location.origin).catch(function (e) {
            // An https origin that has no API is the BUNDLE HOST — someone
            // opened the S3 copy directly (or followed a link to it). No
            // connection can ever work from here: an https page cannot call
            // an http device (mixed content, unconditional). Say that, with
            // directions, instead of relaying S3's AccessDenied XML.
            var bundleHost = location.protocol === "https:";
            document.body.innerHTML = bundleHost
                ? "<div class='connect'><div class='card'>" +
                  "<h3 class='card-title'>This is the pinled app bundle — " +
                  "not your controller</h3>" +
                  "<p class='prose'>You've reached the cloud copy of the app. " +
                  "It can't connect to a controller from here: this page is " +
                  "<code>https://</code>, your controller's API is " +
                  "<code>http://</code> on your own network, and browsers " +
                  "never allow that mix. This is by design — the app is meant " +
                  "to be served <i>by the controller</i>.</p>" +
                  "<p class='prose'><b>To use the app:</b> browse to " +
                  "<a href='http://pinled.local/'>http://pinled.local/</a> " +
                  "or the numeric address the setup page reported (also " +
                  "printed in the boot log). The controller serves this same " +
                  "app from its own address, always current.</p>" +
                  "<p class='prose'><b>No controller yet, or offline?</b> " +
                  "The standalone copy works from disk: save this site's " +
                  "files and open <code>index.html</code> locally, or see " +
                  "the <a href='api/index.html'>API reference</a> — which " +
                  "does work from here.</p>" +
                  "</div></div>"
                : "<div class='connect'><div class='card'><h3 class='card-title'>" +
                  "Could not reach the device API</h3><p class='prose'>" +
                  esc(e.message) + "</p></div></div>";
        });
    }
})();
