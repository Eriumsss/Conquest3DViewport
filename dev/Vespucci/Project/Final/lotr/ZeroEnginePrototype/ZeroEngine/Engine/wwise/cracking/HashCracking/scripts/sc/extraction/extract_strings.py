#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Binary dosyalardan (örn. Wwise .pck) ASCII ve UTF-16LE string çıkarıcı
# Kullanım örnekleri aşağıda.
#
import argparse, re, os, sys

def extract_ascii(data: bytes, min_len: int, with_offset: bool):
    pat = re.compile(rb"[ -~]{%d,}" % min_len)
    if with_offset:
        for m in pat.finditer(data):
            yield (m.start(), m.group().decode("utf-8", errors="replace"))
    else:
        for s in pat.findall(data):
            yield (None, s.decode("utf-8", errors="replace"))

def extract_utf16le(data: bytes, min_len: int, with_offset: bool):
    # örn. (A\x00){4,}
    pat = re.compile(rb"(?:[ -~]\x00){%d,}" % min_len)
    if with_offset:
        for m in pat.finditer(data):
            s = m.group().replace(b"\x00", b"").decode("utf-16le", errors="replace")
            yield (m.start(), s)
    else:
        for m in pat.findall(data):
            s = m.replace(b"\x00", b"").decode("utf-16le", errors="replace")
            yield (None, s)

def write_lines(path, lines, encoding="utf-8"):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding=encoding, newline="\n") as f:
        for line in lines:
            f.write(line + "\n")

def main():
    ap = argparse.ArgumentParser(
        description="Extract ASCII & UTF-16LE strings from a binary file (e.g., Wwise .pck)."
    )
    ap.add_argument("input", help="input file (e.g., sound.pck)")
    ap.add_argument("-o", "--outdir", default="strings_out", help="output directory (default: strings_out)")
    ap.add_argument("-m", "--min", type=int, default=4, help="minimum string length (default: 4)")
    ap.add_argument("--no-ascii", action="store_true", help="disable ASCII extraction")
    ap.add_argument("--no-utf16", action="store_true", help="disable UTF-16LE extraction")
    ap.add_argument("--dedup", action="store_true", help="deduplicate strings (case-sensitive)")
    ap.add_argument("--with-offset", action="store_true", help="prefix each line with 0xOFFSET")
    ap.add_argument("--print", dest="to_print", type=int, default=0, help="print first N results to console")
    args = ap.parse_args()

    try:
        data = open(args.input, "rb").read()
    except Exception as e:
        print(f"[!] read error: {e}")
        sys.exit(1)

    base = os.path.splitext(os.path.basename(args.input))[0]
    ascii_lines, utf16_lines = [], []

    if not args.no_ascii:
        items = list(extract_ascii(data, args.min, args.with_offset))
        if args.dedup:
            seen = set()
            items = [(off, s) for (off, s) in items if not (s in seen or seen.add(s))]
        ascii_lines = [ (f"0x{off:08X}: {s}" if off is not None else s) for off, s in items ]
        out_ascii = os.path.join(args.outdir, f"{base}_strings_ascii.txt")
        write_lines(out_ascii, ascii_lines)

    if not args.no_utf16:
        items = list(extract_utf16le(data, args.min, args.with_offset))
        if args.dedup:
            seen = set()
            items = [(off, s) for (off, s) in items if not (s in seen or seen.add(s))]
        utf16_lines = [ (f"0x{off:08X}: {s}" if off is not None else s) for off, s in items ]
        out_utf16 = os.path.join(args.outdir, f"{base}_strings_utf16.txt")
        write_lines(out_utf16, utf16_lines)

    total = len(ascii_lines) + len(utf16_lines)
    print(f"[+] done. saved {total} strings to '{args.outdir}'")
    if ascii_lines:
        print(f"    - ASCII:  {len(ascii_lines)} lines -> {base}_strings_ascii.txt")
    if utf16_lines:
        print(f"    - UTF16:  {len(utf16_lines)} lines -> {base}_strings_utf16.txt")

    if args.to_print > 0:
        print("\n--- sample ---")
        shown = 0
        for L in (ascii_lines + utf16_lines):
            print(L)
            shown += 1
            if shown >= args.to_print: break

if __name__ == "__main__":
    main()
