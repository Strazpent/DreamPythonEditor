# SPDX-License-Identifier: Apache-2.0
"""(icons) PO-06 F6: the editor's own icon set ("dream" set), authored here as geometry and written as SVG.

    python dream_icons.py write              writes Resources/Icons/*.svg (the files Slate loads)
    python dream_icons.py render [--out D]   reads those SVGs back with the small rasterizer below and writes
                                             Resources/Icon128.png plus preview sheets (default: <plugin>/Intermediate/Icons)
    python dream_icons.py all                both

Style rules (posted on the board; Shell/DreamIcons.h repeats them):
  * 16 x 16 grid (viewBox 0 0 16 16), art inside 1..15, straight edges on whole or half pixels.
  * Monochrome white glyphs, tinted at paint by the caller (FDreamIcons::TintColor), so every icon follows all ten themes.
    Two tones only: the glyph (opacity 1) and the "dream layer" (fill-opacity 0.45) for backs, pages and echoes.
  * Soft and rounded: every corner rounded (r >= 0.8), strokes 1.5 with round caps and joins.
  * Motifs: crescent moon (brand, paused), four-point sparkle (run, samples, lightbulb, success, recovered), cloud
    (scratches). One motif per icon at most, never at the cost of the 16 px silhouette.
  * The brand mark is the only colour art (brand.svg, brand_tile.svg); everything else is white.
  * No text or letters in any icon.

Only a small SVG subset is written and read: path (absolute M L C Q A Z), circle, rect; fill, fill-opacity, fill-rule
evenodd, stroke, stroke-width, stroke-opacity, round caps and joins; one linearGradient (objectBoundingBox) for the tile.
That is also what Slate's nanosvg reads. The rasterizer needs Pillow and NumPy (outside the engine's Python).
"""
import math
import os
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(os.path.dirname(HERE))
ICON_DIR = os.path.join(PLUGIN, "Resources", "Icons")
SEC = 0.45   # the dream layer's opacity


# ---- geometry helpers ------------------------------------------------------------------------------------------------

def n(v):
    s = ("%.3f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def pt(p):
    return "%s %s" % (n(p[0]), n(p[1]))


def rrect(x, y, w, h, r):
    r = min(r, w / 2.0, h / 2.0)
    return ("M%s L%s A%s %s 0 0 1 %s L%s A%s %s 0 0 1 %s L%s A%s %s 0 0 1 %s L%s A%s %s 0 0 1 %s Z" % (
        pt((x + r, y)), pt((x + w - r, y)), n(r), n(r), pt((x + w, y + r)), pt((x + w, y + h - r)), n(r), n(r),
        pt((x + w - r, y + h)), pt((x + r, y + h)), n(r), n(r), pt((x, y + h - r)), pt((x, y + r)), n(r), n(r), pt((x + r, y))))


def circle_path(cx, cy, r, ccw=False):
    s = 0 if ccw else 1
    return "M%s A%s %s 0 1 %d %s A%s %s 0 1 %d %s Z" % (pt((cx - r, cy)), n(r), n(r), s, pt((cx + r, cy)), n(r), n(r), s, pt((cx - r, cy)))


def rpoly(points, r):
    """A closed polygon whose corners are rounded with quadratic curves (the corner is the control point)."""
    out = []
    count = len(points)
    for i in range(count):
        p0, p1, p2 = points[i - 1], points[i], points[(i + 1) % count]
        rr = r[i] if isinstance(r, (list, tuple)) else r
        d0 = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
        d2 = math.hypot(p2[0] - p1[0], p2[1] - p1[1])
        k0 = min(rr, d0 / 2.0) / d0 if d0 else 0
        k2 = min(rr, d2 / 2.0) / d2 if d2 else 0
        a = (p1[0] + (p0[0] - p1[0]) * k0, p1[1] + (p0[1] - p1[1]) * k0)
        b = (p1[0] + (p2[0] - p1[0]) * k2, p1[1] + (p2[1] - p1[1]) * k2)
        out.append(("L" if out else "M") + pt(a))
        out.append("Q%s %s" % (pt(p1), pt(b)))
    return " ".join(out) + " Z"


def rotate(points, angle_deg, cx, cy):
    a = math.radians(angle_deg)
    c, s = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in points]


def sparkle(cx, cy, r, k=0.18):
    """The four-point sparkle: concave sides, soft centre."""
    q = r * k
    return "M%s Q%s %s Q%s %s Q%s %s Q%s %s Z" % (
        pt((cx, cy - r)), pt((cx + q, cy - q)), pt((cx + r, cy)), pt((cx + q, cy + q)), pt((cx, cy + r)),
        pt((cx - q, cy + q)), pt((cx - r, cy)), pt((cx - q, cy - q)), pt((cx, cy - r)))


def _arc_flags(c, r, a, b, mid_ok):
    """Flags (large, sweep) of the arc of circle (c, r) from a to b whose midpoint satisfies mid_ok."""
    ta = math.atan2(a[1] - c[1], a[0] - c[0])
    tb = math.atan2(b[1] - c[1], b[0] - c[0])
    for sweep in (1, 0):
        span = (tb - ta) % (2 * math.pi) if sweep else -((ta - tb) % (2 * math.pi))
        tm = ta + span / 2.0
        m = (c[0] + r * math.cos(tm), c[1] + r * math.sin(tm))
        if mid_ok(m):
            return (1 if abs(span) > math.pi else 0), sweep
    return 0, 1


