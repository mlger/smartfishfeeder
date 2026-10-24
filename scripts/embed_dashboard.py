#!/usr/bin/env python3
"""PlatformIO pre-build hook: embed the web dashboard into a C header.

Reads dashboard/index.html, gzip-compresses it (level 9) and writes
src/dashboard_html.h containing a PROGMEM byte array. Regenerates only when
the source is newer than the generated header (or the header is missing).

Edit the dashboard in dashboard/index.html; this script runs automatically
on the next `pio run`. The file is served with Content-Encoding: gzip.

Runs under SCons (Import("env"), no __file__) AND standalone (python3 ...).
"""

import gzip
import os

try:
    Import("env")  # PlatformIO SCons environment
    PROJECT_DIR = env["PROJECT_DIR"]  # .../firmware/display-unit
    STANDALONE = False
except (NameError, ImportError):
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    STANDALONE = True

SRC = os.path.normpath(os.path.join(PROJECT_DIR, "..", "..", "dashboard", "index.html"))
DST = os.path.normpath(os.path.join(PROJECT_DIR, "src", "dashboard_html.h"))


def build() -> None:
    with open(SRC, "rb") as f:
        raw = f.read()
    gz = gzip.compress(raw, 9)
    out = []
    out.append("#pragma once")
    out.append("// AUTO-GENERATED from dashboard/index.html by scripts/embed_dashboard.py")
    out.append("// gzip-compressed single-page dashboard. DO NOT EDIT BY HAND.")
    out.append("")
    out.append("static const unsigned int DASHBOARD_HTML_GZ_LEN = %d;" % len(gz))
    out.append("static const uint8_t DASHBOARD_HTML_GZ[] PROGMEM = {")
    for i in range(0, len(gz), 20):
        chunk = ", ".join("0x%02X" % b for b in gz[i : i + 20])
        out.append("    " + chunk + ",")
    out.append("};")
    out.append("")
    tmp = DST + ".tmp"
    with open(tmp, "w") as f:
        f.write("\n".join(out))
    os.replace(tmp, DST)
    print("[embed_dashboard] embedded %d -> %d bytes gz" % (len(raw), len(gz)))


def _should_build() -> bool:
    if not os.path.exists(SRC):
        return False
    return not os.path.exists(DST) or os.path.getmtime(SRC) > os.path.getmtime(DST)


if STANDALONE:
    if _should_build():
        build()
else:
    if _should_build():
        build()
