# The pinled configuration app

The SPA specified in `docs/WEBUI.md`, against the real device API. No build
step, no framework, no network dependency — the design reference it
implements is `docs/ui-mockup.html`, and the deliberate constraints are the
mockup's too: classic scripts because `file://` blocks ES modules, a pure-JS
SHA-256 because an `http://` origin has no `crypto.subtle`, and protobuf.js
parsing `proto/pinled.proto` at runtime so there is exactly one schema
authority (FR-CFG-13).

## Running it

**Standalone (FR-UI-8, the offline path):** open `index.html` from disk. It
asks for the device address once (`http://pinled.local`, or the IP the setup
page reported) and remembers it.

**From the device (FR-UI-1):** a provisioned board serves a shell at
`http://<device>/` that loads this app from the bundle URL baked into the
firmware (`PINLED_BUNDLE_URL`). With no bundle configured the shell explains
the standalone path instead. Serving these files from any plain HTTP host on
the LAN also works — the app talks to whatever device it is pointed at.

**The device never fetches any of this** (FR-UI-2). Only the browser talks
to both sides.

## Layout

| Path | What |
|---|---|
| `index.html` | markup for every panel; the only HTML |
| `css/app.css` | extracted from `ui-mockup.html` (the approved design), app-only additions at the end |
| `js/api.js` | the device API client: protobuf codec, fetch, live socket, SHA-256 |
| `js/app.js` | panels, the learn-wiring flow, apply/import/export |
| `js/gen/proto.js` | **generated** — `tools/gen_proto_js.py` wraps the `.proto` text; regenerate after any schema change |
| `js/vendor/protobuf.min.js` | protobuf.js 7.4.0, vendored (BSD-3-Clause, see `protobuf.LICENSE`) |

## Testing

`test/web/api_check.js` runs `js/api.js` under node against a real board —
the wire-level half that does not need eyes:

```sh
node test/web/api_check.js http://192.168.3.129
```

The visual half is the acceptance test from `FIRMWARE_PLAN.md` §5.2 step 4:
turn on Learn wiring, fire a lamp from the test card, click the channel that
lit, watch the live view confirm the binding.