def crescent(cx, cy, R, ox, oy, r):
    """Disc (cx, cy, R) minus disc (ox, oy, r), as one closed outline (the discs must intersect)."""
    dx, dy = ox - cx, oy - cy
    d = math.hypot(dx, dy)
    a = (R * R - r * r + d * d) / (2 * d)
    h = math.sqrt(max(R * R - a * a, 0.0))
    mx, my = cx + a * dx / d, cy + a * dy / d
    p1 = (mx + h * dy / d, my - h * dx / d)
    p2 = (mx - h * dy / d, my + h * dx / d)
    outside_cut = lambda m: math.hypot(m[0] - ox, m[1] - oy) > r
    inside_disc = lambda m: math.hypot(m[0] - cx, m[1] - cy) < R
    l1, s1 = _arc_flags((cx, cy), R, p1, p2, outside_cut)
    l2, s2 = _arc_flags((ox, oy), r, p2, p1, inside_disc)
    return "M%s A%s %s 0 %d %d %s A%s %s 0 %d %d %s Z" % (pt(p1), n(R), n(R), l1, s1, pt(p2), n(r), n(r), l2, s2, pt(p1))


def thick_polyline(points, w):
    """Outline of a polyline of width w with mitred joins and square ends (for cut-outs: a check, a cross)."""
    h = w / 2.0
    left, right = [], []
    count = len(points)
    for i in range(count):
        if i == 0:
            d = (points[1][0] - points[0][0], points[1][1] - points[0][1])
            L = math.hypot(*d)
            nx, ny = -d[1] / L, d[0] / L
            sc = h
        elif i == count - 1:
            d = (points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1])
            L = math.hypot(*d)
            nx, ny = -d[1] / L, d[0] / L
            sc = h
        else:
            d0 = (points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1])
            d1 = (points[i + 1][0] - points[i][0], points[i + 1][1] - points[i][1])
            L0, L1 = math.hypot(*d0), math.hypot(*d1)
            n0 = (-d0[1] / L0, d0[0] / L0)
            n1 = (-d1[1] / L1, d1[0] / L1)
            nx, ny = n0[0] + n1[0], n0[1] + n1[1]
            Ln = math.hypot(nx, ny)
            nx, ny = nx / Ln, ny / Ln
            sc = h / max(nx * n0[0] + ny * n0[1], 0.3)
        left.append((points[i][0] + nx * sc, points[i][1] + ny * sc))
        right.append((points[i][0] - nx * sc, points[i][1] - ny * sc))
    return left + right[::-1]


def poly(points):
    return "M" + " L".join(pt(p) for p in points) + " Z"


# ---- SVG elements ----------------------------------------------------------------------------------------------------

def fill(d, opacity=1.0, color="#FFFFFF"):
    op = "" if opacity >= 1.0 else ' fill-opacity="%s"' % n(opacity)
    return '<path fill="%s"%s fill-rule="evenodd" d="%s"/>' % (color, op, d)


def stroke(d, width=1.5, opacity=1.0, color="#FFFFFF"):
    op = "" if opacity >= 1.0 else ' stroke-opacity="%s"' % n(opacity)
    return ('<path fill="none" stroke="%s"%s stroke-width="%s" stroke-linecap="round" stroke-linejoin="round" d="%s"/>'
            % (color, op, n(width), d))


def svg(body, size=16, defs=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s\n%s\n</svg>\n'
            % (size, size, size, size, ("\n<defs>%s</defs>" % defs) if defs else "", "\n".join(body)))


# ---- the set ---------------------------------------------------------------------------------------------------------

def curl_path(ox=0.0, oy=0.0, s=1.0):
    """The brand's little python curl: a tail that rises and coils once (the snake-ish wisp)."""
    P = lambda x, y: (ox + x * s, oy + y * s)
    return "M%s C%s %s %s C%s %s %s C%s %s %s" % (
        pt(P(0, 6)), pt(P(2.6, 6.2)), pt(P(4.6, 4.6)), pt(P(4.4, 2.4)),
        pt(P(4.25, 0.6)), pt(P(2.2, 0.0)), pt(P(1.5, 1.4)),
        pt(P(1.0, 2.5)), pt(P(2.1, 3.4)), pt(P(3.0, 2.6)))


def icon_brand_mono():
    return [fill(crescent(7.0, 8.6, 6.0, 10.4, 6.6, 4.9)),
            stroke(curl_path(7.6, 7.2, 1.0), 1.5),
            fill(sparkle(12.6, 3.0, 2.3))]


def icon_brand(colored=True):
    # Review q1 nit: the sparkle is lavender, not white, so it still reads on a light theme.
    moon, curl, star = ("#F5C04A", "#45C29C", "#A98BEA") if colored else ("#FFFFFF",) * 3
    return [fill(crescent(7.0, 8.6, 6.0, 10.4, 6.6, 4.9), color=moon),
            stroke(curl_path(7.6, 7.2, 1.0), 1.5, color=curl),
            fill(sparkle(12.6, 3.0, 2.3), color=star)]


def icon_brand_tile():
    # 128 px tile: a night-sky rounded square, the crescent, the curl, three stars and a soft cloud.
    defs = ('<linearGradient id="sky" x1="0" y1="0" x2="0.35" y2="1">'
            '<stop offset="0" stop-color="#3B2F7A"/><stop offset="0.6" stop-color="#6A4BB0"/><stop offset="1" stop-color="#B07CC9"/>'
            '</linearGradient>')
    body = ['<path fill="url(#sky)" d="%s"/>' % rrect(4, 4, 120, 120, 26),
            fill(cloud_path(88, 100, 3.6), 0.3),
            fill(crescent(56, 66, 38, 78, 52, 31), color="#FFD978"),
            stroke(curl_path(60, 56, 7.2), 9.5, color="#8FE3C8"),
            fill(sparkle(98, 26, 13), color="#FFFFFF"),
            fill(sparkle(26, 98, 6), color="#FFFFFF", opacity=0.8),
            fill(circle_path(26, 24, 3.2), color="#FFFFFF", opacity=0.85),
            fill(circle_path(110, 66, 2.6), color="#FFFFFF", opacity=0.7)]
    return body, defs


