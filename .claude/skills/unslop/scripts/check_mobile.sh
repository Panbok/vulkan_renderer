#!/usr/bin/env bash
# Render an HTML report in a 360 x 780 CSS px viewport (Galaxy S23 portrait),
# report horizontal page overflow, and save a first-screen screenshot.
#
# Usage: check_mobile.sh <page.html> [screenshot.png]
# Exit:  0 no overflow, 1 overflow, 2 usage or tool error.
#
# Headless Chrome will not lay out a window narrower than 500 px, so the page
# loads in a 360 px iframe. Override the browser with CHROME=<path>.

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ! -f "$1" ]]; then
    echo "usage: check_mobile.sh <page.html> [screenshot.png]" >&2
    exit 2
fi

chrome="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
if [[ ! -x "$chrome" ]]; then
    echo "error: Chrome not found at '$chrome'; set CHROME" >&2
    exit 2
fi

page_url="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve().as_uri())' "$1")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/probe.html" <<EOF
<!doctype html>
<body style="margin:0">
<iframe id="f" src="$page_url" style="width:360px;height:780px;border:0"></iframe>
<script>
f.onload = () => {
    const doc = f.contentDocument;
    const width = f.contentWindow.innerWidth;
    const scrolls = (el) => {
        for (let p = el.parentElement; p; p = p.parentElement) {
            const o = getComputedStyle(p).overflowX;
            if (o === "auto" || o === "scroll" || o === "hidden") {
                return true;
            }
        }
        return false;
    };
    const wide = [...doc.body.querySelectorAll("*")]
        .filter((el) => el.getBoundingClientRect().right > width + 1 && !scrolls(el))
        .slice(0, 8)
        .map((el) => el.tagName.toLowerCase() + (el.className && typeof el.className === "string" ? "." + el.className.trim().split(/\s+/).join(".") : ""));
    document.body.dataset.result = doc.documentElement.scrollWidth + " " + width + " " + wide.join(",");
};
</script>
EOF

result="$("$chrome" --headless --disable-gpu --allow-file-access-from-files \
    --virtual-time-budget=5000 --dump-dom "file://$work/probe.html" 2>/dev/null |
    sed -n 's/.*data-result="\([^"]*\)".*/\1/p')"

if [[ -z "$result" ]]; then
    echo "error: the probe produced no result; the page may not have loaded" >&2
    exit 2
fi

read -r scroll_width viewport_width wide <<< "$result"

if [[ $# -eq 2 ]]; then
    "$chrome" --headless --disable-gpu --allow-file-access-from-files --hide-scrollbars \
        --window-size=500,780 --virtual-time-budget=5000 --screenshot="$2" "file://$work/probe.html" >/dev/null 2>&1
    echo "screenshot: $2 (the left 360 px show the phone viewport)"
fi

if (( scroll_width > viewport_width )); then
    echo "FAIL: page scrolls horizontally: scroll width ${scroll_width} px > viewport ${viewport_width} px"
    echo "elements past the right edge outside a scroll container: ${wide:-none found}"
    exit 1
fi

echo "PASS: no horizontal page scroll at ${viewport_width} px"
