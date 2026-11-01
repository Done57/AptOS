#!/usr/bin/env python3
"""mkcjkfont.py - build kernel/fontcjk.h (16x16 CJK bitmap font) from GNU Unifont.

GNU Unifont (https://unifoundry.com/unifont/) ships a plain-text bitmap format
where every line is  CODE:HEXDATA  (64 hex digits == 16x16 pixels for wide
characters).  That is exactly what a 32-bit protected mode kernel wants: no
rasterizer, no font engine, just bits.

The character set is derived from the source tree: every non-ASCII codepoint
that appears in a C string literal of the scanned sources is included, plus the
punctuation set below and any --extra characters.  Only the glyphs the system
actually draws get embedded, which keeps the table at a few tens of KiB instead
of the 8 MiB of the full Unifont.

Usage:
    tools/mkcjkfont.py --hex /path/to/unifont_all-18.0.01.hex \
                       --src kernel apps --out kernel/fontcjk.h

Download the font first with tools/getunifont.sh.  The generated header is
committed, so a normal `make` needs neither the .hex file nor network access.

GNU Unifont is distributed under the GNU GPL v2 or later with the GNU font
embedding exception; the generated table is a subset of that font and keeps the
same terms (see the header of kernel/fontcjk.h).
"""

import argparse
import gzip
import os
import re
import sys

# Punctuation, arrows and symbols that the UI may use even when no literal
# contains them yet.  Unifont draws all of these 16x16.
PUNCT = (
    "\u3000\u3001\u3002\u300a\u300b\u300c\u300d\u300e\u300f\u3010\u3011"
    "\uff01\uff02\uff03\uff04\uff05\uff06\uff07\uff08\uff09\uff0a\uff0b\uff0c"
    "\uff0d\uff0e\uff0f\uff1a\uff1b\uff1c\uff1d\uff1e\uff1f\uff20\uff3b\uff3c"
    "\uff3d\uff5b\uff5c\uff5d\uff5e\u2014\u2018\u2019\u201c\u201d\u2026\u00b7"
    "\u2190\u2191\u2192\u2193\u25b6\u25c0\u2605\u2606\u2713\u2717"
)

# Characters that are handy to have even before any string uses them.
BASE = "中文英文语言切换设置关于帮助窗口桌面终端文件夹系统安装完成失败成功"