def cloud_path(cx, cy, s=1.0):
    """A cloud of three bumps on a flat base, centred near (cx, cy); s = 1 is about 13 x 8 px."""
    P = lambda x, y: (cx + x * s, cy + y * s)
    return ("M%s L%s A%s %s 0 0 0 %s A%s %s 0 0 0 %s A%s %s 0 0 0 %s A%s %s 0 0 0 %s Z" % (
        pt(P(-4.0, 3.5)), pt(P(3.8, 3.5)),
        n(2.6 * s), n(2.6 * s), pt(P(4.4, -1.6)),
        n(3.6 * s), n(3.6 * s), pt(P(-1.4, -2.6)),
        n(2.4 * s), n(2.4 * s), pt(P(-4.9, -0.9)),
        n(2.2 * s), n(2.2 * s), pt(P(-4.0, 3.5))))


def folder_back():
    return rpoly([(1.5, 2.5), (5.6, 2.5), (7.1, 4.0), (13.0, 4.0), (13.0, 12.0), (1.5, 12.0)], 1.2)


def icon_folder_closed():
    return [fill(folder_back(), SEC), fill(rrect(1.5, 5.5, 13.0, 8.0, 1.5))]


def icon_folder_open():
    return [fill(folder_back(), SEC), fill(rpoly([(4.0, 7.0), (15.0, 7.0), (12.6, 13.5), (1.5, 13.5)], 1.1))]


def page(cut=True):
    # A page with a folded corner: the body on the dream layer, the fold as the glyph.
    body = rpoly([(3.0, 1.5), (9.0, 1.5), (13.0, 5.5), (13.0, 14.5), (3.0, 14.5)], [1.3, 0.01, 0.01, 1.3, 1.3])
    fold = rpoly([(9.0, 1.5), (13.0, 5.5), (9.8, 5.5), (9.0, 4.7)], [0.4, 0.4, 0.8, 0.8])
    return body, fold


def icon_file():
    body, fold = page()
    return [fill(body, SEC), fill(fold), fill(rrect(5.0, 8.0, 6.0, 1.5, 0.75)), fill(rrect(5.0, 11.0, 4.0, 1.5, 0.75))]


def icon_file_python():
    body, fold = page()
    return [fill(body, SEC), fill(fold), stroke(curl_path(5.2, 6.6, 0.95), 1.5)]


def icon_scratches():
    return [fill(cloud_path(8.0, 8.0, 1.0)),
            ]


def star5(cx, cy, R, r, rot=-90):
    pts = []
    for i in range(10):
        a = math.radians(rot + i * 36)
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    return pts


def icon_samples():
    return [fill(rpoly(star5(7.2, 8.8, 6.4, 3.0), 0.9)), fill(sparkle(13.2, 3.0, 2.2))]


def icon_project_plugins():
    return [fill(rrect(5.5, 1.5, 1.6, 4.5, 0.8)), fill(rrect(8.9, 1.5, 1.6, 4.5, 0.8)),
            fill(rpoly([(3.5, 5.5), (12.5, 5.5), (12.5, 8.5), (9.5, 11.5), (6.5, 11.5), (3.5, 8.5)], 1.6)),
            stroke("M8 11.5 L8 13 Q8 14.5 9.5 14.5 L12.5 14.5", 1.5, SEC)]


def gear(cx, cy, R, r, teeth, hole):
    pts = []
    for i in range(teeth):
        base = 2 * math.pi * i / teeth
        w = math.pi / teeth
        for a, rad in ((base - w * 0.55, r), (base - w * 0.32, R), (base + w * 0.32, R), (base + w * 0.55, r)):
            pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    return rpoly(pts, 0.5) + " " + circle_path(cx, cy, hole)


def icon_engine():
    return [fill(gear(8, 8, 6.6, 4.9, 8, 2.1))]


def lock_parts():
    body = rrect(2.5, 7.0, 11.0, 7.5, 2.0) + " " + rrect(7.25, 9.25, 1.5, 3.0, 0.75)
    shackle = "M5 7.2 L5 5.5 A3 3 0 0 1 11 5.5 L11 7.2"
    return [fill(body), stroke(shackle, 1.6)]


def icon_save():
    outer = rpoly([(2.0, 2.0), (11.6, 2.0), (14.0, 4.4), (14.0, 14.0), (2.0, 14.0)], [2.0, 1.0, 1.0, 2.0, 2.0])
    label = rrect(4.5, 8.5, 7.0, 3.5, 1.0)
    shutter = rrect(5.0, 2.0, 5.0, 3.6, 0.9)
    return [fill(outer + " " + label + " " + shutter), fill(sparkle(8.0, 10.25, 1.6), SEC)]


def icon_run():
    return [fill(rpoly([(3.2, 2.8), (13.0, 8.6), (3.2, 14.2)], [1.6, 1.4, 1.6])), fill(sparkle(13.0, 2.9, 2.2))]


def icon_run_line():
    return [fill(rpoly([(1.5, 4.8), (7.2, 8.0), (1.5, 11.2)], 1.0)),
            fill(rrect(8.5, 2.5, 6.0, 1.6, 0.8), SEC), fill(rrect(8.5, 7.2, 6.0, 1.6, 0.8)), fill(rrect(8.5, 11.9, 4.5, 1.6, 0.8), SEC)]


def icon_stop():
    return [fill(rrect(3.0, 3.0, 10.0, 10.0, 2.6))]


