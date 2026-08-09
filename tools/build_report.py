"""
build_report.py -- inline the hero images into a report template.

A published artifact runs under a strict CSP that blocks every external host, so
an <img src="..."> pointing at a file or a CDN silently renders nothing. Images
have to be data URIs, and a 200KB base64 blob is not something to paste into a
template by hand. So the template keeps readable {{TOKEN}} placeholders and this
substitutes them at build time.

    python tools/build_report.py docs/report_deep_template.html docs/REPORT-DEEP.html
"""

import base64
import os
import sys

IMAGES = {
    "{{HERO}}": "build/bin/hero.jpg",
    "{{HERO2}}": "build/bin/hero2.jpg",
}


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: build_report.py <template> <output>")
    src, dst = sys.argv[1], sys.argv[2]

    html = open(src, encoding="utf-8").read()
    for token, path in IMAGES.items():
        if token not in html:
            continue
        if not os.path.exists(path):
            raise SystemExit("missing image for %s: %s" % (token, path))
        with open(path, "rb") as f:
            b64 = base64.b64encode(f.read()).decode("ascii")
        html = html.replace(token, "data:image/jpeg;base64," + b64)

    # Fail loudly on an unresolved placeholder. A stray {{TOKEN}} renders as a
    # broken image in the published page and is easy to miss on a long document.
    if "{{" in html:
        start = html.index("{{")
        raise SystemExit("unresolved placeholder near: %s" % html[start:start + 40])

    with open(dst, "w", encoding="utf-8") as f:
        f.write(html)

    print("%s  %d KB" % (dst, os.path.getsize(dst) // 1024))
    print("  mermaid diagrams : %d" % html.count('class="mermaid"'))
    print("  failure blocks   : %d" % html.count('class="fail"'))
    print("  loop blocks      : %d" % html.count('class="loop"'))
    print("  sections         : %d" % html.count("<section"))
    print("  tables           : %d" % html.count("<table>"))


if __name__ == "__main__":
    main()