def strip_comments(text):
    """Drop // and /* */ comments so only real string literals are scanned."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def collect_sources(paths):
    """Return (codepoints used in string literals, list of scanned files)."""
    chars = set()
    files = []
    for path in paths:
        if os.path.isdir(path):
            for root, _dirs, names in os.walk(path):
                for n in sorted(names):
                    if n.endswith((".c", ".h")):
                        files.append(os.path.join(root, n))
        else:
            files.append(path)

    for path in files:
        base = os.path.basename(path)
        if base in ("fontcjk.h", "font8x8.h"):
            continue
        try:
            text = open(path, encoding="utf-8").read()
        except (OSError, UnicodeDecodeError):
            continue
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', strip_comments(text)):
            for ch in m.group(1):
                if ord(ch) >= 0x80:
                    chars.add(ord(ch))
    return chars, files


def open_hex(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="ascii", errors="replace")
    return open(path, "r", encoding="ascii", errors="replace")


def load_unifont(path, wanted):
    """Return {codepoint: [16 rows of 16 bits]} for the wanted codepoints."""
    glyphs = {}
    with open_hex(path) as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            code_s, data = line.split(":", 1)
            try:
                code = int(code_s, 16)
            except ValueError:
                continue
            if code not in wanted or code in glyphs:
                continue
            data = data.strip()
            if len(data) == 64:                 # 16x16 bitmap, used as is
                rows = [int(data[i * 4:i * 4 + 4], 16) for i in range(16)]
            elif len(data) == 32:               # 8x16: centred in a 16x16 cell
                rows = [int(data[i * 2:i * 2 + 2], 16) << 4 for i in range(16)]
            else:
                continue                        # 32x16 (plane 1+): not used
            glyphs[code] = rows
            if len(glyphs) == len(wanted):
                break
    return glyphs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hex", required=True, help="unifont .hex or .hex.gz")
    ap.add_argument("--src", action="append", default=[], help="source tree to scan")
    ap.add_argument("--extra", default="", help="extra characters to embed")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    if not os.path.exists(args.hex):
        sys.exit("mkcjkfont: %s: no such file (run tools/getunifont.sh)" % args.hex)

    chars, files = collect_sources(args.src)
    for ch in PUNCT + BASE + args.extra:
        chars.add(ord(ch))
    wanted = set(c for c in chars if c >= 0x80)

    glyphs = load_unifont(args.hex, wanted)
    missing = sorted(c for c in wanted if c not in glyphs)

    codes = sorted(glyphs)
    out = []
    out.append("/* Auto-generated by tools/mkcjkfont.py - do not edit by hand.")
    out.append(" *")
    out.append(" * Font   : GNU Unifont 16x16 bitmap subset, %d wide glyphs" % len(codes))
    out.append(" * Source : %s" % os.path.basename(args.hex))
    out.append(" * Scanned: %s%s" % (", ".join(os.path.basename(f) for f in files[:6]),
                                     " ..." if len(files) > 6 else ""))
    out.append(" *")
    out.append(" * GNU Unifont is (C) Roman Czyborra, Paul Hardy and contributors,")
    out.append(" * licensed under the GNU GPL v2 or later with the GNU font embedding")
    out.append(" * exception; see https://unifoundry.com/unifont/ .  This table is an")
    out.append(" * unmodified bitmap subset of that font and carries the same terms.")
    out.append(" *")
    out.append(" * Regenerate: tools/getunifont.sh && make font")
    out.append(" */")
    out.append("#ifndef APTOS_FONTCJK_H")
    out.append("#define APTOS_FONTCJK_H")
    out.append("")
    out.append("#define FONT_CJK_W 16")
    out.append("#define FONT_CJK_H 16")
    out.append("#define FONTCJK_COUNT %d" % len(codes))
    out.append("")
    out.append("/* One row per line, bit 0x8000 is the leftmost pixel. */")
    out.append("static const unsigned short fontcjk_rows[FONTCJK_COUNT][FONT_CJK_H] = {")
    for code in codes:
        out.append("    { %s }, /* U+%04X %s */" %
                   (", ".join("0x%04X" % r for r in glyphs[code]), code, chr(code)))
    out.append("};")
    out.append("")
    out.append("static const unsigned int fontcjk_code[FONTCJK_COUNT] = {")
    for i in range(0, len(codes), 12):
        out.append("    " + ", ".join("0x%04X" % c for c in codes[i:i + 12]) + ",")
    out.append("};")
    out.append("")
    out.append("/* Binary search over the sorted code table; 0 when the glyph is absent. */")
    out.append("static const unsigned short *fontcjk_lookup(unsigned int cp)")
    out.append("{")
    out.append("    int lo = 0, hi = FONTCJK_COUNT - 1;")
    out.append("    while (lo <= hi) {")
    out.append("        int mid = (lo + hi) / 2;")
    out.append("        if (fontcjk_code[mid] == cp)")
    out.append("            return fontcjk_rows[mid];")
    out.append("        if (fontcjk_code[mid] < cp)")
    out.append("            lo = mid + 1;")
    out.append("        else")
    out.append("            hi = mid - 1;")
    out.append("    }")
    out.append("    return 0;")
    out.append("}")
    out.append("")
    out.append("#endif")
    out.append("")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    print("mkcjkfont: %d/%d glyphs -> %s (%.1f KiB), scanned %d sources" %
          (len(codes), len(wanted), args.out, os.path.getsize(args.out) / 1024.0,
           len(files)))
    if missing:
        shown = " ".join("%s(U+%04X)" % (chr(c), c) for c in missing[:40])
        print("mkcjkfont: WARNING: Unifont has no 16x16 glyph for: %s" % shown)
        print("mkcjkfont: %d codepoints missing in total" % len(missing))


if __name__ == "__main__":
    main()