def icon_clear():
    # An eraser leaning right, its tip on the dream layer, two crumbs.
    sleeve = rotate([(4.0, 5.5), (9.0, 5.5), (9.0, 10.5), (4.0, 10.5)], -45, 8.0, 8.0)
    tip = rotate([(9.0, 5.5), (13.0, 5.5), (13.0, 10.5), (9.0, 10.5)], -45, 8.0, 8.0)
    sh = lambda ps: [(x - 0.5, y - 1.2) for x, y in ps]
    return [fill(rpoly(sh(sleeve), [1.2, 0.3, 0.3, 1.2])), fill(rpoly(sh(tip), [0.3, 1.2, 1.2, 0.3]), SEC),
            fill(rrect(7.0, 13.0, 7.5, 1.5, 0.75))]


def icon_undo_run():
    arc = "M4.2 6.6 A5 5 0 1 1 6.2 12.9"
    head = rpoly([(1.6, 3.6), (6.6, 4.6), (3.0, 8.2)], 0.6)
    return [stroke(arc, 1.6), fill(head), fill(rpoly([(7.6, 7.0), (10.6, 8.9), (7.6, 10.8)], 0.6), SEC)]


def icon_echo():
    return [stroke("M2 4.5 L5.5 8 L2 11.5", 1.6), fill(rrect(6.5, 10.6, 3.6, 1.6, 0.8)),
            stroke("M9.8 4.4 A4.2 4.2 0 0 1 9.8 9.2", 1.4, SEC), stroke("M12 2.5 A6.6 6.6 0 0 1 12 11.1", 1.4, SEC)]


def icon_line_numbers():
    return [fill(rrect(1.5, 2.5, 3.0, 1.6, 0.8)), fill(rrect(1.5, 7.2, 3.0, 1.6, 0.8)), fill(rrect(1.5, 11.9, 3.0, 1.6, 0.8)),
            fill(rrect(6.0, 1.5, 1.4, 13.0, 0.7)),
            fill(rrect(9.0, 2.5, 5.5, 1.6, 0.8), SEC), fill(rrect(9.0, 7.2, 4.0, 1.6, 0.8), SEC), fill(rrect(9.0, 11.9, 5.0, 1.6, 0.8), SEC)]


def icon_chevron_down():
    return [stroke("M4 6.2 L8 10.2 L12 6.2", 1.8)]


def icon_close():
    return [stroke("M4.5 4.5 L11.5 11.5 M11.5 4.5 L4.5 11.5", 1.6)]


def icon_plus():
    return [stroke("M8 3.5 L8 12.5 M3.5 8 L12.5 8", 1.6)]


def icon_recovered():
    arc = "M12.6 9.6 A5 5 0 1 1 11.4 4.4"
    head = rpoly([(13.6, 1.8), (13.4, 6.6), (9.0, 5.0)], 0.6)
    return [stroke(arc, 1.6), fill(head), fill(sparkle(8.0, 8.6, 2.4), SEC)]


# Structure kinds
def iso_cube(cx, cy, s):
    top = [(cx, cy - s), (cx + s * 0.95, cy - s * 0.5), (cx, cy), (cx - s * 0.95, cy - s * 0.5)]
    left = [(cx - s * 0.95, cy - s * 0.5 + 0.0), (cx, cy), (cx, cy + s), (cx - s * 0.95, cy + s * 0.5)]
    right = [(cx, cy), (cx + s * 0.95, cy - s * 0.5), (cx + s * 0.95, cy + s * 0.5), (cx, cy + s)]
    return top, left, right


def icon_class():
    top, left, right = iso_cube(8.0, 8.4, 6.3)
    gap = 0.45
    left = [(x - gap * 0.7, y + gap * 0.4) for x, y in left]
    right = [(x + gap * 0.7, y + gap * 0.4) for x, y in right]
    top = [(x, y - gap * 0.6) for x, y in top]
    return [fill(rpoly(top, 0.8), SEC), fill(rpoly(left, 0.8)), fill(rpoly(right, 0.8))]


def diamond(cx, cy, r):
    return [(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)]


def icon_function():
    return [fill(rpoly(diamond(8.0, 8.0, 6.5), 1.6))]


def icon_method():
    return [fill(rpoly(diamond(8.0, 8.0, 6.5), 1.6) + " " + circle_path(8.0, 8.0, 2.0))]


def icon_async():
    return [fill(rpoly(diamond(5.6, 8.0, 4.6), 1.2), SEC), fill(rpoly(diamond(10.0, 8.0, 5.2), 1.3))]


def icon_cell():
    return [fill(rrect(1.5, 2.0, 13.0, 1.6, 0.8), SEC), fill(rrect(1.5, 5.4, 13.0, 5.2, 1.4)), fill(rrect(1.5, 12.4, 13.0, 1.6, 0.8), SEC)]


# Severities
def x_cross(cx, cy, arm, w):
    h = w / 2.0
    plus = [(cx - h, cy - arm), (cx + h, cy - arm), (cx + h, cy - h), (cx + arm, cy - h), (cx + arm, cy + h), (cx + h, cy + h),
            (cx + h, cy + arm), (cx - h, cy + arm), (cx - h, cy + h), (cx - arm, cy + h), (cx - arm, cy - h), (cx - h, cy - h)]
    return rpoly(rotate(plus, 45, cx, cy), [0.6, 0.6, 0.01, 0.6, 0.6, 0.01, 0.6, 0.6, 0.01, 0.6, 0.6, 0.01])


def octagon(cx, cy, r):
    return [(cx + r * math.cos(math.radians(22.5 + 45 * i)), cy + r * math.sin(math.radians(22.5 + 45 * i))) for i in range(8)]


def icon_error():
    # A rounded octagon (severities differ by shape too: octagon, triangle, circle) with a cut-out cross.
    return [fill(rpoly(octagon(8, 8, 7.2), 1.3) + " " + x_cross(8, 8, 3.6, 1.45))]


