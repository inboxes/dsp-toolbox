#!/usr/bin/env python3
"""Generates the app icon and tray glyph (SVG, and optionally PNG/ICO).

The icon is an isometric "module cube" filling a hexagon. Its three faces show
the three module kinds: orbit (top), equalizer bars (left), spectrogram (right).

    python tools/gen_icons.py              # writes SVGs into assets/
    python tools/gen_icons.py --render     # also PNGs + .ico (needs rsvg-convert on PATH)

Only the standard library is used. Edit the constants below, re-run, commit the output.
"""
import argparse
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path

CYAN, VIOLET = "#22d3ee", "#a78bfa"
SEAM_ON_DARK = "#0b0e22"   # seams match a dark surface
SEAM_ON_LIGHT = "#f8fafc"  # seams match a light surface
R = 114                    # hexagon circumradius in the 256 viewBox (colour icon)
R_MONO = 118               # tray glyph fills slightly more of the canvas


# --- geometry helpers ---------------------------------------------------------
def pt(r, deg):
    a = math.radians(deg)
    return 128 + r * math.cos(a), 128 + r * math.sin(a)


def hexverts(r):
    """Pointy-top hexagon: top, upper-right, lower-right, bottom, lower-left, upper-left."""
    return [pt(r, -90 + 60 * k) for k in range(6)]


def pts(ps):
    return " ".join("%.1f,%.1f" % p for p in ps)


def face(o, eu, ev):
    """Map a unit square (u, v) onto a cube face spanned from origin o towards eu and ev."""
    return lambda u, v: (o[0] + u * (eu[0] - o[0]) + v * (ev[0] - o[0]),
                         o[1] + u * (eu[1] - o[1]) + v * (ev[1] - o[1]))


def quad(f, u0, v0, u1, v1):
    return pts([f(u0, v0), f(u1, v0), f(u1, v1), f(u0, v1)])


def inferno(v):
    st = [(0, (12, 8, 38)), (.3, (110, 20, 110)), (.6, (220, 70, 70)), (.85, (251, 160, 20)), (1, (252, 255, 164))]
    for (a, ca), (b, cb) in zip(st, st[1:]):
        if v <= b:
            k = (v - a) / (b - a)
            return "#%02x%02x%02x" % tuple(round(ca[i] + (cb[i] - ca[i]) * k) for i in range(3))
    return "#fcffa4"


def wrap(body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">{body}</svg>\n'


# --- colour icon ----------------------------------------------------------------
def color_icon(seam):
    t, ur, lr, b, ll, ul = hexverts(R)
    c = (128, 128)
    hexp = pts([t, ur, lr, b, ll, ul])
    o = [f'<defs><mask id="hm"><polygon points="{hexp}" fill="#fff" stroke="#fff" stroke-width="8" stroke-linejoin="round"/></mask></defs>',
         # lighter rim keeps the dark silhouette visible on dark surfaces
         f'<polygon points="{hexp}" fill="#3b4390" stroke="#3b4390" stroke-width="16" stroke-linejoin="round"/>',
         '<g mask="url(#hm)">',
         f'<polygon points="{pts([t, ur, c, ul])}" fill="#2b3274"/>',
         f'<polygon points="{pts([ul, c, b, ll])}" fill="#1e2459"/>',
         f'<polygon points="{pts([c, ur, lr, b])}" fill="#161a43"/>']
    for e in (ur, ul, b):
        o.append(f'<line x1="128" y1="128" x2="{e[0]:.1f}" y2="{e[1]:.1f}" stroke="{seam}" stroke-width="6"/>')
    # top face: orbit ring, listener, source
    top = face(t, ur, ul)
    ring = [top(.5 + .3 * math.cos(i * math.pi / 18), .5 + .3 * math.sin(i * math.pi / 18)) for i in range(37)]
    o.append(f'<polyline points="{pts(ring)}" fill="none" stroke="{VIOLET}" stroke-width="9" stroke-linejoin="round"/>')
    cx, cy = top(.5, .5)
    o.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="8" fill="{CYAN}"/>')
    dx, dy = top(.5 + .3 * math.cos(-.9), .5 + .3 * math.sin(-.9))
    o.append(f'<circle cx="{dx:.1f}" cy="{dy:.1f}" r="12" fill="#fff" stroke="#2b3274" stroke-width="4"/>')
    # left face: equalizer bars
    lf = face(ul, c, ll)
    for i, h in enumerate((.28, .55, .40, .68)):
        u0 = .12 + i * .22
        o.append(f'<polygon points="{quad(lf, u0, .88, u0 + .14, .88 - h)}" fill="{CYAN}" stroke="{CYAN}" stroke-width="3" stroke-linejoin="round"/>')
    # right face: spectrogram cells
    rf = face(c, ur, b)
    heat = [[.2, .55, .9], [.5, .95, .45], [.85, .4, .15]]
    for ri in range(3):
        for ci in range(3):
            u0, v0 = .10 + ci * .29, .10 + ri * .29
            col = inferno(heat[ri][ci])
            o.append(f'<polygon points="{quad(rf, u0, v0, u0 + .23, v0 + .23)}" fill="{col}" stroke="{col}" stroke-width="2" stroke-linejoin="round"/>')
    o.append("</g>")
    return wrap("".join(o))


# --- monochrome tray glyph ---------------------------------------------------------
def mono_icon(fill):
    """Solid hexagon with cube seams, an orbit ring and EQ bars knocked out (transparent); right face stays solid."""
    t, ur, lr, b, ll, ul = hexverts(R_MONO)
    c = (128, 128)
    hexp = pts([t, ur, lr, b, ll, ul])
    k = ['<rect width="256" height="256" fill="#000"/>',
         f'<polygon points="{hexp}" fill="#fff" stroke="#fff" stroke-width="12" stroke-linejoin="round"/>']
    for e in (ur, ul, b):
        k.append(f'<line x1="128" y1="128" x2="{e[0]:.1f}" y2="{e[1]:.1f}" stroke="#000" stroke-width="15"/>')
    top = face(t, ur, ul)
    # orbit: a flattened ring knocked out of the top face (reads as an ellipse, not a dice pip)
    outer = [top(.5 + .30 * math.cos(i * math.pi / 8), .5 + .30 * math.sin(i * math.pi / 8)) for i in range(16)]
    inner = [top(.5 + .13 * math.cos(i * math.pi / 8), .5 + .13 * math.sin(i * math.pi / 8)) for i in range(16)]
    k.append(f'<polygon points="{pts(outer)}" fill="#000"/>')
    k.append(f'<polygon points="{pts(inner)}" fill="#fff"/>')
    lf = face(ul, c, ll)
    for u0, h in ((.14, .34), (.42, .62), (.70, .46)):
        k.append(f'<polygon points="{quad(lf, u0, .84, u0 + .18, .84 - h)}" fill="#000"/>')
    return wrap(f'<defs><mask id="k">{"".join(k)}</mask></defs>'
                f'<rect width="256" height="256" fill="{fill}" mask="url(#k)"/>')


# --- rendering ----------------------------------------------------------------------
def render_png(svg, size, out):
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), str(svg), "-o", str(out)], check=True)


