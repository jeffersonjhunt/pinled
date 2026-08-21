#!/usr/bin/env node
/**
 * @file browser_check.js
 * @brief The SPA driven by real browser engines, against a real board.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * api_check.js proves the wire under node; this proves it under the rules
 * only browsers enforce — CORS, preflights, connection pools, `hidden`
 * versus CSS. It exists because both classes of bug shipped: node passed
 * while WebKit refused a doubled Allow-Origin header, and a display rule
 * kept a working app behind an opaque overlay. Node cannot see either.
 *
 *   npm i playwright && npx playwright install chromium webkit --with-deps
 *   node test/web/browser_check.js http://<device> [webkit|chromium|both]
 *
 * WebKit is the engine that matters most: the bench browser is Safari.
 * Touches the same state api_check.js does (a throwaway author handle,
 * restored; a colour-lab override, cleared).
 */
"use strict";

const path = require("path");

let failures = 0;
function check(name, ok, detail) {
    console.log((ok ? "  ok    " : "  FAIL  ") + name +
                (ok || !detail ? "" : " — " + detail));
    if (!ok) failures += 1;
}

async function run(name, engine, base) {
    console.log("=== " + name + " ===");
    const browser = await engine.launch();
    const page = await browser.newPage();
    const netFail = [];
    page.on("requestfailed", (r) => netFail.push(
        r.method() + " " + r.url().replace(base, "") + " :: " +
        ((r.failure() || {}).errorText || "?")));

    const index = path.resolve(__dirname, "..", "..", "web", "index.html");
    await page.goto("file://" + index);

    // The file:// path: connect screen, then the app becomes visible —
    // visible as in rendered, not merely hidden=false (the overlay bug).
    await page.fill("#connect-addr", base);
    await page.click("#connect-btn");
    const appVisible = await page.waitForSelector("#app:not([hidden])",
        { timeout: 20000 }).then(() => true, () => false);
    check("app appears after connect", appVisible);
    check("connect overlay actually disappears",
          !(await page.locator("#connect").isVisible()));
    await page.waitForTimeout(1500);
    check("live badge reaches 'live'",
          (await page.locator("#live-badge").textContent()) === "live");

    // A write with a follow-up refresh: the author handle. Remember what is
    // stored, store a marker, expect the SUCCESS toast, restore.
    const before = await page.locator("#author-input").inputValue();
    await page.click('[data-nav="device"]');
    await page.fill("#author-input", "browser-check");
    await page.click("#author-save");
    await page.waitForTimeout(2000);
    let toasts = await page.$$eval(".toast", (els) =>
        els.map((e) => e.textContent));
    check("author store succeeds in-browser",
          toasts.some((t) => t.includes("Author handle stored")),
          JSON.stringify(toasts));
    if (before) {
        await page.fill("#author-input", before);
        await page.click("#author-save");
    } else {
        await page.click("#author-clear");
    }
    await page.waitForTimeout(1500);

    // The colour lab: pin, expect the device's own message back, clear.
    await page.click("#lab-pin");
    await page.waitForTimeout(1500);
    toasts = await page.$$eval(".toast", (els) => els.map((e) => e.textContent));
    check("colour lab pin succeeds in-browser",
          toasts.some((t) => t.includes("Pinned")), JSON.stringify(toasts));
    await page.click("#lab-clear");
    await page.waitForTimeout(1000);

    check("no failed network requests", netFail.length === 0,
          netFail.join(" | "));
    await browser.close();
}

(async () => {
    const base = (process.argv[2] || "").replace(/\/+$/, "");
    if (!base) {
        console.error("usage: node browser_check.js http://<device> [webkit|chromium|both]");
        process.exit(2);
    }
    const which = process.argv[3] || "both";
    const pw = require("playwright");
    if (which === "webkit" || which === "both")
        await run("WEBKIT", pw.webkit, base);
    if (which === "chromium" || which === "both")
        await run("CHROMIUM", pw.chromium, base);
    console.log(failures ? "\n" + failures + " FAILED" : "\nall checks passed");
    process.exit(failures ? 1 : 0);
})().catch((e) => {
    console.error("aborted: " + e.message);
    process.exit(1);
});