def icon_warning():
    tri = rpoly([(8.0, 1.6), (14.8, 13.8), (1.2, 13.8)], 1.8)
    bar = rrect(7.2, 5.6, 1.6, 4.6, 0.8)
    dot = circle_path(8.0, 11.9, 0.95)
    return [fill(tri + " " + bar + " " + dot)]


def icon_info():
    return [fill(circle_path(8, 8, 6.8) + " " + rrect(7.2, 7.0, 1.6, 4.8, 0.8) + " " + circle_path(8, 4.9, 1.0))]


def icon_lightbulb():
    bulb = ("M8 1.8 A4.6 4.6 0 0 1 10.9 10 Q10.4 10.5 10.3 11.2 L5.7 11.2 Q5.6 10.5 5.1 10 A4.6 4.6 0 0 1 8 1.8 Z")
    return [fill(bulb), fill(rrect(5.8, 12.2, 4.4, 1.3, 0.65), SEC), fill(rrect(6.4, 14.0, 3.2, 1.1, 0.55), SEC),
            fill(sparkle(13.6, 2.6, 1.9))]


def icon_success():
    check = thick_polyline([(4.6, 8.2), (7.0, 10.6), (11.5, 5.8)], 1.8)
    return [fill(circle_path(8, 8, 6.8) + " " + rpoly(check, 0.4))]


def icon_notify():
    bell = ("M8 2 C10.9 2 11.8 4.4 11.8 6.6 L11.8 9.2 Q11.8 10.4 13.2 11.4 L13.2 12.2 L2.8 12.2 L2.8 11.4 Q4.2 10.4 4.2 9.2 "
            "L4.2 6.6 C4.2 4.4 5.1 2 8 2 Z")
    return [fill(bell), fill("M6.3 13.2 L9.7 13.2 A1.7 1.7 0 0 1 6.3 13.2 Z", SEC), fill(sparkle(13.6, 2.6, 1.9))]


# Debugger
def icon_breakpoint():
    return [fill(circle_path(8, 8, 5.2))]


def icon_breakpoint_disabled():
    return [fill(circle_path(8, 8, 5.2) + " " + circle_path(8, 8, 3.6))]


def icon_current_line():
    return [fill(rpoly([(1.8, 4.6), (9.0, 4.6), (14.4, 8.0), (9.0, 11.4), (1.8, 11.4)], [1.0, 0.8, 1.2, 0.8, 1.0]))]


def icon_continue():
    return [fill(rrect(2.2, 3.0, 2.4, 10.0, 1.0)), fill(rpoly([(6.6, 3.0), (14.2, 8.0), (6.6, 13.0)], 1.3))]


def icon_step_over():
    return [stroke("M2.6 9.6 A5.6 5.6 0 0 1 12.6 6.8", 1.6), fill(rpoly([(14.4, 4.0), (14.0, 9.8), (9.6, 7.6)], 0.6)),
            fill(circle_path(8.0, 13.0, 1.7))]


def icon_step_into():
    return [stroke("M8 1.8 L8 7.2", 1.6), fill(rpoly([(4.8, 6.2), (11.2, 6.2), (8.0, 10.0)], 0.7)),
            fill(circle_path(8.0, 13.2, 1.7))]


def icon_step_out():
    return [stroke("M8 10 L8 5", 1.6), fill(rpoly([(4.8, 5.8), (11.2, 5.8), (8.0, 2.0)], 0.7)),
            fill(circle_path(8.0, 13.2, 1.7))]


def icon_debug_run():
    body = "M8 5.2 C11 5.2 11.6 7.8 11.6 9.6 C11.6 12.4 10 14.4 8 14.4 C6 14.4 4.4 12.4 4.4 9.6 C4.4 7.8 5 5.2 8 5.2 Z"
    head = circle_path(8.0, 3.6, 2.0)
    legs = "M4.4 8.4 L2.2 7.2 M4.3 10.8 L1.8 10.8 M4.8 13 L2.6 14.2 M11.6 8.4 L13.8 7.2 M11.7 10.8 L14.2 10.8 M11.2 13 L13.4 14.2"
    return [stroke(legs, 1.3, SEC), fill(head, SEC), fill(body + " " + rrect(7.4, 7.0, 1.2, 6.4, 0.6))]


def window_frame():
    return rrect(1.5, 2.0, 13.0, 12.0, 2.0) + " " + rrect(3.0, 5.0, 10.0, 7.5, 0.8)


def icon_run_window():
    return [fill(window_frame()), fill(rpoly([(6.4, 6.4), (10.4, 8.75), (6.4, 11.1)], 0.6))]


def icon_paused_window():
    # The run sleeps: a crescent with the pause bars beside it.
    return [fill(crescent(6.2, 8.0, 5.4, 9.0, 6.4, 4.4)), fill(rrect(10.0, 4.6, 1.8, 6.8, 0.9)), fill(rrect(13.0, 4.6, 1.8, 6.8, 0.9))]


ICONS = {
    # brand and tab
    "brand": lambda: icon_brand(True),
    "brand_mono": icon_brand_mono,
    # icon row
    "open": icon_folder_open,
    "save": icon_save,
    "run": icon_run,
    "run_line": icon_run_line,
    "stop": icon_stop,
    "clear": icon_clear,
    "undo_run": icon_undo_run,
    "echo": icon_echo,
    "line_numbers": icon_line_numbers,
    "chevron_down": icon_chevron_down,
    "close": icon_close,
    "plus": icon_plus,
    "recovered": icon_recovered,
    # tree
    "folder_closed": icon_folder_closed,
    "folder_open": icon_folder_open,
    "file_python": icon_file_python,
    "file": icon_file,
    "scratches": icon_scratches,
    "samples": icon_samples,
    "project_plugins": icon_project_plugins,
    "engine": icon_engine,
    "read_only": lock_parts,
    # structure
    "class": icon_class,
    "function": icon_function,
    "method": icon_method,
    "async": icon_async,
    "cell": icon_cell,
    # severities
    "error": icon_error,
    "warning": icon_warning,
    "info": icon_info,
    "lightbulb": icon_lightbulb,
    # debugger
    "breakpoint": icon_breakpoint,
    "breakpoint_disabled": icon_breakpoint_disabled,
    "current_line": icon_current_line,
    "continue": icon_continue,
    "step_over": icon_step_over,
    "step_into": icon_step_into,
    "step_out": icon_step_out,
    "debug_run": icon_debug_run,
    # windows
    "run_window": icon_run_window,
    "paused_window": icon_paused_window,
    # notifications
    "notify": icon_notify,
    "success": icon_success,
}