def write_ico(path, entries):
    """entries: {size: png_bytes}. PNG-compressed ICO, readable by Windows Vista and later."""
    sizes = sorted(entries)
    blob = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    body = b""
    for s in sizes:
        data = entries[s]
        blob += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    Path(path).write_bytes(blob + body)


def build_ico(svg, sizes, out, tmp):
    entries = {}
    for s in sizes:
        p = tmp / f"{Path(out).stem}-{s}.png"
        render_png(svg, s, p)
        entries[s] = p.read_bytes()
    write_ico(out, entries)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=str(Path(__file__).resolve().parent.parent / "assets"))
    ap.add_argument("--render", action="store_true", help="also write PNGs and .ico files (needs rsvg-convert)")
    a = ap.parse_args()
    assets = Path(a.assets)
    (assets / "icon").mkdir(parents=True, exist_ok=True)
    (assets / "tray").mkdir(parents=True, exist_ok=True)

    files = {
        assets / "icon" / "icon-on-dark.svg": color_icon(SEAM_ON_DARK),
        assets / "icon" / "icon-on-light.svg": color_icon(SEAM_ON_LIGHT),
        assets / "tray" / "tray-white.svg": mono_icon("#ffffff"),
        assets / "tray" / "tray-black.svg": mono_icon("#000000"),
    }
    for p, s in files.items():
        p.write_text(s, encoding="utf-8")
    print("wrote", len(files), "SVGs to", assets)

    if not a.render:
        return
    if not shutil.which("rsvg-convert"):
        sys.exit("--render needs rsvg-convert on PATH")
    tmp = assets / ".render-tmp"
    tmp.mkdir(exist_ok=True)
    try:
        app_sizes = (16, 24, 32, 48, 64, 128, 256)
        tray_sizes = (16, 20, 24, 32, 48)  # 100%..300% display scaling
        for name in ("icon-on-dark", "icon-on-light"):
            render_png(assets / "icon" / f"{name}.svg", 256, assets / "icon" / f"{name}.png")
        build_ico(assets / "icon" / "icon-on-dark.svg", app_sizes, assets / "app.ico", tmp)
        build_ico(assets / "icon" / "icon-on-light.svg", app_sizes, assets / "icon" / "app-on-light.ico", tmp)
        for name in ("tray-white", "tray-black"):
            build_ico(assets / "tray" / f"{name}.svg", tray_sizes, assets / "tray" / f"{name}.ico", tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("rendered PNG + ICO files")


if __name__ == "__main__":
    main()
