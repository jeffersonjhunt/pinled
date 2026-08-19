/**
 * @file api.js
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The device API client: protobuf over fetch, and the live socket.
 * @version 0.1.0
 * @date 2026-08-19
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * A classic script, not a module: the standalone bundle runs from file://
 * (FR-UI-8), where ES modules are blocked by CORS. The schema text arrives
 * the same way (js/gen/proto.js) and protobuf.js parses it at load — one
 * schema authority, no generated bindings to drift (FR-CFG-13).
 *
 * Everything here is also loadable under node for the wire-level tests in
 * test/web/, which run against a real board. The DOM never appears in this
 * file for exactly that reason.
 */
/* global protobuf */
(function (global) {
    "use strict";

    var CONTENT_TYPE = "application/x-protobuf";

    // ------------------------------------------------------------ schema --

    function makeRoot() {
        var src = global.PINLED_PROTO;
        if (!src)
            throw new Error("js/gen/proto.js missing — run tools/gen_proto_js.py");
        return protobuf.parse(src).root;
    }

    // proto3's canonical JSON uses camelCase keys, which is what protobuf.js
    // emits and expects. pbtool and hand-written files use the .proto's
    // snake_case names — equally legal in canonical JSON, but fromObject()
    // does not translate. This shim does, so an imported file may use either.
    function camelKeys(v) {
        if (Array.isArray(v)) return v.map(camelKeys);
        if (v === null || typeof v !== "object") return v;
        var out = {};
        Object.keys(v).forEach(function (k) {
            var ck = k.replace(/_([a-z0-9])/g, function (_, c) {
                return c.toUpperCase();
            });
            out[ck] = camelKeys(v[k]);
        });
        return out;
    }

    // -------------------------------------------------------------- sha256 --
    //
    // Pure JS, because the device origin is plain http:// and file:// — not a
    // secure context, so crypto.subtle does not exist there (WEBUI.md §1).
    // The OTA path needs a digest for X-Image-SHA256 (FR-OTA-3).

    var K = [];
    var H0 = [];
    (function () {
        function frac(x) { return Math.floor((x - Math.floor(x)) * 0x100000000); }
        var primes = [], n = 2;
        while (primes.length < 64) {
            var isP = true;
            for (var i = 0; i < primes.length && primes[i] * primes[i] <= n; ++i)
                if (n % primes[i] === 0) { isP = false; break; }
            if (isP) primes.push(n);
            n += 1;
        }
        for (var j = 0; j < 64; ++j) K[j] = frac(Math.cbrt(primes[j]));
        for (var k = 0; k < 8; ++k) H0[k] = frac(Math.sqrt(primes[k]));
    })();

    function sha256(bytes) {
        var len = bytes.length;
        var bitLen = len * 8;
        var padded = new Uint8Array((((len + 8) >> 6) + 1) << 6);
        padded.set(bytes);
        padded[len] = 0x80;
        var dv = new DataView(padded.buffer);
        dv.setUint32(padded.length - 8, Math.floor(bitLen / 0x100000000));
        dv.setUint32(padded.length - 4, bitLen >>> 0);

        var h = H0.slice();
        var w = new Int32Array(64);
        for (var off = 0; off < padded.length; off += 64) {
            var i;
            for (i = 0; i < 16; ++i) w[i] = dv.getUint32(off + i * 4);
            for (i = 16; i < 64; ++i) {
                var s0 = ((w[i - 15] >>> 7) | (w[i - 15] << 25)) ^
                         ((w[i - 15] >>> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >>> 3);
                var s1 = ((w[i - 2] >>> 17) | (w[i - 2] << 15)) ^
                         ((w[i - 2] >>> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >>> 10);
                w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
            }
            var a = h[0], b = h[1], c = h[2], d = h[3];
            var e = h[4], f = h[5], g = h[6], hh = h[7];
            for (i = 0; i < 64; ++i) {
                var S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^
                         ((e >>> 25) | (e << 7));
                var ch = (e & f) ^ (~e & g);
                var t1 = (hh + S1 + ch + K[i] + w[i]) | 0;
                var S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^
                         ((a >>> 22) | (a << 10));
                var maj = (a & b) ^ (a & c) ^ (b & c);
                var t2 = (S0 + maj) | 0;
                hh = g; g = f; f = e; e = (d + t1) | 0;
                d = c; c = b; b = a; a = (t1 + t2) | 0;
            }
            h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0;
            h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
            h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0;
            h[6] = (h[6] + g) | 0; h[7] = (h[7] + hh) | 0;
        }
        var hex = "";
        for (var x = 0; x < 8; ++x)
            hex += (h[x] >>> 0).toString(16).padStart(8, "0");
        return hex;
    }

    // ----------------------------------------------------------- the client --

    /**
     * @param base device base URL ("http://192.168.3.129"), or "" when the
     *             page is served by the device itself (same origin).
     */
    function PinledApi(base) {
        this.base = (base || "").replace(/\/+$/, "");
        this.root = makeRoot();
        this.T = {};
        var self = this;
        ["DeviceInfo", "InstallConfig", "MachineProfile", "ApplyResult",
         "LiveFrame", "ProfilerStatus", "AuthorHandle", "DriveClass",
         "ColorOrder"].forEach(function (n) {
            self.T[n] = n === "DriveClass" || n === "ColorOrder"
                ? self.root.lookupEnum("pinled.v1." + n)
                : self.root.lookupType("pinled.v1." + n);
        });
    }

    PinledApi.prototype.url = function (path) {
        return this.base + "/api/v1/" + path;
    };

    /// Every non-2xx becomes an Error whose message carries the device's own
    /// explanation when the body is an ApplyResult — "SHA-256 mismatch" reads
    /// better than "400".
    PinledApi.prototype._check = function (resp) {
        var self = this;
        if (resp.ok) return Promise.resolve(resp);
        return resp.arrayBuffer().then(function (buf) {
            var detail = "";
            try {
                var r = self.T.ApplyResult.decode(new Uint8Array(buf));
                if (r.message) detail = ": " + r.message;
            } catch (e) {
                try { detail = ": " + new TextDecoder().decode(buf).slice(0, 120); }
                catch (e2) { /* the status alone will have to do */ }
            }
            throw new Error("HTTP " + resp.status + detail);
        });
    };

    PinledApi.prototype._get = function (path, type) {
        var self = this;
        return fetch(this.url(path)).then(function (r) {
            return self._check(r);
        }).then(function (r) {
            return r.arrayBuffer();
        }).then(function (buf) {
            return type.decode(new Uint8Array(buf));
        });
    };

    PinledApi.prototype._send = function (path, method, body, contentType) {
        var self = this;
        var opts = { method: method };
        if (body) {
            opts.body = body;
            opts.headers = { "Content-Type": contentType || CONTENT_TYPE };
        }
        return fetch(this.url(path), opts).then(function (r) {
            return self._check(r);
        }).then(function (r) {
            return r.arrayBuffer();
        }).then(function (buf) {
            // A config apply restarts the device (FR-CFG-16), so an empty or
            // torn body here is the expected shape of success, not a failure.
            if (!buf.byteLength)
                return { accepted: true, restarted: true, message: "" };
            return self.T.ApplyResult.toObject(
                self.T.ApplyResult.decode(new Uint8Array(buf)), { defaults: true });
        });
    };

    // Reads. GET returns the stored bytes verbatim (api.h), decoded here.
    PinledApi.prototype.info = function () {
        var T = this.T.DeviceInfo;
        return this._get("info", T).then(function (m) {
            return T.toObject(m, { defaults: true });
        });
    };
    PinledApi.prototype.config = function () {
        var T = this.T.InstallConfig;
        return this._get("config", T).then(function (m) {
            return T.toObject(m, { defaults: true, enums: String });
        });
    };
    PinledApi.prototype.profile = function () {
        var T = this.T.MachineProfile;
        return this._get("profile", T).then(function (m) {
            return T.toObject(m, { defaults: true, enums: String });
        });
    };
    PinledApi.prototype.profiler = function () {
        var T = this.T.ProfilerStatus;
        return this._get("profiler", T).then(function (m) {
            return T.toObject(m, { defaults: true, enums: String });
        });
    };

    // Writes. Objects go in as canonical JSON shapes (camelCase or the
    // .proto's snake_case — both accepted) and leave as encoded protobuf.
    PinledApi.prototype.encode = function (typeName, obj) {
        var T = this.T[typeName];
        var msg = T.fromObject(camelKeys(obj));
        return T.encode(msg).finish();
    };
    PinledApi.prototype.putConfig = function (obj) {
        return this._send("config", "PUT", this.encode("InstallConfig", obj));
    };
    PinledApi.prototype.putProfile = function (obj) {
        return this._send("profile", "PUT", this.encode("MachineProfile", obj));
    };
    PinledApi.prototype.deleteConfig = function () {
        return this._send("config", "DELETE");
    };
    PinledApi.prototype.deleteProfile = function () {
        return this._send("profile", "DELETE");
    };
    PinledApi.prototype.rearmProfiler = function () {
        var self = this;
        return this._send("profiler", "POST").then(function () {
            return self.profiler();
        });
    };
    PinledApi.prototype.setAuthor = function (handle) {
        return this._send("author", "PUT",
                          this.encode("AuthorHandle", { handle: handle }));
    };
    PinledApi.prototype.clearAuthor = function () {
        return this._send("author", "DELETE");
    };

    /// Stage a firmware image (FR-OTA-1/3). Success means STAGED — the
    /// physical button decides whether it ever boots (FR-OTA-2), and there
    /// is deliberately no way to confirm from here.
    PinledApi.prototype.otaUpload = function (bytes) {
        var digest = sha256(bytes);
        var self = this;
        return fetch(this.url("ota"), {
            method: "POST",
            headers: {
                "Content-Type": "application/octet-stream",
                "X-Image-SHA256": digest,
            },
            body: bytes,
        }).then(function (r) {
            return self._check(r);
        }).then(function (r) {
            return r.arrayBuffer();
        }).then(function (buf) {
            return self.T.ApplyResult.toObject(
                self.T.ApplyResult.decode(new Uint8Array(buf)), { defaults: true });
        });
    };
    PinledApi.prototype.otaDiscard = function () {
        return this._send("ota", "DELETE");
    };

    /// The live socket (FR-UI-5). onFrame receives {seq, channelCount,
    /// samples} with samples a Uint8Array, 2 bytes per channel: level, then
    /// flags (bit0 active-since-last-push, bit1 bound, bits 2..4 DriveClass).
    /// Reconnects itself with a small backoff; returns {close()}.
    PinledApi.prototype.openLive = function (onFrame, onState) {
        var T = this.T.LiveFrame;
        var origin = this.base ||
            (global.location ? global.location.origin : "");
        var wsUrl = origin.replace(/^http/, "ws") + "/api/v1/live";
        var closed = false;
        var sock = null;
        var attempt = 0;

        function connect() {
            if (closed) return;
            sock = new global.WebSocket(wsUrl);
            sock.binaryType = "arraybuffer";
            sock.onopen = function () {
                attempt = 0;
                if (onState) onState("live");
            };
            sock.onmessage = function (ev) {
                var m = T.decode(new Uint8Array(ev.data));
                onFrame({
                    seq: m.seq,
                    channelCount: m.channelCount,
                    samples: m.samples,
                });
            };
            sock.onclose = function () {
                if (closed) return;
                if (onState) onState("reconnecting");
                attempt += 1;
                setTimeout(connect, Math.min(1000 * attempt, 5000));
            };
            sock.onerror = function () { /* onclose follows and handles it */ };
        }
        connect();
        return {
            close: function () {
                closed = true;
                if (sock) sock.close();
            },
        };
    };

    // Canonical JSON in and out, for files (FR-CFG-3: human-readable shared
    // profiles). Defaults are omitted on export — that is the canonical form
    // — and either key style is accepted on import.
    PinledApi.prototype.toJson = function (typeName, obj) {
        var T = this.T[typeName];
        return JSON.stringify(
            T.toObject(T.fromObject(camelKeys(obj)), { enums: String }),
            null, 2);
    };
    PinledApi.prototype.fromJson = function (typeName, text) {
        var T = this.T[typeName];
        var msg = T.fromObject(camelKeys(JSON.parse(text)));
        return T.toObject(msg, { defaults: true, enums: String });
    };

    var api = {
        PinledApi: PinledApi,
        sha256: sha256,
        camelKeys: camelKeys,
    };
    global.pinledApi = api;
    if (typeof module !== "undefined" && module.exports)
        module.exports = api;
})(typeof window !== "undefined" ? window : globalThis);