def write_all():
    os.makedirs(ICON_DIR, exist_ok=True)
    for name, make in ICONS.items():
        with open(os.path.join(ICON_DIR, name + ".svg"), "w", encoding="ascii", newline="\n") as handle:
            handle.write(svg(make()))
    body, defs = icon_brand_tile()
    with open(os.path.join(ICON_DIR, "brand_tile.svg"), "w", encoding="ascii", newline="\n") as handle:
        handle.write(svg(body, 128, defs))
    print("wrote %d SVGs to %s" % (len(ICONS) + 1, ICON_DIR))


# ---- the rasterizer (the subset above) -------------------------------------------------------------------------------

def _tokens(d):
    import re
    return re.findall(r"[MLCQAZ]|-?(?:\d+\.?\d*|\.\d+)", d)


def _arc_points(p0, rx, ry, large, sweep, p1, steps_per_rad=10):
    # SVG F.6.5 endpoint to centre (rx == ry here, no rotation)
    x1, y1 = p0
    x2, y2 = p1
    if rx == 0 or (x1 == x2 and y1 == y2):
        return [p1]
    dx, dy = (x1 - x2) / 2.0, (y1 - y2) / 2.0
    lam = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry)
    if lam > 1:
        rx *= math.sqrt(lam)
        ry *= math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * dy * dy - ry * ry * dx * dx
    den = rx * rx * dy * dy + ry * ry * dx * dx
    co = math.sqrt(max(num / den, 0.0)) if den else 0.0
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * dy / ry, -co * ry * dx / rx
    cx, cy = cxp + (x1 + x2) / 2.0, cyp + (y1 + y2) / 2.0
    t1 = math.atan2((dy - cyp) / ry, (dx - cxp) / rx)
    t2 = math.atan2((-dy - cyp) / ry, (-dx - cxp) / rx)
    dt = t2 - t1
    if sweep and dt < 0:
        dt += 2 * math.pi
    elif not sweep and dt > 0:
        dt -= 2 * math.pi
    steps = max(4, int(abs(dt) * steps_per_rad))
    return [(cx + rx * math.cos(t1 + dt * i / steps), cy + ry * math.sin(t1 + dt * i / steps)) for i in range(1, steps + 1)]


def parse_path(d):
    """Subpaths as (points, closed)."""
    tk = _tokens(d)
    i = 0
    subs, cur, closed = [], [], False
    pos = (0.0, 0.0)
    num = lambda k: float(tk[k])
    while i < len(tk):
        c = tk[i]
        i += 1
        if c == "M":
            if cur:
                subs.append((cur, closed))
            pos = (num(i), num(i + 1))
            i += 2
            cur, closed = [pos], False
        elif c == "L":
            pos = (num(i), num(i + 1))
            i += 2
            cur.append(pos)
        elif c == "Q":
            c1, p = (num(i), num(i + 1)), (num(i + 2), num(i + 3))
            i += 4
            for s in range(1, 13):
                t = s / 12.0
                cur.append(((1 - t) ** 2 * pos[0] + 2 * (1 - t) * t * c1[0] + t * t * p[0],
                            (1 - t) ** 2 * pos[1] + 2 * (1 - t) * t * c1[1] + t * t * p[1]))
            pos = p
        elif c == "C":
            c1, c2, p = (num(i), num(i + 1)), (num(i + 2), num(i + 3)), (num(i + 4), num(i + 5))
            i += 6
            for s in range(1, 21):
                t = s / 20.0
                a, b, cc, dd = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
                cur.append((a * pos[0] + b * c1[0] + cc * c2[0] + dd * p[0], a * pos[1] + b * c1[1] + cc * c2[1] + dd * p[1]))
            pos = p
        elif c == "A":
            rx, ry, _rot, large, sweep, x, y = (num(i + k) for k in range(7))
            i += 7
            cur.extend(_arc_points(pos, rx, ry, int(large), int(sweep), (x, y)))
            pos = (x, y)
        elif c == "Z":
            closed = True
            if cur:
                subs.append((cur, True))
                pos = cur[0]
            cur, closed = [], False
        else:
            raise ValueError("unsupported path token %r" % c)
    if cur:
        subs.append((cur, closed))
    return subs


def _color(text):
    text = text.strip()
    if text.startswith("#") and len(text) == 7:
        return tuple(int(text[k:k + 2], 16) / 255.0 for k in (1, 3, 5))
    raise ValueError("colour %r" % text)


