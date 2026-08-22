#!/usr/bin/env bash
# Publish web/ as the cloud bundle (WEBUI.md §1).
#
# The device serves only the shell; this bundle is what the shell loads,
# from the visitor's browser — the device itself never fetches it
# (FR-UI-2). Publishing here updates every provisioned device's UI on its
# next page load, with no firmware involved: that is the entire point of
# the split.
#
# Cache strategy, stated: index.html is no-cache so a publish takes effect
# immediately; everything else gets an hour, because assets are not
# content-hashed and an hour bounds how long a stale css/js can pair with
# a fresh index. If the bundle ever grows a build step, hash the assets
# and make them immutable instead.
#
# Bucket config is code: tools/cloud/*.json, applied at creation
# 2026-08-22. CORS GET/HEAD from any origin is REQUIRED — the shell
# fetch()es index.html cross-origin from a http://<device> page.
set -euo pipefail

BUCKET="s3://oneoffendeavors-pinled-webui"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# The schema text ships inside the bundle; regenerate so it can never lag
# the .proto.
python3 "$ROOT/tools/gen_proto_js.py"
python3 "$ROOT/tools/gen_openapi_js.py"

# Assets first, index last: a visitor mid-publish gets old-index+old-assets
# or new-index+new-assets, never new-index over missing assets.
aws s3 sync "$ROOT/web/" "$BUCKET/" \
    --exclude "index.html" \
    --cache-control "max-age=3600" \
    --delete

aws s3 cp "$ROOT/web/index.html" "$BUCKET/index.html" \
    --cache-control "no-cache"

echo "published: https://oneoffendeavors-pinled-webui.s3.us-east-1.amazonaws.com/index.html"
