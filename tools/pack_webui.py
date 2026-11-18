#!/usr/bin/env python3
"""
pack_webui.py — Compress tools/webui/index.html into webui_gz.h (gzip -9, deterministic mtime=0)
Usage:
    python tools/pack_webui.py
"""
import gzip
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_HTML = ROOT / "tools" / "webui" / "index.html"
OUT_HDR = ROOT / "webui_gz.h"


def main() -> None:
    raw_text = SRC_HTML.read_text(encoding="utf-8")
    # Normalize CRLF -> LF for deterministic cross-platform builds
    raw_bytes = raw_text.replace("\r\n", "\n").rstrip("\n").encode("utf-8")
    gz_bytes = gzip.compress(raw_bytes, compresslevel=9, mtime=0)

    # Strict round-trip verification
    assert gzip.decompress(gz_bytes) == raw_bytes, "Gzip round-trip mismatch!"

    ratio = (1.0 - len(gz_bytes) / len(raw_bytes)) * 100.0
    lines = [
        "/*",
        " * webui_gz.h — Auto-generated gzipped WebUI asset (DO NOT EDIT DIRECTLY)",
        " * Edit tools/webui/index.html and regenerate via: python tools/pack_webui.py",
        f" * Raw UTF-8 size : {len(raw_bytes)} bytes",
        f" * Gzip (-9) size : {len(gz_bytes)} bytes ({ratio:.1f}% reduction)",
        " */",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        f"constexpr size_t INDEX_HTML_GZ_LEN = {len(gz_bytes)}U;",
        "",
        "alignas(4) static const uint8_t INDEX_HTML_GZ[] = {",
    ]

    for i in range(0, len(gz_bytes), 16):
        chunk = gz_bytes[i : i + 16]
        hex_row = ", ".join(f"0x{b:02X}" for b in chunk)
        suffix = "," if (i + 16 < len(gz_bytes)) else ""
        lines.append(f"  {hex_row}{suffix}")

    lines.append("};")
    lines.append("")

    OUT_HDR.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(
        f"Generated {OUT_HDR.name}: {len(raw_bytes)}B -> {len(gz_bytes)}B "
        f"(-{len(raw_bytes) - len(gz_bytes)}B, {ratio:.1f}% smaller)"
    )


if __name__ == "__main__":
    main()