def render_svg(path, size, ss=None):
    """RGBA float array (straight alpha) of an SVG file at size x size pixels."""
    import numpy as np
    from PIL import Image, ImageDraw
    tree = ET.parse(path)
    root = tree.getroot()
    ns = "{http://www.w3.org/2000/svg}"
    vb = [float(v) for v in root.get("viewBox").split()]
    ss = ss or max(4, int(math.ceil(256.0 / size)))
    S = size * ss
    k = S / vb[2]
    acc = np.zeros((S, S, 4), dtype=np.float64)   # premultiplied
    grads = {}
    for g in root.iter(ns + "linearGradient"):
        stops = [(float(s.get("offset")), _color(s.get("stop-color"))) for s in g.findall(ns + "stop")]
        grads[g.get("id")] = (tuple(float(g.get(a, dflt)) for a, dflt in (("x1", 0), ("y1", 0), ("x2", 1), ("y2", 0))), stops)

    def shape_subpaths(el):
        tag = el.tag[len(ns):]
        if tag == "path":
            return parse_path(el.get("d"))
        if tag == "circle":
            return parse_path(circle_path(float(el.get("cx")), float(el.get("cy")), float(el.get("r"))))
        if tag == "rect":
            return parse_path(rrect(float(el.get("x", 0)), float(el.get("y", 0)), float(el.get("width")), float(el.get("height")), float(el.get("rx", 0))))
        return None

    def composite(mask, rgb_or_img, opacity):
        a = mask * opacity
        if isinstance(rgb_or_img, tuple):
            col = np.array(rgb_or_img, dtype=np.float64)[None, None, :]
        else:
            col = rgb_or_img
        acc[..., :3] = col * a[..., None] + acc[..., :3] * (1 - a[..., None])
        acc[..., 3] = a + acc[..., 3] * (1 - a)

    for el in root.iter():
        subs = shape_subpaths(el)
        if subs is None:
            continue
        fill_attr = el.get("fill", "#000000")
        if fill_attr != "none":
            mask = np.zeros((S, S), dtype=bool)
            for pts, _closed in subs:
                if len(pts) < 3:
                    continue
                im = Image.new("1", (S, S), 0)
                ImageDraw.Draw(im).polygon([(x * k, y * k) for x, y in pts], fill=1)
                mask ^= np.array(im, dtype=bool)
            m = mask.astype(np.float64)
            op = float(el.get("fill-opacity", 1.0))
            if fill_attr.startswith("url(#"):
                (x1, y1, x2, y2), stops = grads[fill_attr[5:-1]]
                xs = [x for pts, _ in subs for x, _y in pts]
                ys = [y for pts, _ in subs for _x, y in pts]
                bx0, by0, bx1, by1 = min(xs) * k, min(ys) * k, max(xs) * k, max(ys) * k
                yy, xx = np.mgrid[0:S, 0:S] + 0.5
                u = (xx - bx0) / (bx1 - bx0)
                v = (yy - by0) / (by1 - by0)
                gx, gy = x2 - x1, y2 - y1
                t = np.clip(((u - x1) * gx + (v - y1) * gy) / (gx * gx + gy * gy), 0, 1)
                img = np.zeros((S, S, 3))
                for ch in range(3):
                    img[..., ch] = np.interp(t, [s[0] for s in stops], [s[1][ch] for s in stops])
                composite(m, img, op)
            else:
                composite(m, _color(fill_attr), op)
        stroke_attr = el.get("stroke")
        if stroke_attr and stroke_attr != "none":
            w = float(el.get("stroke-width", 1.0)) * k
            im = Image.new("L", (S, S), 0)
            dr = ImageDraw.Draw(im)
            for pts, closed in subs:
                p = [(x * k, y * k) for x, y in pts] + ([(pts[0][0] * k, pts[0][1] * k)] if closed else [])
                dr.line(p, fill=255, width=max(1, int(round(w))), joint="curve")
                r = w / 2.0
                for (x, y) in p:   # round caps and joins
                    dr.ellipse([x - r, y - r, x + r, y + r], fill=255)
            composite(np.array(im, dtype=np.float64) / 255.0, _color(stroke_attr), float(el.get("stroke-opacity", 1.0)))

    small = acc.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))
    a = small[..., 3:4]
    rgb = np.where(a > 0, small[..., :3] / np.maximum(a, 1e-9), 0)
    return np.concatenate([rgb, a], axis=2)


def to_image(arr):
    import numpy as np
    from PIL import Image
    return Image.fromarray((np.clip(arr, 0, 1) * 255 + 0.5).astype(np.uint8))


# ---- previews (approximate theme colours; the editor's own capture is Dream.Visual.Icons) -------------------------------

THEMES = {
    # name: (bg, chrome, text, muted, accent, danger, warning, error, keyword, definition, builtin, comment, light)
    "Darcula": ("2B2B2B", "3C3F41", "BBBBBB", "8C8C8C", "4A88C7", "C75450", "BBB529", "FF6B68", "CC7832", "FFC66D", "8888C6", "808080", False),
    "Dracula": ("282A36", "191A21", "F8F8F2", "8D93B8", "BD93F9", "FF5555", "FFB86C", "FF5555", "FF79C6", "50FA7B", "8BE9FD", "6272A4", False),
    "Maya": ("2B2B2B", "444444", "C8C8C8", "9A9A9A", "5285A6", "F08080", "E5C07B", "F08080", "7EB6E8", "E2E2E2", "B9A0E0", "D08770", False),
    "Monokai": ("272822", "1E1F1C", "F8F8F2", "A59F85", "66D9EF", "F92672", "FD971F", "F92672", "F92672", "A6E22E", "66D9EF", "75715E", False),
    "One Dark": ("282C34", "21252B", "ABB2BF", "7F848E", "61AFEF", "E06C75", "D19A66", "E06C75", "C678DD", "61AFEF", "E5C07B", "5C6370", False),
    "Nord": ("2E3440", "3B4252", "D8DEE9", "A3ABBA", "88C0D0", "BF616A", "EBCB8B", "BF616A", "81A1C1", "88C0D0", "8FBCBB", "616E88", False),
    "Gruvbox Dark": ("282828", "3C3836", "EBDBB2", "A89984", "83A598", "FB4934", "FE8019", "FB4934", "FB4934", "FABD2F", "83A598", "928374", False),
    "Solarized Light": ("FDF6E3", "EEE8D5", "586E75", "839496", "268BD2", "DC322F", "CB4B16", "DC322F", "859900", "268BD2", "B58900", "93A1A1", True),
    "GitHub Light": ("FFFFFF", "F6F8FA", "24292F", "57606A", "0550AE", "CF222E", "9A6700", "CF222E", "CF222E", "8250DF", "953800", "6E7781", True),
    "Unreal": ("1A1A1A", "242424", "C0C0C0", "808080", "26BBFF", "E64D4D", "FFB800", "E64D4D", "569CD6", "DCDCAA", "4EC9B0", "57A64A", False),
}

