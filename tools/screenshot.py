#!/usr/bin/env python3
"""
Pull a screenshot off the M5StickC Plus over the serial port.

The LCD cannot be read back, but every page is composed into a TFT_eSprite
first - a real 240x135x16bpp framebuffer in RAM - and the firmware can stream
that out. This decodes it and writes a PNG using only the standard library.

  python screenshot.py                 capture whatever page is showing
  python screenshot.py --page 4        switch to page 4 first
  python screenshot.py --all           one PNG per page
  python screenshot.py --scale 1       native 240x135 instead of 3x
"""
import argparse
import base64
import struct
import sys
import time
import zlib

import serial


def write_png(path, w, h, rows_rgb):
    """rows_rgb: list of bytes objects, each w*3 bytes of RGB888."""
    raw = b"".join(b"\x00" + r for r in rows_rgb)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def rgb565_row_to_rgb888(data, width):
    out = bytearray(width * 3)
    for x in range(width):
        c = (data[x * 2] << 8) | data[x * 2 + 1]
        r = (c >> 11) & 0x1F
        g = (c >> 5) & 0x3F
        b = c & 0x1F
        # expand by replicating the high bits, so 0x1F maps to 255 exactly
        out[x * 3]     = (r << 3) | (r >> 2)
        out[x * 3 + 1] = (g << 2) | (g >> 4)
        out[x * 3 + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def capture(ser, out_path, scale):
    ser.reset_input_buffer()
    ser.write(b"C")
    ser.flush()

    width = height = None
    rows = {}
    bad = []
    deadline = time.time() + 40

    while time.time() < deadline:
        line = ser.readline().decode("ascii", "replace").strip()
        if not line:
            continue
        if line.startswith("SSBEGIN"):
            parts = line.split()
            width, height = int(parts[1]), int(parts[2])
            continue
        if line.startswith("SSEND"):
            break
        if line.startswith("R ") and width:
            parts = line.split()
            if len(parts) != 4:
                bad.append(line[:20])
                continue
            y = int(parts[1])
            try:
                data = base64.b64decode(parts[2])
            except Exception:
                bad.append("b64 row %d" % y)
                continue
            if len(data) != width * 2:
                bad.append("len row %d" % y)
                continue
            if (sum(data) & 0xFFFF) != int(parts[3], 16):
                bad.append("chk row %d" % y)
                continue
            rows[y] = data

    if width is None:
        print("no SSBEGIN seen - is the screenshot firmware running?")
        return False

    missing = [y for y in range(height) if y not in rows]
    if missing:
        print("  %d row(s) lost/corrupt: %s%s" % (
            len(missing), missing[:12], " ..." if len(missing) > 12 else ""))
    if bad:
        print("  parse issues: %s" % bad[:6])

    rgb = []
    for y in range(height):
        data = rows.get(y, b"\x00" * (width * 2))   # drop-outs stay black
        row = rgb565_row_to_rgb888(data, width)
        if scale > 1:
            wide = bytearray()
            for x in range(width):
                wide += row[x * 3:x * 3 + 3] * scale
            row = bytes(wide)
        rgb.extend([row] * scale)

    write_png(out_path, width * scale, height * scale, rgb)
    print("  wrote %s  (%dx%d, %d/%d rows good)" % (
        out_path, width * scale, height * scale, len(rows), height))
    return len(missing) == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--page", type=int, help="jump to this page first (1-5)")
    ap.add_argument("--all", action="store_true", help="capture every page")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--out", default="screen")
    ap.add_argument("--retries", type=int, default=2)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1.0)
    # opening the port toggles DTR/RTS and resets the ESP32 - wait it out
    time.sleep(3.0)
    ser.reset_input_buffer()

    pages = range(1, 6) if args.all else [args.page] if args.page else [None]

    for p in pages:
        if p is not None:
            ser.write(str(p).encode())
            ser.flush()
            time.sleep(1.5 if p != 4 else 3.0)   # page 4 needs FFT data first
        name = "%s%s.png" % (args.out, "_p%d" % p if p else "")
        print("capturing %s" % name)
        for attempt in range(args.retries + 1):
            if capture(ser, name, args.scale):
                break
            if attempt < args.retries:
                print("  retrying...")
                time.sleep(0.5)

    ser.close()


if __name__ == "__main__":
    main()
