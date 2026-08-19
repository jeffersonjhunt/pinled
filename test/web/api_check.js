#!/usr/bin/env node
/**
 * @file api_check.js
 * @brief The SPA's API layer, exercised against a real board.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The browser half of the wire is the risky half — the firmware side is
 * already covered by pbtool and the host suites — so this runs the SPA's own
 * api.js under node, against live hardware, before any UI sits on top of it.
 * Same spirit as tools/rearm_check.py: a scripted acceptance test.
 *
 *   node test/web/api_check.js http://192.168.3.129
 *
 * Read-mostly by design: it stores and clears a throwaway author handle
 * (restoring what was there), and touches nothing else that persists.
 */
"use strict";

const path = require("path");
const crypto = require("crypto");

const WEB = path.join(__dirname, "..", "..", "web");
globalThis.window = globalThis; // the classic scripts attach to window
globalThis.protobuf = require(path.join(WEB, "js", "vendor", "protobuf.min.js"));
require(path.join(WEB, "js", "gen", "proto.js"));
const { PinledApi, sha256, camelKeys } = require(path.join(WEB, "js", "api.js"));

let failures = 0;
function check(name, ok, detail) {
    console.log((ok ? "  ok    " : "  FAIL  ") + name + (ok || !detail ? "" : " — " + detail));
    if (!ok) failures += 1;
}

async function main() {
    const base = process.argv[2];
    if (!base) {
        console.error("usage: node api_check.js http://<device>");
        return 2;
    }

    // --- pure parts first: no board needed to fail these -----------------
    check("sha256 of empty input",
          sha256(new Uint8Array(0)) ===
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("sha256 of 'abc'",
          sha256(new TextEncoder().encode("abc")) ===
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        // A multi-block, odd-length input against node's own implementation.
        const blob = crypto.randomBytes(70000);
        check("sha256 of 70000 random bytes matches node:crypto",
              sha256(new Uint8Array(blob)) ===
              crypto.createHash("sha256").update(blob).digest("hex"));
    }
    check("snake_case keys import as camelCase",
          JSON.stringify(camelKeys({ class_lock: "X", lamps: [{ attack_ms: 1 }] })) ===
          '{"classLock":"X","lamps":[{"attackMs":1}]}');

    const api = new PinledApi(base);

    // --- reads ------------------------------------------------------------
    const info = await api.info();
    check("GET /info decodes", info.apiVersion === 1 && info.deviceId.length === 12,
          JSON.stringify(info));
    console.log("        device " + info.deviceId + ", fw " + info.firmwareVersion +
                ", build " + info.buildId + ", slot " + info.runningSlot);

    const cfg = await api.config();
    check("GET /config decodes with geometry",
          cfg.geometry && cfg.geometry.numModules >= 1, JSON.stringify(cfg.geometry));

    const prof = await api.profile();
    check("GET /profile decodes", typeof prof === "object");

    const ps = await api.profiler();
    check("GET /profiler decodes", typeof ps.state === "string", JSON.stringify(ps));

    // --- the document round trip -------------------------------------------
    // The SPA edits a decoded config and re-encodes it; the board must parse
    // the result identically. Encode the object we read and decode it again:
    // the two objects must match field for field.
    {
        const bytes = api.encode("InstallConfig", cfg);
        const again = api.T.InstallConfig.toObject(
            api.T.InstallConfig.decode(bytes), { defaults: true, enums: String });
        check("InstallConfig survives encode/decode",
              JSON.stringify(again) === JSON.stringify(cfg));
    }
    {
        const text = api.toJson("MachineProfile", prof);
        const back = api.fromJson("MachineProfile", text);
        check("MachineProfile survives the JSON file round trip",
              JSON.stringify(back) === JSON.stringify(prof));
    }

    // --- author handle: store, read back, restore ---------------------------
    {
        const before = info.authorHandle;
        const r1 = await api.setAuthor("api-check");
        const mid = await api.info();
        check("PUT /author stores", r1.accepted && mid.authorHandle === "api-check",
              JSON.stringify(r1));
        let r2;
        if (before) {
            r2 = await api.setAuthor(before);
        } else {
            r2 = await api.clearAuthor();
        }
        const after = await api.info();
        check("author handle restored", r2.accepted && after.authorHandle === before);

        let rejected = false;
        try { await api.setAuthor(" edge-space"); }
        catch (e) { rejected = /400/.test(e.message); }
        check("an invalid handle is a 400", rejected);
    }

    // --- the live socket ----------------------------------------------------
    // node 20 has no global WebSocket; the check degrades to a warning there
    // rather than pretending the socket was exercised.
    if (typeof globalThis.WebSocket === "function") {
        const frames = [];
        await new Promise((resolve) => {
            const live = api.openLive((f) => {
                frames.push(f);
                if (frames.length >= 5) { live.close(); resolve(); }
            });
            setTimeout(() => { live.close(); resolve(); }, 5000);
        });
        const f = frames[frames.length - 1];
        check("live socket delivers frames", frames.length >= 5,
              frames.length + " frames");
        check("live frame carries 2 bytes per channel",
              f && f.samples.length === f.channelCount * 2,
              f ? f.channelCount + " ch, " + f.samples.length + " bytes" : "none");
        check("live sequence advances",
              frames.length >= 2 &&
              frames[frames.length - 1].seq > frames[0].seq);
    } else {
        console.log("  note  no WebSocket in this node; live socket not exercised");
    }

    console.log(failures ? "\n" + failures + " FAILED" : "\nall checks passed");
    return failures ? 1 : 0;
}

main().then((rc) => process.exit(rc), (e) => {
    console.error("aborted: " + (e && e.message));
    process.exit(1);
});