# The dream accents (FDreamIcons::TintColor): (dark-theme value, light-theme value).
ACCENTS = {"Moon": ("E8C15A", "B07F12"), "Mint": ("4FC99A", "1F8A62"), "Lavender": ("A98BEA", "7552C8"),
           "Sky": ("5AAEEA", "1F6FB8"), "Rose": ("EA7A93", "C2405F")}

# Default tint of each icon (FDreamIcons::DefaultTint); "Brand" = drawn in its own colours.
TINTS = {
    "brand": "Brand", "brand_mono": "Text", "open": "Text", "save": "Text", "run": "Accent", "run_line": "Text", "stop": "Danger",
    "clear": "Text", "undo_run": "Text", "echo": "Text", "line_numbers": "Text", "chevron_down": "Muted", "close": "Muted",
    "plus": "Muted", "recovered": "Moon", "folder_closed": "Muted", "folder_open": "Muted", "file_python": "Sky", "file": "Muted",
    "scratches": "Lavender", "samples": "Moon", "project_plugins": "Text", "engine": "Muted", "read_only": "Muted",
    "class": "Keyword", "function": "Definition", "method": "Definition", "async": "Builtin", "cell": "Comment",
    "error": "Error", "warning": "Warning", "info": "Sky", "lightbulb": "Moon", "breakpoint": "Error",
    "breakpoint_disabled": "Muted", "current_line": "Moon", "continue": "Accent", "step_over": "Accent", "step_into": "Accent",
    "step_out": "Accent", "debug_run": "Mint", "run_window": "Accent", "paused_window": "Lavender", "notify": "Text",
    "success": "Mint",
}


def tint_rgb(theme, tint):
    bg, chrome, text, muted, accent, danger, warning, error, keyword, definition, builtin, comment, light = THEMES[theme]
    table = {"Text": text, "Muted": muted, "Accent": accent, "Danger": danger, "Warning": warning, "Error": error,
             "Keyword": keyword, "Definition": definition, "Builtin": builtin, "Comment": comment, "Brand": "FFFFFF"}
    if tint in ACCENTS:
        value = ACCENTS[tint][1 if light else 0]
    else:
        value = table[tint]
    return tuple(int(value[k:k + 2], 16) / 255.0 for k in (0, 2, 4))


def hexrgb(h):
    return tuple(int(h[k:k + 2], 16) for k in (0, 2, 4))


def sheet(names, themes, out, zoom=5, cols=8):
    """A grid per theme: each cell shows the icon zoomed (nearest), then at 16 and 20 px, tinted as FDreamIcons would."""
    import numpy as np
    from PIL import Image
    cw, ch = 16 * zoom + 12, 16 * zoom + 8 + 20 + 6 + 16 + 8
    rows = (len(names) + cols - 1) // cols
    W, Ht = cols * cw + 8, rows * ch + 8
    img = Image.new("RGB", (W, Ht * len(themes)), (0, 0, 0))
    cache = {}
    for ti, theme in enumerate(themes):
        oy = ti * Ht
        img.paste(Image.new("RGB", (W, Ht), hexrgb(THEMES[theme][0])), (0, oy))
        for i, name in enumerate(names):
            x0, y0 = 4 + (i % cols) * cw, oy + 4 + (i // cols) * ch
            tiles = {}
            for size in (16, 20):
                if (name, size) not in cache:
                    cache[(name, size)] = render_svg(os.path.join(ICON_DIR, name + ".svg"), size)
                arr = cache[(name, size)].copy()
                t = TINTS.get(name, "Text")
                if t != "Brand":
                    arr[..., :3] = np.array(tint_rgb(theme, t))[None, None, :]
                tiles[size] = to_image(arr)
            z = tiles[16].resize((16 * zoom, 16 * zoom), Image.NEAREST)
            img.paste(z, (x0 + 4, y0), z)
            img.paste(tiles[16], (x0 + 4, y0 + 16 * zoom + 8), tiles[16])
            img.paste(tiles[20], (x0 + 28, y0 + 16 * zoom + 6), tiles[20])
    img.save(out)
    print("wrote", out)


def render_all(out_dir):
    from PIL import Image
    os.makedirs(out_dir, exist_ok=True)
    tile = render_svg(os.path.join(ICON_DIR, "brand_tile.svg"), 128, ss=4)
    to_image(tile).save(os.path.join(PLUGIN, "Resources", "Icon128.png"))
    print("wrote", os.path.join(PLUGIN, "Resources", "Icon128.png"))
    names = list(ICONS.keys())
    sheet(names, ["Darcula"], os.path.join(out_dir, "preview-darcula.png"))
    sheet(names, ["GitHub Light"], os.path.join(out_dir, "preview-githublight.png"))
    to_image(tile).resize((256, 256), Image.LANCZOS).save(os.path.join(out_dir, "preview-tile.png"))


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "all"
    out = os.path.join(PLUGIN, "Intermediate", "Icons")
    if "--out" in argv:
        out = argv[argv.index("--out") + 1]
    if cmd in ("write", "all"):
        write_all()
    if cmd in ("render", "all"):
        render_all(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
